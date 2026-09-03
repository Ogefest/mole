#include "plugins/network/S3FileSystem.h"

#include "plugins/network/TransferStreams.h"

#include "core/platform/Staging.h"

#include <QBuffer>
#include <QSet>
#include <QTemporaryFile>

#include <algorithm>

namespace mole {
namespace {

    /// A directory is a key ending in '/'. Written out as a named idea because it
    /// is the one convention the whole backend rests on.
    constexpr QLatin1Char kSeparator('/');

    /// How much of an object one request carries when it takes more than one.
    ///
    /// S3 puts a floor of 5 MiB under every part but the last and a ceiling of
    /// ten thousand parts on an object, so this size decides both how much local
    /// scratch space a write costs -- one part, never the object -- and how large
    /// an object can be at all. At 64 MiB that ceiling is 640 GB, which is past
    /// anything a file manager will be handed, and the staging cost stays small
    /// enough to be beneath notice.
    constexpr qint64 kPartBytes = 64LL * 1024 * 1024;

    /// The most one PUT carrying x-amz-copy-source will copy. S3's own limit,
    /// and B2 and MinIO hold to it as well; above it the request is refused
    /// outright rather than truncated.
    constexpr qint64 kCopyInOneRequestLimit = 5LL * 1024 * 1024 * 1024;

    /// How much of a large object each server-side part copy takes. A gibibyte
    /// rather than kPartBytes, because nothing travels to this machine for a
    /// copy -- what a part costs here is one request, and 64 MiB parts would
    /// mean fifteen hundred of them for a 94 GB object. Ten thousand parts is
    /// the ceiling either way, so this also carries objects up to 10 TB.
    constexpr qint64 kCopyPartBytes = 1024LL * 1024 * 1024;

    QString withTrailingSlash(const QString& key)
    {
        return key.endsWith(kSeparator) ? key : key + kSeparator;
    }

