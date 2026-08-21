#include "plugins/network/S3Signer.h"

#include <algorithm>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace mole::net {
namespace {

    QByteArray toHex(const unsigned char* data, unsigned int length)
    {
        return QByteArray(reinterpret_cast<const char*>(data), static_cast<int>(length)).toHex();
    }

    QByteArray hmacSha256(const QByteArray& key, const QByteArray& data)
    {
        unsigned char output[EVP_MAX_MD_SIZE] = { 0 };
        unsigned int length = 0;
        HMAC(EVP_sha256(), key.constData(), key.size(),
            reinterpret_cast<const unsigned char*>(data.constData()), static_cast<size_t>(data.size()),
            output, &length);
        return QByteArray(reinterpret_cast<char*>(output), static_cast<int>(length));
    }

    /// The four-step key derivation from AWS's specification. Each step narrows
    /// the key: to the day, then the region, then the service, then to signing --
    /// which is what stops a captured signature from being reused anywhere else.
    QByteArray derivedSigningKey(const SigningIdentity& identity, const QByteArray& dateStamp)
    {
        const QByteArray initial = "AWS4" + identity.secretAccessKey.toUtf8();
        const QByteArray dateKey = hmacSha256(initial, dateStamp);
        const QByteArray regionKey = hmacSha256(dateKey, identity.region.toUtf8());
        const QByteArray serviceKey = hmacSha256(regionKey, identity.service.toUtf8());
        return hmacSha256(serviceKey, "aws4_request");
    }

    QByteArray amzDate(const QDateTime& timestamp)
    {
        return timestamp.toUTC().toString(QStringLiteral("yyyyMMddThhmmssZ")).toUtf8();
    }

    QByteArray dateStamp(const QDateTime& timestamp)
    {
        return timestamp.toUTC().toString(QStringLiteral("yyyyMMdd")).toUtf8();
    }

    /// The headers that will actually be sent, which is what has to be signed.
    ///
    /// `x-amz-date` and `x-amz-content-sha256` are added here rather than left to
    /// the caller, because the signer is what produces them: leaving it to the
    /// caller means every call site has to remember to put them back into the
    /// signed set, and a request that sends them unsigned is refused by S3 with
    /// "header must be included in signature". Already-present values win, so a
    /// caller that supplies its own is not duplicated.
    HeaderList effectiveHeaders(const SignableRequest& request);

    /// Headers as the signature sees them: names lowercased, values trimmed and
    /// internal runs of whitespace collapsed, sorted by name.
    HeaderList normalisedHeaders(const HeaderList& headers)
    {
        HeaderList out;
        out.reserve(headers.size());
        for (const auto& header : headers) {
            QByteArray value = header.second.trimmed();
            QByteArray collapsed;
            collapsed.reserve(value.size());
            bool inSpace = false;
            for (const char character : value) {
                const bool isSpace = character == ' ' || character == '\t';
                if (isSpace) {
                    inSpace = true;
                    continue;
                }
                if (inSpace && !collapsed.isEmpty())
                    collapsed.append(' ');
                inSpace = false;
                collapsed.append(character);
            }
            out.append({ header.first.toLower(), collapsed });
        }
        std::sort(out.begin(), out.end(),
            [](const QPair<QByteArray, QByteArray>& a, const QPair<QByteArray, QByteArray>& b) {
                return a.first < b.first;
            });
        return out;
    }

    HeaderList effectiveHeaders(const SignableRequest& request)
    {
        HeaderList headers = request.headers;
        const auto alreadyPresent = [&headers](const char* name) {
            const QByteArray wanted = QByteArray(name);
            for (const auto& header : headers) {
                if (header.first.toLower() == wanted)
                    return true;
            }
            return false;
        };

        // A presigned url signs the host and nothing else: the date and the
        // payload hash travel in the query string instead, and whoever is given
        // the url sends no headers of ours at all.
        if (request.presigned)
            return headers;

        if (!alreadyPresent("x-amz-date"))
            headers.append({ QByteArray("x-amz-date"), amzDate(request.timestamp) });
        if (!alreadyPresent("x-amz-content-sha256"))
            headers.append({ QByteArray("x-amz-content-sha256"), request.payloadSha256 });
        return headers;
    }

} // namespace

