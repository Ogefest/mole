#include "plugins/network/FtpFileSystem.h"

#include "plugins/network/TransferStreams.h"
#include "plugins/network/UnixListing.h"

#include <QTemporaryFile>

namespace mole {

FtpFileSystem::FtpFileSystem(QString scheme, FtpSettings settings)
    : m_scheme(std::move(scheme))
    , m_settings(std::move(settings))
{
    net::TransportOptions options;
    options.username = m_settings.username;
    options.password = m_settings.password;
    m_pool = std::make_unique<net::CurlPool>(std::move(options));
}

VfsCapabilities FtpFileSystem::capabilities() const
{
    // No ReportsAccess. A LIST line carries mode bits, but plenty of servers
    // invent them -- Windows and appliance FTP servers print a fixed string --
    // so they are shown in a listing and not offered as an answer about access.
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead;
}

QString FtpFileSystem::remotePath(const VfsUri& uri) const
{
    QString root = m_settings.remoteRoot;
    while (root.endsWith(QLatin1Char('/')))
        root.chop(1);
    const QString path = uri.path().isEmpty() ? QStringLiteral("/") : uri.path();
    const QString joined = root + path;
    return joined.isEmpty() ? QStringLiteral("/") : joined;
}

QByteArray FtpFileSystem::urlFor(const VfsUri& uri, bool asDirectory) const
{
    QByteArray url = "ftp://";
    url += m_settings.host.toUtf8();
    url += ':';
    url += QByteArray::number(m_settings.port);
    url += net::encodePath(remotePath(uri));
    if (asDirectory && !url.endsWith('/'))
        url += '/';
    return url;
}

void FtpFileSystem::applySettings(const net::CurlPool::Lease& lease) const
{
    CURL* handle = lease.get();
    switch (m_settings.security) {
    case FtpSettings::Security::Require:
        curl_easy_setopt(handle, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        break;
    case FtpSettings::Security::Try:
        curl_easy_setopt(handle, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_TRY));
        break;
    case FtpSettings::Security::None:
        curl_easy_setopt(handle, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_NONE));
        break;
    }
    curl_easy_setopt(handle, CURLOPT_FTP_USE_EPSV, m_settings.passive ? 1L : 0L);
    if (!m_settings.passive)
        curl_easy_setopt(handle, CURLOPT_FTPPORT, "-");
    // Creating a missing parent silently would turn a typo into a new tree.
    curl_easy_setopt(handle, CURLOPT_FTP_CREATE_MISSING_DIRS, 0L);
}

Result<FileEntryList> FtpFileSystem::listRaw(const VfsUri& dir, const CancelToken& cancel)
{
    auto lease = m_pool->take();
    if (!lease) {
        return Result<FileEntryList>::failure(
            VfsError::IoError, QStringLiteral("Could not start an FTP transfer"));
    }

    const QByteArray url = urlFor(dir, true);
    lease.setUrl(url);
    applySettings(lease);

    const net::Response response = m_pool->perform(lease, cancel);
    const VfsError error = net::errorFor(
        response, QStringLiteral("Listing %1").arg(dir.path()), net::StatusMeaning::ProtocolReply);
    if (error.isError())
        return Result<FileEntryList>(error);

    FileEntryList entries;
    const QDateTime now = QDateTime::currentDateTime();
    for (const net::ListingRow& row : net::parseUnixListing(response.body, now)) {
        if (net::isDotEntry(row))
            continue;
        FileEntry entry;
        entry.name = row.name;
        entry.uri = dir.child(row.name);
        entry.isDir = row.isDir;
        entry.isSymlink = row.isSymlink;
        entry.isHidden = row.name.startsWith(QLatin1Char('.'));
        entry.size = row.size;
        entry.modified = row.modified;
        entry.permissions = row.permissions;
        entries.append(entry);
    }
    return Result<FileEntryList>(entries);
}

Result<FileEntryList> FtpFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    Result<FileEntryList> listing = listRaw(dir, cancel);

    if (!listing.ok()) {
        switch (listing.error().code) {
        case VfsError::Cancelled:
        case VfsError::NotFound:
            return listing;
        default:
            break;
        }
        const Result<FileEntry> what = stat(dir);
        if (!what.ok())
            return Result<FileEntryList>(what.error());
        if (!what.value().isDir) {
            return Result<FileEntryList>::failure(
                VfsError::NotADirectory, QStringLiteral("%1 is a file, not a directory").arg(dir.path()));
        }
        return listing;
    }

    // A server asked to LIST a file usually prints that file's own line instead
    // of refusing, which would arrive here as a directory containing one entry.
    // The shape is indistinguishable from a real directory, so it is only worth
    // a second look when it actually occurs.
    if (!dir.isRoot() && listing.value().size() == 1 && listing.value().first().name == dir.fileName()) {
        const Result<FileEntry> what = stat(dir);
        if (what.ok() && !what.value().isDir) {
            return Result<FileEntryList>::failure(
                VfsError::NotADirectory, QStringLiteral("%1 is a file, not a directory").arg(dir.path()));
        }
    }
    return listing;
}

