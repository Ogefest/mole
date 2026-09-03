#include "plugins/network/WebdavFileSystem.h"

#include "plugins/network/TransferStreams.h"

#include "core/platform/Staging.h"

#include <QTemporaryFile>
#include <QUrl>

#include <limits>

namespace mole {
namespace {

    /// Above this a write is streamed rather than staged, because staging it is
    /// what would make it impossible. See openWrite for why the small case is
    /// left exactly as it was.
    constexpr qint64 kStreamAbove = 64LL * 1024 * 1024;

    /// Asks for exactly the four properties a listing needs. Sending a body
    /// rather than relying on allprop keeps the answer small on a server that
    /// would otherwise return every property it has.
    const char* const kPropfindBody = R"(<?xml version="1.0" encoding="utf-8"?>
<propfind xmlns="DAV:">
  <prop>
    <resourcetype/>
    <getcontentlength/>
    <getlastmodified/>
  </prop>
</propfind>)";

} // namespace

QString WebdavSettings::origin() const
{
    const QUrl url(baseUrl);
    QString out = url.scheme() + QStringLiteral("://") + url.host();
    if (url.port() > 0)
        out += QLatin1Char(':') + QString::number(url.port());
    return out;
}

QString WebdavSettings::basePath() const
{
    return net::withoutTrailingSlash(QUrl(baseUrl).path());
}

WebdavFileSystem::WebdavFileSystem(QString scheme, WebdavSettings settings)
    : m_scheme(std::move(scheme))
    , m_settings(std::move(settings))
{
    net::TransportOptions options;
    options.username = m_settings.username;
    options.password = m_settings.password;
    options.verifyTls = m_settings.verifyTls;
    m_pool = std::make_unique<net::CurlPool>(std::move(options));
}

VfsCapabilities WebdavFileSystem::capabilities() const
{
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead;
}

QString WebdavFileSystem::remotePath(const VfsUri& uri) const
{
    QString root = net::withoutTrailingSlash(m_settings.remoteRoot);
    if (!root.isEmpty() && !root.startsWith(QLatin1Char('/')))
        root.prepend(QLatin1Char('/'));

    const QString path = uri.path() == QLatin1String("/") ? QString() : uri.path();
    const QString joined = m_settings.basePath() + root + path;
    return joined.isEmpty() ? QStringLiteral("/") : joined;
}

QByteArray WebdavFileSystem::urlFor(const VfsUri& uri) const
{
    return m_settings.origin().toUtf8() + net::encodePath(remotePath(uri));
}

net::Response WebdavFileSystem::send(const Call& call, const CancelToken& cancel, QIODevice* sink)
{
    auto lease = m_pool->take();
    if (!lease) {
        net::Response response;
        response.code = CURLE_FAILED_INIT;
        response.detail = QStringLiteral("could not allocate a transfer handle");
        return response;
    }

    curl_slist* headers = nullptr;
    for (const auto& header : call.headers)
        headers = curl_slist_append(headers, (header.first + ": " + header.second).constData());
    if (!call.body.isEmpty())
        headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
    if (!call.payload) {
        // Suppressed for a request that carries no file, where a server which
        // never answers "100 Continue" costs a wait for nothing: the body of a
        // PROPFIND is a few hundred bytes curl holds in memory and can send
        // again as often as it likes.
        //
        // **Never suppressed for a request that carries a file.** This one line
        // made every WebDAV write of any real size fail. `CURLAUTH_ANY` sends
        // the first request with no credentials, takes the 401 and tries again
        // -- and the body cannot be sent twice, because it comes from a
        // QIODevice curl has no way to rewind. curl calls that "necessary data
        // rewind wasn't possible", and it happens at every size: reproduced with
        // plain `curl --anyauth -T -` against this same server, exit 65, from
        // one kilobyte upwards. Only the conformance fixtures were small enough
        // to escape it, which is exactly why nothing noticed.
        //
        // Asking permission first means the 401 arrives before any of the file
        // does, so there is nothing to rewind. curl asks only when it needs to
        // -- an upload it knows it can repeat does not carry the header at all
        // -- so this costs a directory of small files nothing.
        headers = curl_slist_append(headers, "Expect:");
    }

    lease.setUrl(call.url);
    curl_easy_setopt(lease.get(), CURLOPT_HTTPHEADER, headers);
    // Whatever the server asks for -- Nextcloud wants Basic, some appliances
    // insist on Digest, and there is no reason to make the user care.
    curl_easy_setopt(lease.get(), CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY));

    if (call.payload) {
        net::CurlPool::sendFrom(lease, *call.payload, call.payloadSize);
        curl_easy_setopt(lease.get(), CURLOPT_CUSTOMREQUEST, call.method.constData());
    } else if (!call.body.isEmpty()) {
        curl_easy_setopt(lease.get(), CURLOPT_CUSTOMREQUEST, call.method.constData());
        curl_easy_setopt(lease.get(), CURLOPT_POSTFIELDS, call.body.constData());
        curl_easy_setopt(lease.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(call.body.size()));
    } else if (call.method != "GET") {
        curl_easy_setopt(lease.get(), CURLOPT_CUSTOMREQUEST, call.method.constData());
    }

    const net::Response response = m_pool->perform(lease, cancel, sink);
    curl_slist_free_all(headers);
    return response;
}