    /// What a key or a common prefix is called *inside* `prefix`: the text
    /// between the two, up to the next separator.
    ///
    /// Read relative to the prefix rather than by trimming the end of the key,
    /// which is what this replaced. Nothing stops a key having an empty segment
    /// -- `a//b` is a perfectly good key, and so is a leading `/` at the root --
    /// and trimming *all* trailing slashes turned the common prefix `a//` that
    /// S3 returns for it into "a": a phantom child of `a/` named after its own
    /// parent, which recursed. An empty answer means there is no name here, and
    /// the caller skips the row rather than showing one with no text in it. See
    /// MOLE-347.
    QString nameInside(const QString& prefix, const QString& key)
    {
        if (!key.startsWith(prefix))
            return QString();
        const QString relative = key.mid(prefix.size());
        const int slash = relative.indexOf(kSeparator);
        return slash < 0 ? relative : relative.left(slash);
    }

} // namespace

QString S3Settings::resolvedEndpoint() const
{
    if (!endpoint.isEmpty())
        return endpoint;
    return QStringLiteral("s3.%1.amazonaws.com").arg(region);
}

bool S3Settings::bucketFitsInHostName() const
{
    return !bucket.isEmpty() && !bucket.contains(kSeparator) && !bucket.contains(QLatin1Char('.'));
}

bool S3Settings::usesPathStyle() const
{
    return pathStyleAddressing || !bucketFitsInHostName();
}

QString S3Settings::hostName() const
{
    const QString host = resolvedEndpoint();
    if (usesPathStyle() || bucket.isEmpty())
        return host;

    // An endpoint that already carries the bucket is taken at its word rather than
    // having it prepended a second time. People paste the address B2 shows them,
    // and that address includes the bucket -- which would otherwise produce
    // "bucket.bucket.s3.…" and fail TLS for the same reason a dotted name does.
    if (host.startsWith(bucket + QLatin1Char('.')))
        return host;

    return bucket + QLatin1Char('.') + host;
}

S3FileSystem::S3FileSystem(QString scheme, S3Settings settings)
    : m_scheme(std::move(scheme))
    , m_settings(std::move(settings))
{
    m_identity.accessKeyId = m_settings.accessKeyId;
    m_identity.secretAccessKey = m_settings.secretAccessKey;
    m_identity.region = m_settings.region;
    m_identity.service = QStringLiteral("s3");

    net::TransportOptions options;
    // No username or password: S3 authenticates per request with a signature, and
    // handing curl credentials as well would add an Authorization header of its
    // own and break the one we computed.
    options.verifyTls = m_settings.verifyTls;
    m_pool = std::make_unique<net::CurlPool>(std::move(options));
}

VfsCapabilities S3FileSystem::capabilities() const
{
    // No ReportsAccess: an object store answers questions about permissions with
    // a bucket policy, which says nothing about one key. Saying nothing beats
    // inventing an answer -- the conformance suite checks for exactly that.
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead
        | VfsCapability::ReportsLeftovers;
}

QString S3FileSystem::keyFor(const VfsUri& uri) const
{
    QString path = uri.path();
    while (path.startsWith(kSeparator))
        path.remove(0, 1);

    QString prefix = m_settings.prefix;
    while (prefix.endsWith(kSeparator))
        prefix.chop(1);
    while (prefix.startsWith(kSeparator))
        prefix.remove(0, 1);

    if (prefix.isEmpty())
        return path;
    if (path.isEmpty())
        return prefix;
    return prefix + kSeparator + path;
}

QString S3FileSystem::rootKey() const
{
    QString prefix = m_settings.prefix;
    while (prefix.endsWith(kSeparator))
        prefix.chop(1);
    while (prefix.startsWith(kSeparator))
        prefix.remove(0, 1);
    return prefix;
}

QString S3FileSystem::keyToPath(const QString& key) const
{
    const QString prefix = rootKey();
    if (prefix.isEmpty())
        return kSeparator + key;
    if (key == prefix)
        return QString(kSeparator);
    if (key.startsWith(prefix + kSeparator))
        return kSeparator + key.mid(prefix.size() + 1);
    // Outside this drive's prefix, which the listing filter should have kept out.
    // Shown whole rather than mangled: a path that lies about where something is
    // is worse than one that is longer than expected.
    return kSeparator + key;
}

Result<QList<net::S3Version>> S3FileSystem::versionsUnder(
    const QString& prefix, bool delimited, const CancelToken& cancel)
{
    QList<net::S3Version> found;
    QString keyMarker;
    QString versionMarker;

    // Paged, and with two markers rather than one: several states of one key can
    // straddle a page boundary, so the key alone cannot say where to carry on
    // from. Stopping at the first page would report some of what a container is
    // keeping and quietly drop the rest, which is a wrong answer rather than a
    // slow one.
    for (;;) {
        if (cancel.isCancelled()) {
            return Result<QList<net::S3Version>>::failure(VfsError::Cancelled, QStringLiteral("cancelled"));
        }

        Call call;
        call.method = "GET";
        call.query.append({ QStringLiteral("versions"), QString() });
        if (!prefix.isEmpty())
            call.query.append({ QStringLiteral("prefix"), prefix });
        if (delimited)
            call.query.append({ QStringLiteral("delimiter"), QString(kSeparator) });
        if (!keyMarker.isEmpty())
            call.query.append({ QStringLiteral("key-marker"), keyMarker });
        if (!versionMarker.isEmpty())
            call.query.append({ QStringLiteral("version-id-marker"), versionMarker });

        const net::Response response = send(call, cancel);
        const VfsError error = errorFor(response, QStringLiteral("Listing earlier objects"));
        if (error.isError())
            return Result<QList<net::S3Version>>(error);

        net::S3VersionPage page;
        QString problem;
        if (!net::parseListObjectVersions(response.body, &page, &problem)) {
            return Result<QList<net::S3Version>>::failure(
                VfsError::IoError, QStringLiteral("Listing earlier objects: %1").arg(problem));
        }

        found.append(page.versions);
        if (!page.truncated || (page.nextKeyMarker.isEmpty() && page.nextVersionIdMarker.isEmpty()))
            break;
        keyMarker = page.nextKeyMarker;
        versionMarker = page.nextVersionIdMarker;
    }
    return Result<QList<net::S3Version>>(found);
}

Result<QStringList> S3FileSystem::askWhatIsOffered(const VfsUri&, const CancelToken& cancel)
{
    Call call;
    call.method = "GET";
    call.query.append({ QStringLiteral("versioning"), QString() });

    const net::Response response = send(call, cancel);
    const VfsError said = errorFor(response, QStringLiteral("Asking what this container keeps"));
    if (said.isError()) {
        // A container that will not say is not a container that said no. The
        // probe records that the asking failed and the drive goes on working --
        // see ADR-0076. What it records is what the container actually answered:
        // this used to replace it with "The container would not say", which
        // IFileSystem::probe() then logged at warning level with no reason in it,
        // and a warning with the reason removed is a warning nobody can act on.
        return said;
    }

    if (!net::parseVersioningEnabled(response.body))
        return QStringList();
    return QStringList { versionsActionId() };
}

Result<QStringList> S3FileSystem::entriesWithActions(const VfsUri& dir, const CancelToken& cancel)
{
    if (!offers().has(versionsActionId()))
        return QStringList();

    // One paginated pass over the prefix, which is the shape the service already
    // offers -- not one lookup per row. A folder of five thousand objects costs
    // as many requests as it has pages.
    const QString prefix = keyFor(dir);
    const QString withSlash = prefix.isEmpty() || prefix.endsWith(kSeparator) ? prefix : prefix + kSeparator;

    const Result<QList<net::S3Version>> versions = versionsUnder(withSlash, true, cancel);
    if (!versions.ok())
        return Result<QStringList>(versions.error());

    QSet<QString> named;
    for (const net::S3Version& version : versions.value()) {
        // What is on offer is an earlier state to read. The object as it is now
        // is not one, and the record of a deletion is not something to open.
        if (version.latest || version.deleteMarker)
            continue;
        const QString relative = version.key.mid(withSlash.size());
        // Objects in this folder, not under it. The delimiter already keeps the
        // deeper ones out, and a service that ignores it must not put them in.
        if (relative.isEmpty() || relative.contains(kSeparator))
            continue;
        named.insert(relative);
    }

    QStringList out(named.begin(), named.end());
    out.sort();
    return out;
}

net::SignableRequest S3FileSystem::readRequestFor(const QString& key, const QDateTime& at) const
{
    net::SignableRequest request;
    request.method = "GET";
    request.timestamp = at;

    const QString host = m_settings.hostName();
    QString path = QStringLiteral("/");
    // usesPathStyle() rather than the raw setting, for the reason send() gives:
    // the host and the path have to agree about where the bucket went.
    if (m_settings.usesPathStyle())
        path += m_settings.bucket + (key.isEmpty() ? QString() : kSeparator + key);
    else
        path += key;
    request.path = path;
    request.headers.append({ QByteArray("host"), host.toUtf8() });
    return request;
}

FileActionList S3FileSystem::actionsFor(const VfsUri& target, const FileEntry& entry)
{
    // A folder is not an object here -- it is a prefix, and there is nothing to
    // hand anybody a link to.
    if (entry.isDir || keyFor(target).isEmpty())
        return {};
    // And nothing can be signed without a key to sign with. A drive reading a
    // public bucket unauthenticated is one this cannot be offered on.
    if (m_identity.accessKeyId.isEmpty() || m_identity.secretAccessKey.isEmpty())
        return {};

    // The link is compiled in rather than probed, and rightly: whether one can
    // be signed depends on having a key, not on what the drive was pointed at.
    // The probe tier in ADR-0076 is for the other one below -- whether *this
    // container* keeps earlier objects.
    FileActionList offered { FileAction {
        linkActionId(), QStringLiteral("Copy a temporary link"), true, FileActionKind::Text } };

    // Offered on any object of a container that keeps earlier ones, without
    // asking whether this particular object has any. Asking would be a request
    // to the far end for every step of the cursor, and the precise answer is
    // already made once per folder by entriesWithActions() -- which is what the
    // marks in the listing are drawn from. Performing it on an object with
    // nothing earlier says so.
    if (offers().has(versionsActionId())) {
        offered.append(FileAction {
            versionsActionId(), QStringLiteral("Earlier versions"), true, FileActionKind::Uris });
    }
    return offered;
}

Result<FileActionOutcome> S3FileSystem::invoke(
    const QString& id, const VfsUri& target, const CancelToken& cancel)
{
    if (id == versionsActionId()) {
        const QString versioned = keyFor(target);
        if (versioned.isEmpty()) {
            return VfsError::make(
                VfsError::NotSupported, QStringLiteral("There is nothing here to have versions of"));
        }

        // The prefix is the key itself, so the answer is about this object --
        // and about any whose key merely begins with it, which are filtered out.
        const Result<QList<net::S3Version>> versions = versionsUnder(versioned, false, cancel);
        if (!versions.ok())
            return Result<FileActionOutcome>(versions.error());

        QList<VfsUri> found;
        for (const net::S3Version& version : versions.value()) {
            if (version.key != versioned || version.latest || version.deleteMarker)
                continue;
            found.append(target.withVersion(version.versionId));
        }
        if (found.isEmpty()) {
            return VfsError::make(VfsError::NotFound,
                QStringLiteral("This container keeps nothing earlier for %1").arg(target.fileName()));
        }
        return FileActionOutcome::fromUris(found);
    }

    if (id != linkActionId())
        return IFileSystem::invoke(id, target, cancel);

    const QString key = keyFor(target);
    if (key.isEmpty()) {
        return VfsError::make(
            VfsError::NotSupported, QStringLiteral("There is nothing here to make a link to"));
    }
    if (m_identity.accessKeyId.isEmpty() || m_identity.secretAccessKey.isEmpty()) {
        return VfsError::make(
            VfsError::AccessDenied, QStringLiteral("This drive has no key to sign a link with"));
    }

    // Signed here and sent nowhere: a presigned url is arithmetic over the key,
    // the path and the clock, so making one asks the far end nothing and costs
    // no round trip.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString url
        = net::presignedUrl(readRequestFor(key, now), m_identity, linkLifetime(), m_settings.useHttps);

    // The lifetime travels with the link. A link with no stated expiry is one
    // somebody pastes somewhere and is surprised by later.
    return FileActionOutcome::fromText(url, now.addSecs(linkLifetime().count()).toLocalTime());
}

net::Response S3FileSystem::send(const Call& call, const CancelToken& cancel, QIODevice* sink)
{
    auto lease = m_pool->take();
    if (!lease) {
        net::Response response;
        response.code = CURLE_FAILED_INIT;
        response.detail = QStringLiteral("could not allocate a transfer handle");
        return response;
    }

    net::SignableRequest request;
    request.method = call.method;
    request.queryParameters = call.query;
    request.timestamp = QDateTime::currentDateTimeUtc();

    const QString host = m_settings.hostName();
    QString path = QStringLiteral("/");
    // usesPathStyle() rather than the raw setting, so the host and the path always
    // agree about where the bucket went.
    if (m_settings.usesPathStyle())
        path += m_settings.bucket + (call.key.isEmpty() ? QString() : kSeparator + call.key);
    else
        path += call.key;
    request.path = path;

    request.payloadSha256 = call.body ? net::sha256HexOfStream(*call.body) : net::emptyPayloadSha256();

    request.headers = call.headers;
    request.headers.append({ QByteArray("host"), host.toUtf8() });

    const net::HeaderList signedHeaders = net::signWithSigV4(request, m_identity);

    // The url is built from the same canonical encoder that produced the
    // signature. Building it any other way is how a request comes to be signed
    // for one path and sent to another.
    QByteArray url = (m_settings.useHttps ? "https://" : "http://") + host.toUtf8();
    url += net::canonicalPathFor(request);
    const QByteArray query = net::canonicalQueryFor(request);
    if (!query.isEmpty())
        url += '?' + query;

    curl_slist* headerList = nullptr;
    const auto addHeader = [&headerList](const QByteArray& name, const QByteArray& value) {
        headerList = curl_slist_append(headerList, (name + ": " + value).constData());
    };
    for (const auto& header : call.headers)
        addHeader(header.first, header.second);
    for (const auto& header : signedHeaders)
        addHeader(header.first, header.second);
    // curl would otherwise announce "Expect: 100-continue" on a large upload,
    // which several S3 implementations answer badly.
    headerList = curl_slist_append(headerList, "Expect:");

    lease.setUrl(url);
    curl_easy_setopt(lease.get(), CURLOPT_HTTPHEADER, headerList);

    if (call.method == "HEAD") {
        curl_easy_setopt(lease.get(), CURLOPT_NOBODY, 1L);
    } else if (call.body) {
        net::CurlPool::sendFrom(lease, *call.body, call.bodySize);
        curl_easy_setopt(lease.get(), CURLOPT_CUSTOMREQUEST, call.method.constData());
    } else if (call.method != "GET") {
        curl_easy_setopt(lease.get(), CURLOPT_CUSTOMREQUEST, call.method.constData());
    }

    const net::Response response = m_pool->perform(lease, cancel, sink);
    curl_slist_free_all(headerList);
    return response;
}

VfsError S3FileSystem::errorFor(const net::Response& response, const QString& what) const
{
    const VfsError mapped = net::errorFor(response, what);
    if (!mapped.isError())
        return mapped;

    // The server usually says something far more useful than the status code, and
    // "SignatureDoesNotMatch" in particular is the difference between a bug here
    // and a mistyped secret.
    const QString detail = net::parseS3Error(response.body);
    if (detail.isEmpty())
        return mapped;
    return VfsError::make(mapped.code, QStringLiteral("%1: %2").arg(what, detail));
}

Result<net::S3ListPage> S3FileSystem::listPage(
    const QString& prefix, bool delimited, const QString& token, int maxKeys, const CancelToken& cancel)
{
    Call call;
    call.query.append({ QStringLiteral("list-type"), QStringLiteral("2") });
    if (!prefix.isEmpty())
        call.query.append({ QStringLiteral("prefix"), prefix });
    if (delimited)
        call.query.append({ QStringLiteral("delimiter"), QStringLiteral("/") });
    if (!token.isEmpty())
        call.query.append({ QStringLiteral("continuation-token"), token });
    if (maxKeys > 0)
        call.query.append({ QStringLiteral("max-keys"), QString::number(maxKeys) });

    const net::Response response = send(call, cancel);
    const VfsError error = errorFor(response, QStringLiteral("Listing the bucket"));
    if (error.isError())
        return Result<net::S3ListPage>(error);

    net::S3ListPage page;
    QString parseError;
    if (!net::parseListObjectsV2(response.body, &page, &parseError))
        return Result<net::S3ListPage>::failure(VfsError::IoError, parseError);
    return Result<net::S3ListPage>(page);
}

Result<QList<net::S3Object>> S3FileSystem::allKeysUnder(const QString& prefix, const CancelToken& cancel)
{
    QList<net::S3Object> objects;
    QString token;
    do {
        Result<net::S3ListPage> page = listPage(prefix, false, token, 0, cancel);
        if (!page.ok())
            return Result<QList<net::S3Object>>(page.error());
        objects.append(page.value().objects);
        token = page.value().truncated ? page.value().nextContinuationToken : QString();
    } while (!token.isEmpty());
    return Result<QList<net::S3Object>>(objects);
}

Result<FileEntryList> S3FileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    if (cancel.isCancelled()) {
        return Result<FileEntryList>::failure(VfsError::Cancelled, QStringLiteral("Listing was cancelled"));
    }