QByteArray sha256Hex(const QByteArray& data)
{
    unsigned char digest[SHA256_DIGEST_LENGTH] = { 0 };
    SHA256(
        reinterpret_cast<const unsigned char*>(data.constData()), static_cast<size_t>(data.size()), digest);
    return toHex(digest, SHA256_DIGEST_LENGTH);
}

QByteArray emptyPayloadSha256()
{
    return sha256Hex(QByteArray());
}

QByteArray sha256HexOfStream(QIODevice& stream)
{
    const qint64 startedAt = stream.pos();

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    EVP_DigestInit_ex(context, EVP_sha256(), nullptr);

    constexpr qint64 kChunk = 256 * 1024;
    QByteArray chunk;
    chunk.resize(kChunk);
    while (!stream.atEnd()) {
        const qint64 read = stream.read(chunk.data(), kChunk);
        if (read <= 0)
            break;
        EVP_DigestUpdate(context, chunk.constData(), static_cast<size_t>(read));
    }

    unsigned char digest[EVP_MAX_MD_SIZE] = { 0 };
    unsigned int length = 0;
    EVP_DigestFinal_ex(context, digest, &length);
    EVP_MD_CTX_free(context);

    stream.seek(startedAt);
    return toHex(digest, length);
}

QByteArray uriEncode(const QByteArray& text, bool keepSlashes)
{
    // Written out rather than delegated to QUrl: AWS specifies exactly this
    // unreserved set, and QUrl's idea of what needs escaping differs in the
    // places that would break a signature -- '~' and '*' among them.
    static const char* const unreserved
        = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";

    QByteArray out;
    out.reserve(text.size() * 3);
    for (const char character : text) {
        if (strchr(unreserved, character) != nullptr && character != '\0') {
            out.append(character);
        } else if (character == '/' && keepSlashes) {
            out.append('/');
        } else {
            out.append('%');
            out.append(QByteArray(1, character).toHex().toUpper());
        }
    }
    return out;
}

QByteArray canonicalQuery(const QList<QPair<QString, QString>>& parameters)
{
    QList<QPair<QByteArray, QByteArray>> encoded;
    encoded.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        encoded.append(
            { uriEncode(parameter.first.toUtf8(), false), uriEncode(parameter.second.toUtf8(), false) });
    }
    std::sort(encoded.begin(), encoded.end(),
        [](const QPair<QByteArray, QByteArray>& a, const QPair<QByteArray, QByteArray>& b) {
            return a.first != b.first ? a.first < b.first : a.second < b.second;
        });

    QByteArray out;
    for (const auto& parameter : encoded) {
        if (!out.isEmpty())
            out.append('&');
        out.append(parameter.first);
        out.append('=');
        out.append(parameter.second);
    }
    return out;
}

QByteArray canonicalPathFor(const SignableRequest& request)
{
    if (request.path.isEmpty())
        return "/";
    return uriEncode(request.path.toUtf8(), true);
}

QByteArray canonicalQueryFor(const SignableRequest& request)
{
    return canonicalQuery(request.queryParameters);
}

QByteArray canonicalRequestFor(const SignableRequest& request)
{
    const HeaderList headers = normalisedHeaders(effectiveHeaders(request));

    QByteArray canonicalHeaders;
    QByteArray signedHeaders;
    for (const auto& header : headers) {
        canonicalHeaders.append(header.first);
        canonicalHeaders.append(':');
        canonicalHeaders.append(header.second);
        canonicalHeaders.append('\n');
        if (!signedHeaders.isEmpty())
            signedHeaders.append(';');
        signedHeaders.append(header.first);
    }

    QByteArray out = request.method;
    out.append('\n');
    out.append(canonicalPathFor(request));
    out.append('\n');
    out.append(canonicalQueryFor(request));
    out.append('\n');
    out.append(canonicalHeaders);
    out.append('\n');
    out.append(signedHeaders);
    out.append('\n');
    out.append(request.payloadSha256);
    return out;
}