Result<QList<net::WebdavEntry>> WebdavFileSystem::propfind(
    const VfsUri& target, int depth, const CancelToken& cancel)
{
    Call call;
    call.method = "PROPFIND";
    call.url = urlFor(target);
    call.body = kPropfindBody;
    call.headers.append({ QByteArray("Depth"), QByteArray::number(depth) });

    const net::Response response = send(call, cancel);
    const VfsError error = net::errorFor(response, QStringLiteral("Reading %1").arg(target.path()));
    if (error.isError())
        return Result<QList<net::WebdavEntry>>(error);

    QList<net::WebdavEntry> entries;
    QString parseError;
    if (!net::parseMultistatus(response.body, &entries, &parseError))
        return Result<QList<net::WebdavEntry>>::failure(VfsError::IoError, parseError);
    return Result<QList<net::WebdavEntry>>(entries);
}

Result<FileEntryList> WebdavFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    Result<QList<net::WebdavEntry>> answer = propfind(dir, 1, cancel);
    if (!answer.ok())
        return Result<FileEntryList>(answer.error());

    const QString self = net::withoutTrailingSlash(remotePath(dir));

    // Depth 1 includes the collection itself. Finding it is also the only way to
    // learn that the target is a file: a PROPFIND on a file answers with exactly
    // one entry, itself, which would otherwise read as an empty directory.
    bool selfIsCollection = false;
    bool sawSelf = false;
    FileEntryList entries;

    for (const net::WebdavEntry& entry : answer.value()) {
        const QString path = net::withoutTrailingSlash(entry.path);
        if (path == self) {
            sawSelf = true;
            selfIsCollection = entry.isCollection;
            continue;
        }

        FileEntry out;
        out.name = net::nameFromPath(entry.path);
        if (out.name.isEmpty())
            continue;
        out.uri = dir.child(out.name);
        out.isDir = entry.isCollection;
        out.size = entry.size;
        out.modified = entry.modified;
        out.isHidden = out.name.startsWith(QLatin1Char('.'));
        out.isWritable = true;
        entries.append(out);
    }

    if (sawSelf && !selfIsCollection) {
        return Result<FileEntryList>::failure(
            VfsError::NotADirectory, QStringLiteral("%1 is a file, not a directory").arg(dir.path()));
    }
    return Result<FileEntryList>(entries);
}

Result<FileEntry> WebdavFileSystem::stat(const VfsUri& target)
{
    Result<QList<net::WebdavEntry>> answer = propfind(target, 0, CancelToken());
    if (!answer.ok())
        return Result<FileEntry>(answer.error());
    if (answer.value().isEmpty()) {
        return Result<FileEntry>::failure(
            VfsError::NotFound, QStringLiteral("%1 does not exist").arg(target.path()));
    }

    const net::WebdavEntry& found = answer.value().first();
    FileEntry entry;
    entry.name = target.isRoot() ? QString() : target.fileName();
    entry.uri = target;
    entry.isDir = found.isCollection;
    entry.size = found.size;
    entry.modified = found.modified;
    entry.isWritable = true;
    return Result<FileEntry>(entry);
}

Result<void> WebdavFileSystem::makeDirectory(const VfsUri& target)
{
    if (stat(target).ok()) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(target.path()));
    }

    Call call;
    call.method = "MKCOL";
    call.url = urlFor(target);
    const net::Response response = send(call, CancelToken());

    // MKCOL answers 405 when something is already there, which is a clearer
    // thing to say than "method not allowed".
    if (response.code == CURLE_OK && response.status == 405) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(target.path()));
    }

    const VfsError error = net::errorFor(response, QStringLiteral("Creating %1").arg(target.path()));
    if (error.isError())
        return Result<void>(error);
    return {};
}