    const QString key = keyFor(dir);
    const QString prefix = key.isEmpty() ? QString() : withTrailingSlash(key);

    FileEntryList entries;
    QString token;
    do {
        Result<net::S3ListPage> page = listPage(prefix, true, token, 0, cancel);
        if (!page.ok())
            return Result<FileEntryList>(page.error());

        for (const QString& childPrefix : page.value().commonPrefixes) {
            FileEntry entry;
            entry.name = nameInside(prefix, childPrefix);
            // A prefix with nothing between the separators names nothing. The
            // guard used to be on objects only, so such a prefix reached the
            // model as a row with an empty name.
            if (entry.name.isEmpty())
                continue;
            entry.uri = dir.child(entry.name);
            entry.isDir = true;
            entry.isWritable = true;
            entries.append(entry);
        }

        for (const net::S3Object& object : page.value().objects) {
            // The marker object for this directory is the directory, not a child.
            if (object.key == prefix)
                continue;
            FileEntry entry;
            entry.name = nameInside(prefix, object.key);
            if (entry.name.isEmpty())
                continue;
            entry.uri = dir.child(entry.name);
            entry.isDir = object.key.endsWith(kSeparator);
            // A folder marker is a zero-byte key, and nought is what every other
            // drive reports for a folder -- not the unknown a missing Size means.
            entry.size = entry.isDir ? 0 : object.size;
            entry.modified = object.modified;
            entry.isHidden = entry.name.startsWith(QLatin1Char('.'));
            entry.isWritable = true;
            entries.append(entry);
        }

        token = page.value().truncated ? page.value().nextContinuationToken : QString();
    } while (!token.isEmpty());

