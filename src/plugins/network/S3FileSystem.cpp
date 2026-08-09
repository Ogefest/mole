#include "plugins/network/S3FileSystem.h"

#include "plugins/network/TransferStreams.h"

#include <QBuffer>
#include <QTemporaryFile>

namespace mole {
namespace {

    /// A directory is a key ending in '/'. Written out as a named idea because it
    /// is the one convention the whole backend rests on.
    constexpr QLatin1Char kSeparator('/');

    QString withTrailingSlash(const QString& key)
    {
        return key.endsWith(kSeparator) ? key : key + kSeparator;
    }

    /// The last segment of a key, which is what a listing shows as a name.
    QString lastSegment(const QString& key)
    {
        QString trimmed = key;
        while (trimmed.endsWith(kSeparator))
            trimmed.chop(1);
        const int slash = trimmed.lastIndexOf(kSeparator);
        return slash < 0 ? trimmed : trimmed.mid(slash + 1);
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
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead;
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
            entry.name = lastSegment(childPrefix);
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
            entry.name = lastSegment(object.key);
            if (entry.name.isEmpty())
                continue;
            entry.uri = dir.child(entry.name);
            entry.isDir = object.key.endsWith(kSeparator);
            entry.size = object.size;
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
    const net::Response response = send(head, CancelToken());
    if (response.code == CURLE_OK && response.httpOk()) {
        FileEntry entry;
        entry.name = target.fileName();
        entry.uri = target;
        entry.isDir = false;
        entry.size = response.header("content-length").toLongLong();
        entry.modified
            = QDateTime::fromString(QString::fromUtf8(response.header("last-modified")), Qt::RFC2822Date);
        entry.isWritable = true;
        return Result<FileEntry>(entry);
    }
    if (response.code != CURLE_OK && response.status == 0)
        return Result<FileEntry>(errorFor(response, QStringLiteral("Reading %1").arg(target.path())));
    if (response.status != 404 && response.status != 403) {
        return Result<FileEntry>(errorFor(response, QStringLiteral("Reading %1").arg(target.path())));
    }

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

    return Result<FileEntry>::failure(
        VfsError::NotFound, QStringLiteral("%1 does not exist").arg(target.path()));
}

Result<void> S3FileSystem::putObject(const QString& key, QIODevice* body, qint64 size)
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

    const net::Response response = send(call, CancelToken());
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

Result<void> S3FileSystem::copyObject(const QString& fromKey, const QString& toKey)
{
    Call call;
    call.method = "PUT";
    call.key = toKey;
    // The source is a header, and it carries the bucket -- which is why it is
    // built here rather than by the addressing-style logic.
    const QString source = QLatin1Char('/') + m_settings.bucket + kSeparator + fromKey;
    call.headers.append({ QByteArray("x-amz-copy-source"), net::uriEncode(source.toUtf8(), true) });

    const net::Response response = send(call, CancelToken());
    const VfsError error = errorFor(response, QStringLiteral("Copying %1").arg(fromKey));
    if (error.isError())
        return Result<void>(error);
    return {};
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
        // original alone, which is the right way round.
        const Result<void> copied = copyObject(fromKey, toKey);
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
        const Result<void> copied = copyObject(object.key, target);
        if (!copied.ok())
            return copied;
    }
    for (const net::S3Object& object : under.value()) {
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
    if (!scratch->open()) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IoError, QStringLiteral("Could not open a local copy for %1").arg(target.path()));
    }

    Call call;
    call.key = keyFor(target);
    const net::Response response = send(call, CancelToken(), scratch.get());
    const VfsError error = errorFor(response, QStringLiteral("Reading %1").arg(target.path()));
    if (error.isError())
        return Result<std::unique_ptr<QIODevice>>(error);

    return net::openDownloadedFile(std::move(scratch));
}

Result<std::unique_ptr<QIODevice>> S3FileSystem::openWrite(const VfsUri& target)
{
    const QString key = keyFor(target);
    auto stream = std::make_unique<net::BufferedUpload>(
        [this, key](QIODevice& payload, qint64 size) { return putObject(key, &payload, size); });
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
