#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QIODevice>
#include <QList>
#include <QPair>
#include <QString>

namespace mole::net {

/// Also declared in CurlTransport.h. Repeating the alias is legal and keeps this
/// header a pure algorithm with no dependency on the transport.
using HeaderList = QList<QPair<QByteArray, QByteArray>>;

/// Who is signing, and for which region and service.
struct SigningIdentity
{
    QString accessKeyId;
    QString secretAccessKey;
    QString region;
    QString service = QStringLiteral("s3");
};

/// A request reduced to the parts a signature is computed over.
///
/// The path and query are held *unencoded* and encoded by the signer, which is
/// deliberate. A signature is computed over a canonical encoding, so an interface
/// that accepted a pre-encoded string would need every caller to encode and sort
/// identically -- and the first thing to get that wrong was this file's own test
/// harness. Now there is one encoder, used for both the signature and the url
/// that is actually sent, so the two cannot drift apart.
struct SignableRequest
{
    QByteArray method = "GET";
    /// Unencoded, starting with '/'.
    QString path = QStringLiteral("/");
    /// Unencoded name/value pairs, in any order.
    QList<QPair<QString, QString>> queryParameters;
    /// Must include `host`. Order does not matter; signing sorts them.
    HeaderList headers;
    /// Lowercase hex SHA-256 of the body. For an empty body that is the hash of
    /// the empty string, which S3 requires rather than allowing it to be omitted.
    QByteArray payloadSha256;
    /// The instant the request is signed. Passed in rather than read from the
    /// clock so a signature is reproducible and therefore testable.
    QDateTime timestamp;
};

/// Lowercase hex SHA-256 of `data`.
QByteArray sha256Hex(const QByteArray& data);

/// SHA-256 of an empty body, which every request without one has to send.
QByteArray emptyPayloadSha256();

/// Lowercase hex SHA-256 of everything in `stream`, read in chunks and rewound
/// afterwards so the caller can still send it.
///
/// S3 signs the payload hash, so an upload is necessarily read twice. Streaming
/// it is what keeps that from meaning "held in memory twice".
QByteArray sha256HexOfStream(QIODevice& stream);

/// Signs with AWS Signature Version 4 and returns the headers to add:
/// `Authorization`, `x-amz-date` and `x-amz-content-sha256`.
///
/// Implemented here rather than taken from a library. What was needed from
/// aws-sdk-cpp was this algorithm, and it is a page of HMAC-SHA256 over OpenSSL,
/// which the credential store already required -- see ADR-0011.
HeaderList signWithSigV4(const SignableRequest& request, const SigningIdentity& identity);

/// The canonical request string, exactly as it is fed to the hash. Exposed
/// because it is the thing worth comparing against AWS's published examples: a
/// wrong signature says only "denied", while a wrong canonical request says
/// which line went wrong.
QByteArray canonicalRequestFor(const SignableRequest& request);

/// The "string to sign" derived from a canonical request.
QByteArray stringToSignFor(
    const SignableRequest& request, const SigningIdentity& identity, const QByteArray& canonicalRequest);

/// Percent-encodes for a canonical query string or path segment, to RFC 3986 --
/// which is stricter than a url normally needs and is what S3 verifies against.
QByteArray uriEncode(const QByteArray& text, bool keepSlashes);

/// Builds a canonical query string from unencoded pairs: sorted by name, each
/// part encoded, joined with '&'.
QByteArray canonicalQuery(const QList<QPair<QString, QString>>& parameters);

/// The path as it appears in the signature, and as it must appear in the url.
QByteArray canonicalPathFor(const SignableRequest& request);

/// The query as it appears in the signature, and as it must appear in the url.
/// Empty when there is no query.
QByteArray canonicalQueryFor(const SignableRequest& request);

} // namespace mole::net