    if (!entries.isEmpty() || dir.isRoot())
        return Result<FileEntryList>(entries);

    // Nothing came back. For the drive root that is simply an empty bucket, but
    // deeper down it has to be distinguished from a path that is not a directory
    // at all -- otherwise browsing into a file would show an empty folder.
    const Result<FileEntry> what = stat(dir);
    if (!what.ok())
        return Result<FileEntryList>(what.error());
    if (!what.value().isDir) {
        return Result<FileEntryList>::failure(
            VfsError::NotADirectory, QStringLiteral("%1 is an object, not a directory").arg(dir.path()));
    }
    return Result<FileEntryList>(entries);
}

bool S3FileSystem::objectExists(const QString& key)
{
    Call call;
    call.method = "HEAD";
    call.key = key;
    const net::Response response = send(call, CancelToken());
    return response.code == CURLE_OK && response.httpOk();
}

Result<FileEntry> S3FileSystem::stat(const VfsUri& target)
{
    const QString key = keyFor(target);

    if (target.isRoot()) {
        // The drive root is the bucket, or a prefix in it. It exists as long as
        // the bucket answers at all, so that is what is checked.
        Result<net::S3ListPage> reachable = listPage(QString(), true, QString(), 1, CancelToken());
        if (!reachable.ok())
            return Result<FileEntry>(reachable.error());

        FileEntry entry;
        entry.uri = target;
        entry.isDir = true;
        entry.isWritable = true;
        return Result<FileEntry>(entry);
    }

    // An object with exactly this key: the ordinary case.
    Call head;
    head.method = "HEAD";
    head.key = key;
    if (target.hasVersion())
        head.query.append({ QStringLiteral("versionId"), target.version() });
    const net::Response response = send(head, CancelToken());
    if (response.code == CURLE_OK && response.httpOk()) {
        FileEntry entry;
        entry.name = target.fileName();
        entry.uri = target;
        entry.isDir = false;
        // A HEAD without a Content-Length is unusual and not impossible -- a
        // proxy in the way, a store that answers chunked. Unknown rather than
        // nought: nought is a real size and the one that turns the short-read
        // guard off. See MOLE-344.
        entry.size = sizeFromListing(QString::fromUtf8(response.header("content-length")));
        // Through the shared reader, not a bare RFC 2822 parse: every HTTP date
        // ends in "GMT" and Qt's reader will not have it, so every stat() on
        // every object came back with no timestamp at all. See MOLE-347.
        entry.modified = net::httpDate(QString::fromUtf8(response.header("last-modified")));
        entry.isWritable = true;
        return Result<FileEntry>(entry);
    }
    if (response.code != CURLE_OK && response.status == 0)
        return Result<FileEntry>(errorFor(response, QStringLiteral("Reading %1").arg(target.path())));
    if (response.status != 404 && response.status != 403) {
        return Result<FileEntry>(errorFor(response, QStringLiteral("Reading %1").arg(target.path())));
    }

    // A 403 is still worth the directory probe below -- a bucket that denies
    // ListBucket answers 403 for a key that is simply not there, so the refusal
    // and the absence arrive in the same envelope -- but it must not *end* as
    // NotFound. A per-key denial reported as "does not exist" is the one answer
    // that sends somebody looking for a file rather than for a policy, and the
    // other five backends all say AccessDenied. So the refusal is remembered and
    // used when the probe finds nothing. See MOLE-347.
    const bool refused = response.status == 403;

    // Otherwise it may be a directory: either an explicit marker, or a prefix
    // that something else wrote keys under without one.
    const QString directoryKey = withTrailingSlash(key);
    if (objectExists(directoryKey)) {
        FileEntry entry;
        entry.name = target.fileName();
        entry.uri = target;
        entry.isDir = true;
        entry.isWritable = true;
        return Result<FileEntry>(entry);
    }

    Result<net::S3ListPage> page = listPage(directoryKey, true, QString(), 1, CancelToken());
    if (!page.ok())
        return Result<FileEntry>(page.error());
    if (!page.value().objects.isEmpty() || !page.value().commonPrefixes.isEmpty()) {
        FileEntry entry;
        entry.name = target.fileName();
        entry.uri = target;
        entry.isDir = true;
        entry.isWritable = true;
        return Result<FileEntry>(entry);
    }

    if (refused) {
        return Result<FileEntry>::failure(
            VfsError::AccessDenied, QStringLiteral("Reading %1: the bucket refused it").arg(target.path()));
    }
    return Result<FileEntry>::failure(
        VfsError::NotFound, QStringLiteral("%1 does not exist").arg(target.path()));
}

Result<void> S3FileSystem::putObject(
    const QString& key, QIODevice* body, qint64 size, const CancelToken& cancel)
{
    Call call;
    call.method = "PUT";
    call.key = key;
    call.body = body;
    call.bodySize = size;

    QByteArray empty;
    QBuffer emptyBuffer(&empty);
    if (!body) {
        emptyBuffer.open(QIODevice::ReadOnly);
        call.body = &emptyBuffer;
        call.bodySize = 0;
    }

    const net::Response response = send(call, cancel);
    const VfsError error = errorFor(response, QStringLiteral("Writing %1").arg(key));
    if (error.isError())
        return Result<void>(error);
    return {};
}

Result<void> S3FileSystem::deleteObject(const QString& key)
{
    Call call;
    call.method = "DELETE";
    call.key = key;
    const net::Response response = send(call, CancelToken());
    const VfsError error = errorFor(response, QStringLiteral("Deleting %1").arg(key));
    if (error.isError())
        return Result<void>(error);
    return {};
}

QByteArray S3FileSystem::copySourceFor(const QString& key) const
{
    // The source is a header, and it carries the bucket -- which is why it is
    // built here rather than by the addressing-style logic.
    const QString source = QLatin1Char('/') + m_settings.bucket + kSeparator + key;
    return net::uriEncode(source.toUtf8(), true);
}