QByteArray stringToSignFor(
    const SignableRequest& request, const SigningIdentity& identity, const QByteArray& canonicalRequest)
{
    const QByteArray scope = dateStamp(request.timestamp) + '/' + identity.region.toUtf8() + '/'
        + identity.service.toUtf8() + "/aws4_request";

    QByteArray out = "AWS4-HMAC-SHA256\n";
    out.append(amzDate(request.timestamp));
    out.append('\n');
    out.append(scope);
    out.append('\n');
    out.append(sha256Hex(canonicalRequest));
    return out;
}

QString presignedUrl(const SignableRequest& request, const SigningIdentity& identity,
    std::chrono::seconds expiry, bool useHttps)
{
    QByteArray host;
    for (const auto& header : request.headers) {
        if (header.first.toLower() == "host")
            host = header.second;
    }

    SignableRequest signable = request;
    signable.presigned = true;
    signable.headers = { { QByteArray("host"), host } };
    // Not hashed, and it cannot be: there is no body, and the recipient cannot
    // send the header that would carry a hash of one.
    signable.payloadSha256 = "UNSIGNED-PAYLOAD";

    const QByteArray scope = dateStamp(signable.timestamp) + '/' + identity.region.toUtf8() + '/'
        + identity.service.toUtf8() + "/aws4_request";

    signable.queryParameters.append(
        { QStringLiteral("X-Amz-Algorithm"), QStringLiteral("AWS4-HMAC-SHA256") });
    signable.queryParameters.append({ QStringLiteral("X-Amz-Credential"),
        identity.accessKeyId + QLatin1Char('/') + QString::fromUtf8(scope) });
    signable.queryParameters.append(
        { QStringLiteral("X-Amz-Date"), QString::fromUtf8(amzDate(signable.timestamp)) });
    signable.queryParameters.append({ QStringLiteral("X-Amz-Expires"), QString::number(expiry.count()) });
    signable.queryParameters.append({ QStringLiteral("X-Amz-SignedHeaders"), QStringLiteral("host") });

    const QByteArray canonicalRequest = canonicalRequestFor(signable);
    const QByteArray stringToSign = stringToSignFor(signable, identity, canonicalRequest);
    const QByteArray signingKey = derivedSigningKey(identity, dateStamp(signable.timestamp));
    const QByteArray signature = hmacSha256(signingKey, stringToSign).toHex();

    // Built from the same canonical encoder that produced the signature, for the
    // reason the request path already gives: any other encoder is how a url comes
    // to be signed for one spelling and fetched under another.
    QByteArray url = (useHttps ? "https://" : "http://") + host;
    url += canonicalPathFor(signable);
    url += '?' + canonicalQueryFor(signable);
    // Last, and outside the canonical query, because it is a signature *of* that
    // query and cannot be part of what it signs.
    url += "&X-Amz-Signature=" + signature;
    return QString::fromUtf8(url);
}

HeaderList signWithSigV4(const SignableRequest& request, const SigningIdentity& identity)
{
    const QByteArray canonicalRequest = canonicalRequestFor(request);
    const QByteArray stringToSign = stringToSignFor(request, identity, canonicalRequest);
    const QByteArray signingKey = derivedSigningKey(identity, dateStamp(request.timestamp));
    const QByteArray signature = hmacSha256(signingKey, stringToSign).toHex();

    QByteArray signedHeaders;
    for (const auto& header : normalisedHeaders(effectiveHeaders(request))) {
        if (!signedHeaders.isEmpty())
            signedHeaders.append(';');
        signedHeaders.append(header.first);
    }

    const QByteArray scope = dateStamp(request.timestamp) + '/' + identity.region.toUtf8() + '/'
        + identity.service.toUtf8() + "/aws4_request";

    QByteArray authorization = "AWS4-HMAC-SHA256 Credential=";
    authorization.append(identity.accessKeyId.toUtf8());
    authorization.append('/');
    authorization.append(scope);
    authorization.append(", SignedHeaders=");
    authorization.append(signedHeaders);
    authorization.append(", Signature=");
    authorization.append(signature);

    return { { QByteArray("Authorization"), authorization },
        { QByteArray("x-amz-date"), amzDate(request.timestamp) },
        { QByteArray("x-amz-content-sha256"), request.payloadSha256 } };
}

} // namespace mole::net