Result<void> WebdavFileSystem::remove(const VfsUri& target, bool recursive)
{
    const Result<FileEntry> what = stat(target);
    if (!what.ok())
        return Result<void>(what.error());

    if (what.value().isDir && !recursive) {
        // WebDAV's DELETE on a collection is always recursive, so refusing a
        // non-empty one has to be done here rather than left to the server.
        const Result<FileEntryList> children = list(target, CancelToken());
        if (!children.ok())
            return Result<void>(children.error());
        if (!children.value().isEmpty()) {
            return Result<void>::failure(
                VfsError::NotEmpty, QStringLiteral("%1 is not empty").arg(target.path()));
        }
    }

    Call call;
    call.method = "DELETE";
    call.url = urlFor(target);
    const net::Response response = send(call, CancelToken());
    const VfsError error = net::errorFor(response, QStringLiteral("Deleting %1").arg(target.path()));
    if (error.isError())
        return Result<void>(error);
    return {};
}

Result<void> WebdavFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    const Result<FileEntry> source = stat(from);
    if (!source.ok())
        return Result<void>(source.error());

    Call call;
    call.method = "MOVE";
    call.url = urlFor(from);
    call.headers.append({ QByteArray("Destination"), urlFor(to) });
    // Without this the server would overwrite whatever is at the destination.
    call.headers.append({ QByteArray("Overwrite"), QByteArray("F") });

    const net::Response response = send(call, CancelToken());
    if (response.code == CURLE_OK && (response.status == 412 || response.status == 409)) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(to.path()));
    }

    const VfsError error = net::errorFor(response, QStringLiteral("Renaming %1").arg(from.path()));
    if (error.isError())
        return Result<void>(error);
    return {};
}

Result<std::unique_ptr<QIODevice>> WebdavFileSystem::openRead(const VfsUri& target, qint64)
{
    auto scratch = std::make_unique<QTemporaryFile>();
    QString staging;
    if (!staging::openFile(*scratch, &staging)) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError,
            QStringLiteral("Could not open a local copy for %1: %2").arg(target.path(), staging));
    }

    Call call;
    call.url = urlFor(target);
    const net::Response response = send(call, CancelToken(), scratch.get());
    const VfsError error = net::errorFor(response, QStringLiteral("Reading %1").arg(target.path()));
    if (error.isError())
        return Result<std::unique_ptr<QIODevice>>(error);

    // A GET on a collection is not an error, which is the problem. The server
    // redirects to the same path with a slash on the end and answers with an
    // HTML index of the directory -- 200, a body, and nothing in the response
    // saying it is not the file that was asked for. So a copy of a directory
    // produced an HTML page named after it, and a preview showed the same.
    //
    // The redirect is the tell, and it costs nothing: asking where the transfer
    // landed is free, where asking the server what kind of thing this is would
    // be a PROPFIND before every read.
    if (!response.effectiveUrl.isEmpty() && response.effectiveUrl.endsWith('/') && !call.url.endsWith('/')) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IsADirectory, QStringLiteral("%1 is a directory, not a file").arg(target.path()));
    }

    return net::openDownloadedFile(std::move(scratch));
}

Result<void> WebdavFileSystem::uploadFrom(
    const VfsUri& target, QIODevice& payload, qint64 size, const CancelToken& cancel)
{
    Call call;
    call.method = "PUT";
    call.url = urlFor(target);
    call.payload = &payload;
    call.payloadSize = size;

    const net::Response response = send(call, cancel);
    const VfsError error = net::errorFor(response, QStringLiteral("Writing %1").arg(target.path()));
    if (error.isError())
        return Result<void>(error);
    return {};
}