Result<void> S3FileSystem::copyObject(const QString& fromKey, const QString& toKey, qint64 size)
{
    // Above the single-request limit this has to be a server-side multipart copy
    // instead. One PUT carrying x-amz-copy-source is refused over 5 GB by S3, by
    // B2 and by MinIO, so a rename of one of the files ADR-0014 keeps citing --
    // a 20 GB backup, a 94 GB image -- could not be done at all, and a folder
    // rename stopped at the first large object with half its keys already copied
    // under the new prefix. See MOLE-347.
    if (size >= kCopyInOneRequestLimit)
        return copyObjectInParts(fromKey, toKey, size);

    Call call;
    call.method = "PUT";
    call.key = toKey;
    call.headers.append({ QByteArray("x-amz-copy-source"), copySourceFor(fromKey) });

    const net::Response response = send(call, CancelToken());
    const VfsError error = errorFor(response, QStringLiteral("Copying %1").arg(fromKey));
    if (error.isError())
        return Result<void>(error);

    // Answered 200 with the failure in the body, the same way completeMultipart
    // is: a CopyObject that reports SlowDown or an expired token this way would
    // otherwise be read as a copy that happened.
    const QString said = net::parseS3Error(response.body);
    if (!said.isEmpty())
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Copying %1: %2").arg(fromKey, said));
    return {};
}

Result<void> S3FileSystem::copyObjectInParts(const QString& fromKey, const QString& toKey, qint64 size)
{
    const Result<QString> begun = beginMultipart(toKey);
    if (!begun.ok())
        return Result<void>(begun.error());
    const QString uploadId = begun.value();

    QList<QByteArray> tags;
    for (qint64 at = 0; at < size; at += kCopyPartBytes) {
        const qint64 last = std::min(at + kCopyPartBytes, size) - 1;
        const Result<QByteArray> tag
            = copyPart(fromKey, toKey, uploadId, static_cast<int>(tags.size()) + 1, at, last);
        if (!tag.ok()) {
            abandonMultipart(toKey, uploadId);
            return Result<void>(tag.error());
        }
        tags.append(tag.value());
    }

    const Result<void> finished = completeMultipart(toKey, uploadId, tags);
    if (!finished.ok()) {
        abandonMultipart(toKey, uploadId);
        return finished;
    }
    return {};
}

Result<QByteArray> S3FileSystem::copyPart(const QString& fromKey, const QString& toKey,
    const QString& uploadId, int partNumber, qint64 first, qint64 last)
{
    Call call;
    call.method = "PUT";
    call.key = toKey;
    call.query.append({ QStringLiteral("partNumber"), QString::number(partNumber) });
    call.query.append({ QStringLiteral("uploadId"), uploadId });
    call.headers.append({ QByteArray("x-amz-copy-source"), copySourceFor(fromKey) });
    // Inclusive at both ends, which is what the header means and not what a
    // half-open range would be -- an off-by-one here duplicates or drops a byte
    // at every part boundary and the object still arrives at the right size.
    call.headers.append({ QByteArray("x-amz-copy-source-range"),
        QByteArray("bytes=") + QByteArray::number(first) + '-' + QByteArray::number(last) });

    const net::Response response = send(call, CancelToken());
    const VfsError error = errorFor(
        response, QStringLiteral("Copying bytes %1 to %2 of %3").arg(first).arg(last).arg(fromKey));
    if (error.isError())
        return Result<QByteArray>(error);

    const QString tag = net::parseCopyPartETag(response.body);
    if (tag.isEmpty()) {
        // 200 with no CopyPartResult in it is a failure the status did not
        // mention, which is how S3 reports one for this request.
        const QString said = net::parseS3Error(response.body);
        return Result<QByteArray>::failure(VfsError::IoError,
            QStringLiteral("Copying bytes %1 to %2 of %3: %4")
                .arg(first)
                .arg(last)
                .arg(fromKey, said.isEmpty() ? QStringLiteral("the server did not acknowledge it") : said));
    }
    return Result<QByteArray>(tag.toUtf8());
}

Result<void> S3FileSystem::makeDirectory(const VfsUri& target)
{
    if (stat(target).ok()) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(target.path()));
    }
    return putObject(withTrailingSlash(keyFor(target)), nullptr, 0);
}

Result<void> S3FileSystem::remove(const VfsUri& target, bool recursive)
{
    const Result<FileEntry> what = stat(target);
    if (!what.ok())
        return Result<void>(what.error());

    const QString key = keyFor(target);
    if (!what.value().isDir)
        return deleteObject(key);

    const QString prefix = withTrailingSlash(key);
    Result<QList<net::S3Object>> under = allKeysUnder(prefix, CancelToken());
    if (!under.ok())
        return Result<void>(under.error());

    QList<net::S3Object> children;
    for (const net::S3Object& object : under.value()) {
        if (object.key != prefix)
            children.append(object);
    }

    if (!children.isEmpty()) {
        if (!recursive) {
            return Result<void>::failure(
                VfsError::NotEmpty, QStringLiteral("%1 is not empty").arg(target.path()));
        }
        for (const net::S3Object& object : children) {
            const Result<void> removed = deleteObject(object.key);
            if (!removed.ok())
                return removed;
        }
    }

    // The marker last, and only when there is one: a prefix that never had one is
    // gone the moment its keys are.
    if (objectExists(prefix))
        return deleteObject(prefix);
    return {};
}

Result<void> S3FileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    const Result<FileEntry> source = stat(from);
    if (!source.ok())
        return Result<void>(source.error());
    if (stat(to).ok()) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(to.path()));
    }

    const QString fromKey = keyFor(from);
    const QString toKey = keyFor(to);

    if (!source.value().isDir) {
        // Copy-then-delete, because S3 has no rename. A failed copy leaves the
        // original alone, which is the right way round. The size comes from the
        // stat above and decides whether one request can carry it.
        const Result<void> copied = copyObject(fromKey, toKey, source.value().size);
        if (!copied.ok())
            return copied;
        return deleteObject(fromKey);
    }

    const QString fromPrefix = withTrailingSlash(fromKey);
    const QString toPrefix = withTrailingSlash(toKey);
    Result<QList<net::S3Object>> under = allKeysUnder(fromPrefix, CancelToken());
    if (!under.ok())
        return Result<void>(under.error());

    for (const net::S3Object& object : under.value()) {
        const QString target = toPrefix + object.key.mid(fromPrefix.size());
        // The size the listing gave, so an object too big for one copy request
        // goes part by part rather than failing the whole rename at it.
        const Result<void> copied = copyObject(object.key, target, object.size);
        if (!copied.ok())
            return copied;
        // Removed here rather than in a second pass over the whole list. A
        // folder rename is not atomic on a bucket and cannot be made one; what
        // can be decided is what a failure half way through leaves, and one key
        // in two places is a better place to be interrupted than every key
        // copied so far still sitting under the old prefix as well.
        const Result<void> removed = deleteObject(object.key);
        if (!removed.ok())
            return removed;
    }
    if (!under.value().isEmpty())
        return {};

    // An empty directory is only its marker.
    const Result<void> copied = copyObject(fromPrefix, toPrefix);
    if (!copied.ok())
        return copied;
    return deleteObject(fromPrefix);
}