Result<FileEntry> FtpFileSystem::stat(const VfsUri& target)
{
    if (target.isRoot()) {
        const Result<FileEntryList> reachable = listRaw(target, CancelToken());
        if (!reachable.ok())
            return Result<FileEntry>(reachable.error());

        FileEntry entry;
        entry.uri = target;
        entry.isDir = true;
        return Result<FileEntry>(entry);
    }

    const Result<FileEntryList> siblings = listRaw(target.parent(), CancelToken());
    if (!siblings.ok())
        return Result<FileEntry>(siblings.error());

    const QString wanted = target.fileName();
    for (const FileEntry& entry : siblings.value()) {
        if (entry.name == wanted)
            return Result<FileEntry>(entry);
    }
    return Result<FileEntry>::failure(
        VfsError::NotFound, QStringLiteral("%1 does not exist").arg(target.path()));
}

Result<void> FtpFileSystem::runCommands(
    const QList<QByteArray>& commands, const VfsUri& context, const QString& what)
{
    auto lease = m_pool->take();
    if (!lease)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Could not start an FTP transfer"));

    const QByteArray url = urlFor(context.isRoot() ? context : context.parent(), true);
    curl_slist* list = nullptr;
    for (const QByteArray& command : commands)
        list = curl_slist_append(list, command.constData());

    lease.setUrl(url);
    curl_easy_setopt(lease.get(), CURLOPT_QUOTE, list);
    curl_easy_setopt(lease.get(), CURLOPT_NOBODY, 1L);
    applySettings(lease);

    const net::Response response = m_pool->perform(lease, CancelToken());
    curl_slist_free_all(list);

    const VfsError error = net::errorFor(response, what, net::StatusMeaning::ProtocolReply);
    if (error.isError())
        return Result<void>(error);
    return {};
}

Result<void> FtpFileSystem::makeDirectory(const VfsUri& target)
{
    if (stat(target).ok()) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(target.path()));
    }
    return runCommands(
        { "MKD " + remotePath(target).toUtf8() }, target, QStringLiteral("Creating %1").arg(target.path()));
}

Result<void> FtpFileSystem::remove(const VfsUri& target, bool recursive)
{
    const Result<FileEntry> what = stat(target);
    if (!what.ok())
        return Result<void>(what.error());

    if (!what.value().isDir) {
        return runCommands({ "DELE " + remotePath(target).toUtf8() }, target,
            QStringLiteral("Deleting %1").arg(target.path()));
    }

    const Result<FileEntryList> children = listRaw(target, CancelToken());
    if (!children.ok())
        return Result<void>(children.error());

    if (!children.value().isEmpty()) {
        if (!recursive) {
            return Result<void>::failure(
                VfsError::NotEmpty, QStringLiteral("%1 is not empty").arg(target.path()));
        }
        for (const FileEntry& child : children.value()) {
            const Result<void> removed = remove(child.uri, true);
            if (!removed.ok())
                return removed;
        }
    }

    return runCommands(
        { "RMD " + remotePath(target).toUtf8() }, target, QStringLiteral("Deleting %1").arg(target.path()));
}

Result<void> FtpFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    if (stat(to).ok()) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(to.path()));
    }
    // RNFR then RNTO, which is the only rename FTP has and has to arrive as one
    // pair on one connection.
    return runCommands({ "RNFR " + remotePath(from).toUtf8(), "RNTO " + remotePath(to).toUtf8() }, from,
        QStringLiteral("Renaming %1").arg(from.path()));
}

Result<std::unique_ptr<QIODevice>> FtpFileSystem::openRead(const VfsUri& target, qint64)
{
    auto scratch = std::make_unique<QTemporaryFile>();
    if (!scratch->open()) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IoError, QStringLiteral("Could not open a local copy for %1").arg(target.path()));
    }

    auto lease = m_pool->take();
    if (!lease) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IoError, QStringLiteral("Could not start an FTP transfer"));
    }

    const QByteArray url = urlFor(target, false);
    lease.setUrl(url);
    applySettings(lease);

    const net::Response response = m_pool->perform(lease, CancelToken(), scratch.get());
    const VfsError error = net::errorFor(
        response, QStringLiteral("Reading %1").arg(target.path()), net::StatusMeaning::ProtocolReply);
    if (error.isError())
        return Result<std::unique_ptr<QIODevice>>(error);

    return net::openDownloadedFile(std::move(scratch));
}