Result<std::unique_ptr<QIODevice>> WebdavFileSystem::openWrite(const VfsUri& target, qint64 expectedSize)
{
    // Big enough that staging it locally is the problem rather than the answer,
    // so it goes as it is written, with a chunked transfer encoding.
    //
    // Only for the large case, deliberately. WebDAV servers are not uniformly
    // happy with a chunked PUT -- some answer 411 and demand a length -- and a
    // small file has nothing to gain by finding that out. So a write whose size
    // is known and modest, or not known at all, keeps the staged PUT with an
    // exact Content-Length that has always worked; a write too large to stage
    // takes the only route there is.
    // Either way it goes up under a working name and is moved into place at the
    // end. A process killed mid-PUT does not get to delete what it wrote, and a
    // half-sent file under the name somebody asked for is the outcome worth
    // ruling out; WebDAV's MOVE with `Overwrite: F` is exactly the operation
    // this needs. See ADR-0020.
    const VfsUri staging = partialWriteOf(target);
    // A folder standing at the destination is refused before a byte goes over
    // the wire: it is not an old version of the file and there is nothing to
    // weigh up. The same call answers whether this is an overwrite, which only
    // an answer from before the write began can tell from a file that turned up
    // while this one was in flight. See MOLE-336.
    bool replacing = false;
    if (VfsError folder = refuseWritingOntoAFolder(*this, target, &replacing); folder.isError())
        return folder;
    auto commit = [this, staging, target, replacing] {
        return commitPartialWrite(*this, staging, target, replacing);
    };

    if (expectedSize > kStreamAbove) {
        auto send = [this, staging](QIODevice& source, qint64, bool, const CancelToken& cancel) {
            const Result<void> sent = uploadFrom(staging, source, -1, cancel);
            if (!sent.ok()) {
                remove(staging, false); // a part of a file, under a name nothing opens
                return sent.error();
            }
            return VfsError::ok();
        };
        // One span: HTTP has no way to append to what a previous request wrote,
        // so the whole object goes in a single PUT however long it takes.
        auto stream = std::make_unique<net::StreamingUpload>(
            std::move(send), std::numeric_limits<qint64>::max(), commit);
        if (!stream->open(QIODevice::WriteOnly))
            return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, stream->errorString());
        return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(stream.release()));
    }

    auto stream = std::make_unique<net::BufferedUpload>(
        [this, staging](QIODevice& payload, qint64 size) {
            const Result<void> sent = uploadFrom(staging, payload, size, CancelToken());
            if (!sent.ok())
                remove(staging, false);
            return sent;
        },
        std::move(commit));
    if (!stream->open(QIODevice::WriteOnly))
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, stream->errorString());
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(stream.release()));
}

// ---- factory ---------------------------------------------------------------

QList<ConnectionField> WebdavFileSystemFactory::connectionFields() const
{
    QList<ConnectionField> fields;

    ConnectionField url;
    url.key = QStringLiteral("url");
    url.label = QStringLiteral("Address");
    url.help = QStringLiteral("Full WebDAV url, for example "
                              "https://cloud.example.com/remote.php/dav/files/lukasz");
    fields.append(url);

    ConnectionField user;
    user.key = QStringLiteral("user");
    user.label = QStringLiteral("User");
    fields.append(user);

    ConnectionField password;
    password.key = QStringLiteral("password");
    password.label = QStringLiteral("Password");
    password.kind = ConnectionField::Password;
    password.help
        = QStringLiteral("On Nextcloud, generate an app password rather than using your account password");
    fields.append(password);

    ConnectionField verify;
    verify.key = QStringLiteral("verifyTls");
    verify.label = QStringLiteral("Verify the TLS certificate");
    verify.kind = ConnectionField::Boolean;
    verify.defaultValue = true;
    verify.required = false;
    verify.advanced = true;
    fields.append(verify);

    return fields;
}

WebdavSettings WebdavFileSystemFactory::settingsFrom(const QVariantMap& config)
{
    WebdavSettings settings;
    settings.baseUrl = config.value(QStringLiteral("url")).toString().trimmed();
    settings.username = config.value(QStringLiteral("user")).toString();
    settings.password = config.value(QStringLiteral("password")).toString();
    settings.verifyTls = config.value(QStringLiteral("verifyTls"), true).toBool();

    QString root = config.value(QStringLiteral("__root")).toString().trimmed();
    while (root.endsWith(QLatin1Char('/')))
        root.chop(1);
    settings.remoteRoot = root;

    return settings;
}

FileSystemPtr WebdavFileSystemFactory::create(const QVariantMap& config, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return FileSystemPtr {};
    };

    const WebdavSettings settings = settingsFrom(config);
    if (settings.baseUrl.isEmpty())
        return fail(QStringLiteral("A WebDAV drive needs an address"));

    // The scheme is checked before the host, because an address pasted without
    // one parses as a path with no host at all -- and "start with https://" is
    // something the reader can act on, where "that does not look like an address"
    // is not.
    const QUrl parsed(settings.baseUrl);
    if (parsed.scheme() != QLatin1String("https") && parsed.scheme() != QLatin1String("http"))
        return fail(QStringLiteral("A WebDAV address has to start with https:// or http://"));
    if (!parsed.isValid() || parsed.host().isEmpty())
        return fail(QStringLiteral("That does not look like a WebDAV address"));

    const QString scheme = config.value(QStringLiteral("__scheme"), QStringLiteral("webdav")).toString();
    return std::make_shared<WebdavFileSystem>(scheme, settings);
}

} // namespace mole