Result<std::unique_ptr<QIODevice>> S3FileSystem::openRead(const VfsUri& target, qint64)
{
    auto scratch = std::make_unique<QTemporaryFile>();
    QString staging;
    if (!staging::openFile(*scratch, &staging)) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError,
            QStringLiteral("Could not open a local copy for %1: %2").arg(target.path(), staging));
    }

    Call call;
    call.key = keyFor(target);
    // The state the uri names, rather than the object as it is. One parameter,
    // and the whole reason an earlier version is an ordinary readable uri.
    if (target.hasVersion())
        call.query.append({ QStringLiteral("versionId"), target.version() });
    const net::Response response = send(call, CancelToken(), scratch.get());
    const VfsError error = errorFor(response, QStringLiteral("Reading %1").arg(target.path()));
    if (error.isError())
        return Result<std::unique_ptr<QIODevice>>(error);

    return net::openDownloadedFile(std::move(scratch));
}

Result<QString> S3FileSystem::beginMultipart(const QString& key)
{
    Call call;
    call.method = "POST";
    call.key = key;
    call.query.append({ QStringLiteral("uploads"), QString() });

    const net::Response response = send(call, CancelToken());
    const VfsError error = errorFor(response, QStringLiteral("Starting the upload of %1").arg(key));
    if (error.isError())
        return Result<QString>(error);

    const QString uploadId = net::parseMultipartUploadId(response.body);
    if (uploadId.isEmpty()) {
        return Result<QString>::failure(VfsError::IoError,
            QStringLiteral("Starting the upload of %1: the server did not give an upload id").arg(key));
    }
    return Result<QString>(uploadId);
}

Result<QByteArray> S3FileSystem::uploadPart(const QString& key, const QString& uploadId, int partNumber,
    QIODevice& body, qint64 size, const CancelToken& cancel)
{
    Call call;
    call.method = "PUT";
    call.key = key;
    call.query.append({ QStringLiteral("partNumber"), QString::number(partNumber) });
    call.query.append({ QStringLiteral("uploadId"), uploadId });
    call.body = &body;
    call.bodySize = size;

    // With the token, not without it: a part is up to 64 MiB, and a cancel that
    // is only noticed once the request in flight has finished is a cancel the
    // user waits a minute for. See MOLE-347.
    const net::Response response = send(call, cancel);
    const VfsError error
        = errorFor(response, QStringLiteral("Writing part %1 of %2").arg(partNumber).arg(key));
    if (error.isError())
        return Result<QByteArray>(error);

    const QByteArray tag = response.header("etag");
    if (tag.isEmpty()) {
        return Result<QByteArray>::failure(VfsError::IoError,
            QStringLiteral("Writing part %1 of %2: the server did not acknowledge it")
                .arg(partNumber)
                .arg(key));
    }
    return Result<QByteArray>(tag);
}

Result<void> S3FileSystem::completeMultipart(
    const QString& key, const QString& uploadId, const QList<QByteArray>& tags)
{
    QByteArray xml = "<CompleteMultipartUpload>";
    for (int i = 0; i < tags.size(); ++i) {
        xml += "<Part><PartNumber>" + QByteArray::number(i + 1) + "</PartNumber><ETag>" + tags.at(i)
            + "</ETag></Part>";
    }
    xml += "</CompleteMultipartUpload>";

    QBuffer body(&xml);
    body.open(QIODevice::ReadOnly);

    Call call;
    call.method = "POST";
    call.key = key;
    call.query.append({ QStringLiteral("uploadId"), uploadId });
    call.body = &body;
    call.bodySize = xml.size();

    const net::Response response = send(call, CancelToken());
    const VfsError error = errorFor(response, QStringLiteral("Finishing %1").arg(key));
    if (error.isError())
        return Result<void>(error);

    // S3 answers this one with 200 and puts the failure in the body, which is
    // the one place a status code cannot be trusted. Believing it would report a
    // finished upload that does not exist.
    const QString said = net::parseS3Error(response.body);
    if (!said.isEmpty())
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Finishing %1: %2").arg(key, said));
    return {};
}

void S3FileSystem::abandonMultipart(const QString& key, const QString& uploadId)
{
    Call call;
    call.method = "DELETE";
    call.key = key;
    call.query.append({ QStringLiteral("uploadId"), uploadId });
    send(call, CancelToken());
}

Result<QList<net::S3Upload>> S3FileSystem::listUnfinishedUploads(const CancelToken& cancel)
{
    QList<net::S3Upload> found;
    QString keyMarker;
    QString uploadIdMarker;
    const QString root = rootKey();

    // Paged, and with two markers rather than one: two uploads of the same key
    // can be in flight, so the key alone does not say where to carry on from.
    // Stopping at the first page would report the first thousand leftovers and
    // leave the rest being paid for, which is the fault this exists to end.
    for (;;) {
        if (cancel.isCancelled())
            return Result<QList<net::S3Upload>>::failure(VfsError::Cancelled, QStringLiteral("cancelled"));

        Call call;
        call.method = "GET";
        call.query.append({ QStringLiteral("uploads"), QString() });
        // No `prefix` parameter, deliberately. S3 documents one and MinIO
        // answers an empty list for a prefix that certainly matches -- measured
        // against the test machine, with a hand-seeded upload the unfiltered
        // listing reports and the filtered one does not. A filter that silently
        // hides leftovers is the same fault as not looking for them, so the
        // whole list is asked for and narrowed below.
        //
        // The cost is carrying uploads belonging to other prefixes of the same
        // bucket. A bucket has a handful of these at most -- one per copy
        // somebody was killed in the middle of -- so the whole list is small.
        if (!keyMarker.isEmpty())
            call.query.append({ QStringLiteral("key-marker"), keyMarker });
        if (!uploadIdMarker.isEmpty())
            call.query.append({ QStringLiteral("upload-id-marker"), uploadIdMarker });

        const net::Response response = send(call, cancel);
        const VfsError error = errorFor(response, QStringLiteral("Listing unfinished uploads"));
        if (error.isError())
            return Result<QList<net::S3Upload>>(error);

        net::S3UploadPage page;
        QString problem;
        if (!net::parseListMultipartUploads(response.body, &page, &problem)) {
            return Result<QList<net::S3Upload>>::failure(
                VfsError::IoError, QStringLiteral("Listing unfinished uploads: %1").arg(problem));
        }

        // Narrowed to this drive. A drive rooted at a prefix must not offer to
        // abandon uploads belonging to another drive on the same bucket.
        for (const net::S3Upload& upload : std::as_const(page.uploads)) {
            if (root.isEmpty() || upload.key == root || upload.key.startsWith(root + kSeparator))
                found.append(upload);
        }
        if (!page.truncated || (page.nextKeyMarker.isEmpty() && page.nextUploadIdMarker.isEmpty()))
            break;
        keyMarker = page.nextKeyMarker;
        uploadIdMarker = page.nextUploadIdMarker;
    }
    return Result<QList<net::S3Upload>>(found);
}