Result<void> FtpFileSystem::uploadTo(const VfsUri& target, QIODevice& payload, qint64 size)
{
    auto lease = m_pool->take();
    if (!lease)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Could not start an FTP transfer"));

    const QByteArray url = urlFor(target, false);
    lease.setUrl(url);
    applySettings(lease);
    net::CurlPool::sendFrom(lease, payload, size);

    const net::Response response = m_pool->perform(lease, CancelToken());
    const VfsError error = net::errorFor(
        response, QStringLiteral("Writing %1").arg(target.path()), net::StatusMeaning::ProtocolReply);
    if (error.isError())
        return Result<void>(error);
    return {};
}

Result<std::unique_ptr<QIODevice>> FtpFileSystem::openWrite(const VfsUri& target)
{
    auto stream = std::make_unique<net::BufferedUpload>(
        [this, target](QIODevice& payload, qint64 size) { return uploadTo(target, payload, size); });
    if (!stream->open(QIODevice::WriteOnly))
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, stream->errorString());
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(stream.release()));
}

// ---- factory ---------------------------------------------------------------

QList<ConnectionField> FtpFileSystemFactory::connectionFields() const
{
    QList<ConnectionField> fields;

    ConnectionField host;
    host.key = QStringLiteral("host");
    host.label = QStringLiteral("Host");
    fields.append(host);

    ConnectionField user;
    user.key = QStringLiteral("user");
    user.label = QStringLiteral("User");
    user.help = QStringLiteral("Use \"anonymous\" for a public server");
    fields.append(user);

    ConnectionField password;
    password.key = QStringLiteral("password");
    password.label = QStringLiteral("Password");
    password.kind = ConnectionField::Password;
    password.required = false;
    fields.append(password);

    ConnectionField security;
    security.key = QStringLiteral("security");
    security.label = QStringLiteral("Encryption");
    security.kind = ConnectionField::Choice;
    security.choices = { QStringLiteral("try"), QStringLiteral("require"), QStringLiteral("none") };
    security.choiceLabels = { QStringLiteral("Use TLS if offered"), QStringLiteral("Require TLS"),
        QStringLiteral("Never use TLS") };
    security.defaultValue = QStringLiteral("try");
    security.required = false;
    security.help = QStringLiteral(
        "FTP sends its password in plain text. Require TLS whenever the server supports it.");
    fields.append(security);

    ConnectionField port;
    port.key = QStringLiteral("port");
    port.label = QStringLiteral("Port");
    port.kind = ConnectionField::Number;
    port.defaultValue = 21;
    port.required = false;
    port.advanced = true;
    fields.append(port);

    ConnectionField passive;
    passive.key = QStringLiteral("passive");
    passive.label = QStringLiteral("Passive mode");
    passive.kind = ConnectionField::Boolean;
    passive.defaultValue = true;
    passive.required = false;
    passive.advanced = true;
    passive.help = QStringLiteral("Leave on unless the server specifically needs active mode");
    fields.append(passive);

    return fields;
}

FtpSettings FtpFileSystemFactory::settingsFrom(const QVariantMap& config)
{
    FtpSettings settings;
    settings.host = config.value(QStringLiteral("host")).toString().trimmed();
    settings.username = config.value(QStringLiteral("user")).toString();
    settings.password = config.value(QStringLiteral("password")).toString();

    const int port = config.value(QStringLiteral("port")).toInt();
    settings.port = port > 0 ? port : 21;

    const QString security = config.value(QStringLiteral("security")).toString();
    if (security == QLatin1String("require"))
        settings.security = FtpSettings::Security::Require;
    else if (security == QLatin1String("none"))
        settings.security = FtpSettings::Security::None;
    else
        settings.security = FtpSettings::Security::Try;

    settings.passive = config.value(QStringLiteral("passive"), true).toBool();

    QString root = config.value(QStringLiteral("__root")).toString().trimmed();
    if (root.isEmpty())
        root = QStringLiteral("/");
    if (!root.startsWith(QLatin1Char('/')))
        root.prepend(QLatin1Char('/'));
    settings.remoteRoot = root;

    return settings;
}

FileSystemPtr FtpFileSystemFactory::create(const QVariantMap& config, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return FileSystemPtr {};
    };

    FtpSettings settings = settingsFrom(config);
    if (settings.host.isEmpty())
        return fail(QStringLiteral("An FTP drive needs a host"));
    if (settings.username.isEmpty())
        settings.username = QStringLiteral("anonymous");

    const QString scheme = config.value(QStringLiteral("__scheme"), QStringLiteral("ftp")).toString();
    return std::make_shared<FtpFileSystem>(scheme, settings);
}

} // namespace mole