Result<QList<DriveLeftover>> S3FileSystem::leftovers(
    std::chrono::seconds olderThan, const CancelToken& cancel)
{
    const Result<QList<net::S3Upload>> uploads = listUnfinishedUploads(cancel);
    if (!uploads.ok())
        return Result<QList<DriveLeftover>>(uploads.error());

    // Anything younger than the threshold is left alone. It is very likely an
    // upload somebody has in flight this minute -- possibly another window of
    // this application -- and from here it is indistinguishable from one a
    // killed process abandoned. Abandoning it would break a copy that was going
    // perfectly well, which is a worse fault than the one being cleaned up.
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-olderThan.count());

    QList<DriveLeftover> out;
    for (const net::S3Upload& upload : uploads.value()) {
        if (upload.initiated.isValid() && upload.initiated > cutoff)
            continue;

        DriveLeftover leftover;
        // Key and id together: the id alone does not say what to abandon, and
        // two uploads of one key differ only by it.
        leftover.handle = upload.key + QLatin1Char('\n') + upload.uploadId;
        leftover.path = keyToPath(upload.key);
        leftover.started = upload.initiated;
        // Not asked for. Knowing it means a ListParts for every upload, which
        // turns a single request into one per leftover for a figure nobody needs
        // before deciding -- what is being decided is whether to keep something
        // nothing can finish.
        leftover.bytes = -1;
        leftover.what = QStringLiteral("an upload that was never finished");
        out.append(leftover);
    }
    return Result<QList<DriveLeftover>>(out);
}

Result<void> S3FileSystem::discardLeftover(const DriveLeftover& leftover)
{
    const qsizetype split = leftover.handle.indexOf(QLatin1Char('\n'));
    if (split <= 0) {
        return Result<void>::failure(
            VfsError::NotFound, QStringLiteral("That is not something this drive handed out"));
    }
    const QString key = leftover.handle.left(split);
    const QString uploadId = leftover.handle.mid(split + 1);

    Call call;
    call.method = "DELETE";
    call.key = key;
    call.query.append({ QStringLiteral("uploadId"), uploadId });
    const net::Response response = send(call, CancelToken());
    return errorFor(response, QStringLiteral("Abandoning the unfinished upload of %1").arg(leftover.path));
}

Result<std::unique_ptr<QIODevice>> S3FileSystem::openWrite(const VfsUri& target, qint64 expectedSize)
{
    const QString key = keyFor(target);

    // A bucket has no directories, so nothing here can be destroyed by writing
    // over one -- but the listing shows folder markers as folders, and a key
    // written next to its own marker gives the user two entries with one name
    // and no way to tell them apart. One HEAD per upload buys the same answer
    // every other drive gives. See MOLE-336.
    if (VfsError folder = refuseWritingOntoAFolder(*this, target); folder.isError())
        return folder;

    // Small and measured: one signed PUT, which is cheaper than three requests
    // and is what most writes are.
    if (expectedSize >= 0 && expectedSize <= kPartBytes) {
        auto stream = std::make_unique<net::BufferedUpload>(
            [this, key](QIODevice& payload, qint64 size) { return putObject(key, &payload, size); });
        if (!stream->open(QIODevice::WriteOnly)) {
            return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, stream->errorString());
        }
        return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(stream.release()));
    }

    // Anything else goes up a part at a time. Each part is staged locally so it
    // can be measured and signed -- S3 signs a payload hash, and there is no
    // getting round knowing the length of what is being signed -- but only one
    // part is ever staged, so the cost is the part size and not the object.
    //
    // The upload's own lifetime is what abandons it. A cancelled copy destroys
    // the stream, which destroys the lambda below and with it the last reference
    // to this -- and an upload begun and never completed is parts sitting in the
    // bucket **being charged for** until something removes them (ADR-0015). It
    // used to be abandoned only where uploadPart() or completeMultipart()
    // answered with a failure, so the one case that costs money -- the caller
    // giving up, which is what a cancelled copy is -- was the case that leaked.
    // MOLE-96's sweep is for a process that was killed and never got to tidy up;
    // this is a live cancel, which the code is there to handle itself.
    struct Upload
    {
        Upload(S3FileSystem& drive, QString forKey)
            : fs(&drive)
            , key(std::move(forKey))
        {
        }
        ~Upload()
        {
            // A blocking DELETE from a destructor, and worth it: the alternative
            // is a bill for parts nothing will ever list.
            if (!uploadId.isEmpty() && !settled)
                fs->abandonMultipart(key, uploadId);
        }
        Upload(const Upload&) = delete;
        Upload& operator=(const Upload&) = delete;

        S3FileSystem* fs;
        QString key;
        QString uploadId;
        QList<QByteArray> tags;
        /// Set once the upload has been completed or abandoned deliberately,
        /// which is what disarms the destructor.
        bool settled = false;
    };
    auto upload = std::make_shared<Upload>(*this, key);

    auto send = [this, key, upload](QIODevice& source, qint64 span, bool append, const CancelToken& cancel) {
        QTemporaryFile part;
        QString staging;
        if (!staging::openFile(part, &staging)) {
            return VfsError::make(
                VfsError::IoError, QStringLiteral("Could not stage a part of %1: %2").arg(key, staging));
        }

        QByteArray buffer(256 * 1024, Qt::Uninitialized);
        qint64 staged = 0;
        while (staged < span) {
            // Said as a cancellation rather than as an I/O failure. The reader
            // answers -1 for both, and "Writing x: the writer went away" is what
            // a cancelled copy used to be reported as.
            if (cancel.isCancelled())
                return VfsError::make(VfsError::Cancelled, QStringLiteral("Writing %1: cancelled").arg(key));
            const qint64 got = source.read(buffer.data(), std::min<qint64>(buffer.size(), span - staged));
            if (got < 0)
                return VfsError::make(
                    VfsError::IoError, QStringLiteral("Writing %1: %2").arg(key, source.errorString()));
            if (got == 0)
                break;
            if (part.write(buffer.constData(), got) != got) {
                return VfsError::make(
                    VfsError::IoError, QStringLiteral("Could not stage a part of %1").arg(key));
            }
            staged += got;
        }
        const bool last = staged < span;
        part.seek(0);

        // It all fitted in the first part after all, so it never needed to be a
        // multipart upload. Whoever asked simply did not know how much there was.
        if (!append && last) {
            const Result<void> put = putObject(key, &part, staged, cancel);
            return put.ok() ? VfsError::ok() : put.error();
        }

        if (upload->uploadId.isEmpty()) {
            const Result<QString> begun = beginMultipart(key);
            if (!begun.ok())
                return begun.error();
            upload->uploadId = begun.value();
        }

        if (staged > 0) {
            const Result<QByteArray> tag = uploadPart(
                key, upload->uploadId, static_cast<int>(upload->tags.size()) + 1, part, staged, cancel);
            if (!tag.ok()) {
                // At once rather than when the stream is dropped, which may be a
                // while: the guard above is the backstop, not the plan.
                abandonMultipart(key, upload->uploadId);
                upload->settled = true;
                return tag.error();
            }
            upload->tags.append(tag.value());
        }

        if (last) {
            const Result<void> finished = completeMultipart(key, upload->uploadId, upload->tags);
            if (!finished.ok()) {
                abandonMultipart(key, upload->uploadId);
                upload->settled = true;
                return finished.error();
            }
            upload->settled = true;
        }
        return VfsError::ok();
    };

    auto stream = std::make_unique<net::StreamingUpload>(std::move(send), kPartBytes);
    if (!stream->open(QIODevice::WriteOnly)) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, stream->errorString());
    }
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(stream.release()));
}

// ---- factory ---------------------------------------------------------------

QList<ConnectionField> S3FileSystemFactory::connectionFields() const
{
    QList<ConnectionField> fields;

    ConnectionField key;
    key.key = QStringLiteral("accessKeyId");
    key.label = QStringLiteral("Access key ID");
    key.help = QStringLiteral("On Backblaze B2 this is the application key ID");
    fields.append(key);

    ConnectionField secret;
    secret.key = QStringLiteral("secretAccessKey");
    secret.label = QStringLiteral("Secret access key");
    secret.kind = ConnectionField::Password;
    fields.append(secret);

    ConnectionField bucket;
    bucket.key = QStringLiteral("bucket");
    bucket.label = QStringLiteral("Bucket");
    fields.append(bucket);

    ConnectionField region;
    region.key = QStringLiteral("region");
    region.label = QStringLiteral("Region");
    region.defaultValue = QStringLiteral("us-east-1");
    region.help = QStringLiteral(
        "The region the bucket lives in. For Backblaze B2 it is the middle part of the endpoint, "
        "for example us-east-005.");
    fields.append(region);

    ConnectionField endpoint;
    endpoint.key = QStringLiteral("endpoint");
    endpoint.label = QStringLiteral("Endpoint");
    endpoint.required = false;
    endpoint.help
        = QStringLiteral("Host name of the service, without https://. Leave empty for AWS. Backblaze B2 uses "
                         "s3.<region>.backblazeb2.com; MinIO and Ceph use whatever they were installed as.");
    fields.append(endpoint);

    ConnectionField style;
    style.key = QStringLiteral("addressing");
    style.label = QStringLiteral("Bucket addressing");
    style.kind = ConnectionField::Choice;
    style.choices = { QStringLiteral("host"), QStringLiteral("path") };
    style.choiceLabels
        = { QStringLiteral("In the host name (AWS, B2)"), QStringLiteral("In the path (MinIO, Ceph)") };
    style.defaultValue = QStringLiteral("host");
    style.required = false;
    style.advanced = true;
    style.help = QStringLiteral(
        "A bucket whose name contains a dot always goes in the path, whatever is chosen here: "
        "a certificate wildcard covers only one name part, so such a bucket cannot go in the "
        "host name at all.");
    fields.append(style);

    ConnectionField https;
    https.key = QStringLiteral("useHttps");
    https.label = QStringLiteral("Use HTTPS");
    https.kind = ConnectionField::Boolean;
    https.defaultValue = true;
    https.required = false;
    https.advanced = true;
    fields.append(https);

    ConnectionField verify;
    verify.key = QStringLiteral("verifyTls");
    verify.label = QStringLiteral("Verify the TLS certificate");
    verify.kind = ConnectionField::Boolean;
    verify.defaultValue = true;
    verify.required = false;
    verify.advanced = true;
    verify.help = QStringLiteral("Turn off only for a self-signed test install you already trust");
    fields.append(verify);

    return fields;
}

S3Settings S3FileSystemFactory::settingsFrom(const QVariantMap& config)
{
    S3Settings settings;
    settings.accessKeyId = config.value(QStringLiteral("accessKeyId")).toString().trimmed();
    settings.secretAccessKey = config.value(QStringLiteral("secretAccessKey")).toString();
    settings.bucket = config.value(QStringLiteral("bucket")).toString().trimmed();

    const QString region = config.value(QStringLiteral("region")).toString().trimmed();
    settings.region = region.isEmpty() ? QStringLiteral("us-east-1") : region;

    QString endpoint = config.value(QStringLiteral("endpoint")).toString().trimmed();
    // A host is wanted, but people paste a url. Taking the host out of one is
    // kinder than refusing it.
    endpoint.remove(QStringLiteral("https://"));
    endpoint.remove(QStringLiteral("http://"));
    while (endpoint.endsWith(kSeparator))
        endpoint.chop(1);
    settings.endpoint = endpoint;

    settings.pathStyleAddressing
        = config.value(QStringLiteral("addressing")).toString() == QLatin1String("path");
    settings.useHttps = config.value(QStringLiteral("useHttps"), true).toBool();
    settings.verifyTls = config.value(QStringLiteral("verifyTls"), true).toBool();

    QString prefix = config.value(QStringLiteral("__root")).toString().trimmed();
    while (prefix.startsWith(kSeparator))
        prefix.remove(0, 1);
    while (prefix.endsWith(kSeparator))
        prefix.chop(1);
    settings.prefix = prefix;

    return settings;
}

FileSystemPtr S3FileSystemFactory::create(const QVariantMap& config, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return FileSystemPtr {};
    };

    const S3Settings settings = settingsFrom(config);
    if (settings.accessKeyId.isEmpty())
        return fail(QStringLiteral("An S3 drive needs an access key ID"));
    if (settings.secretAccessKey.isEmpty())
        return fail(QStringLiteral("An S3 drive needs a secret access key"));
    if (settings.bucket.isEmpty())
        return fail(QStringLiteral("An S3 drive needs a bucket"));

    const QString scheme = config.value(QStringLiteral("__scheme"), QStringLiteral("s3")).toString();
    return std::make_shared<S3FileSystem>(scheme, settings);
}

} // namespace mole
