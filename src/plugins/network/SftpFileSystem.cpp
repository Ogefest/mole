#include "plugins/network/SftpFileSystem.h"

#include "plugins/network/TransferStreams.h"
#include "plugins/network/UnixListing.h"

#include <QDir>
#include <QTemporaryFile>

namespace mole {
namespace {

    /// Wraps a path for an SFTP quote command. curl's own parser understands
    /// double quotes with backslash escapes, which is the only way a name with a
    /// space in it survives being sent as part of a command line.
    QByteArray quoted(const QString& path)
    {
        QString escaped = path;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return '"' + escaped.toUtf8() + '"';
    }

    QString defaultKnownHosts()
    {
        return QDir::homePath() + QStringLiteral("/.ssh/known_hosts");
    }

    /// How much of a large file one connection is asked to carry.
    ///
    /// An SFTP transfer stops dead a little short of a gibibyte: the bytes
    /// arrive at full speed and then simply cease, with the connection open, the
    /// server there, and libcurl reporting nothing at all until the stall guard
    /// gives up two minutes later. It happens to plain `curl` with no Mole
    /// involved, on a connection nothing else has touched, and always around the
    /// same place -- which is where an OpenSSH server with a small-block cipher
    /// re-keys the session. OpenSSH's own client and `scp` carry any size, so it
    /// is the pairing rather than either end alone.
    ///
    /// Fetching in spans keeps every connection well clear of it, at the cost of
    /// an SSH handshake per span -- about half a second per quarter gigabyte,
    /// which is nothing next to the transfer it carries. A file that fits in one
    /// span is fetched over a pooled connection exactly as before, because a
    /// handshake for each of ten thousand small files is not nothing at all.
    constexpr qint64 kSpanBytes = 256LL * 1024 * 1024;

} // namespace

SftpFileSystem::SftpFileSystem(QString scheme, SftpSettings settings)
    : m_scheme(std::move(scheme))
    , m_settings(std::move(settings))
{
    net::TransportOptions options;
    options.username = m_settings.username;
    options.password = m_settings.password;
    options.knownHostsPath = defaultKnownHosts();
    options.acceptUnknownHostKey = m_settings.acceptNewHostKey;
    m_pool = std::make_unique<net::CurlPool>(std::move(options));
}

VfsCapabilities SftpFileSystem::capabilities() const
{
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead
        | VfsCapability::ReportsAccess;
}

QString SftpFileSystem::remotePath(const VfsUri& uri) const
{
    QString root = m_settings.remoteRoot;
    while (root.endsWith(QLatin1Char('/')))
        root.chop(1);
    const QString path = uri.path().isEmpty() ? QStringLiteral("/") : uri.path();
    const QString joined = root + path;
    return joined.isEmpty() ? QStringLiteral("/") : joined;
}

QByteArray SftpFileSystem::urlFor(const VfsUri& uri, bool asDirectory) const
{
    QByteArray url = "sftp://";
    url += m_settings.host.toUtf8();
    url += ':';
    url += QByteArray::number(m_settings.port);
    url += net::encodePath(remotePath(uri));
    if (asDirectory && !url.endsWith('/'))
        url += '/';
    return url;
}

Result<FileEntryList> SftpFileSystem::listRaw(const VfsUri& dir, const CancelToken& cancel)
{
    auto lease = m_pool->take();
    if (!lease) {
        return Result<FileEntryList>::failure(
            VfsError::IoError, QStringLiteral("Could not start an SFTP transfer"));
    }

    const QByteArray url = urlFor(dir, true);
    lease.setUrl(url);
    if (!m_settings.privateKeyPath.isEmpty()) {
        curl_easy_setopt(
            lease.get(), CURLOPT_SSH_PRIVATE_KEYFILE, m_settings.privateKeyPath.toUtf8().constData());
        if (!m_settings.privateKeyPassphrase.isEmpty()) {
            curl_easy_setopt(
                lease.get(), CURLOPT_KEYPASSWD, m_settings.privateKeyPassphrase.toUtf8().constData());
        }
    }

    const net::Response response = m_pool->perform(lease, cancel);
    const VfsError error = net::errorFor(
        response, QStringLiteral("Listing %1").arg(dir.path()), net::StatusMeaning::ProtocolReply);
    if (error.isError())
        return Result<FileEntryList>(error);

    FileEntryList entries;
    const QDateTime now = QDateTime::currentDateTime();
    for (const net::ListingRow& row : net::parseUnixListing(response.body, now)) {
        if (net::isDotEntry(row)) {
            // Asked for a regular file with a trailing slash, a server does not
            // refuse -- it answers with a "." describing the file itself. Without
            // this the caller would be handed an empty directory instead of being
            // told it asked about a file.
            if (row.name == QLatin1String(".") && !row.isDir) {
                return Result<FileEntryList>::failure(
                    VfsError::NotADirectory, QStringLiteral("%1 is a file, not a directory").arg(dir.path()));
            }
            continue;
        }

        FileEntry entry;
        entry.name = row.name;
        entry.uri = dir.child(row.name);
        entry.isDir = row.isDir;
        entry.isSymlink = row.isSymlink;
        entry.isHidden = row.name.startsWith(QLatin1Char('.'));
        entry.size = row.size;
        entry.modified = row.modified;
        entry.permissions = row.permissions;
        // Only what the mode bits actually claim. Whether *this* account is the
        // owner is not something a listing says, so nothing is inferred from it.
        entry.isReadable = row.permissions.contains(QLatin1Char('r'));
        entry.isWritable = row.permissions.contains(QLatin1Char('w'));
        entries.append(entry);
    }
    return Result<FileEntryList>(entries);
}

Result<FileEntryList> SftpFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    Result<FileEntryList> listing = listRaw(dir, cancel);
    if (listing.ok())
        return listing;

    switch (listing.error().code) {
    case VfsError::Cancelled:
    case VfsError::NotADirectory:
    case VfsError::NotFound:
        // Already unambiguous; asking again would only cost a round trip.
        return listing;
    default:
        break;
    }

    // Otherwise the protocol has said "that did not work" without saying why, so
    // the reason is established here: a file rather than a directory, or nothing
    // at all. Servers differ on which of these they report properly, and the
    // layers above must not have to care which kind they are talking to.
    const Result<FileEntry> what = stat(dir);
    if (!what.ok())
        return Result<FileEntryList>(what.error());
    if (!what.value().isDir) {
        return Result<FileEntryList>::failure(
            VfsError::NotADirectory, QStringLiteral("%1 is a file, not a directory").arg(dir.path()));
    }
    return listing;
}

Result<FileEntry> SftpFileSystem::stat(const VfsUri& target)
{
    if (target.isRoot()) {
        // The root has no parent to be listed out of, so its existence is
        // established by reaching it.
        const Result<FileEntryList> reachable = listRaw(target, CancelToken());
        if (!reachable.ok())
            return Result<FileEntry>(reachable.error());

        FileEntry entry;
        entry.name = QString();
        entry.uri = target;
        entry.isDir = true;
        entry.isReadable = true;
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

Result<void> SftpFileSystem::runCommand(const QByteArray& command, const VfsUri& context, const QString& what)
{
    auto lease = m_pool->take();
    if (!lease)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Could not start an SFTP transfer"));

    // The url has to be somewhere that exists even though the command carries its
    // own path, so the parent directory is used -- it is the one place we already
    // know is there.
    const QByteArray url = urlFor(context.isRoot() ? context : context.parent(), true);
    curl_slist* commands = curl_slist_append(nullptr, command.constData());

    lease.setUrl(url);
    curl_easy_setopt(lease.get(), CURLOPT_QUOTE, commands);
    curl_easy_setopt(lease.get(), CURLOPT_NOBODY, 1L);

    const net::Response response = m_pool->perform(lease, CancelToken());
    curl_slist_free_all(commands);

    const VfsError error = net::errorFor(response, what, net::StatusMeaning::ProtocolReply);
    if (error.isError())
        return Result<void>(error);
    return {};
}

Result<void> SftpFileSystem::makeDirectory(const VfsUri& target)
{
    if (stat(target).ok()) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(target.path()));
    }
    return runCommand(
        "mkdir " + quoted(remotePath(target)), target, QStringLiteral("Creating %1").arg(target.path()));
}

Result<void> SftpFileSystem::remove(const VfsUri& target, bool recursive)
{
    const Result<FileEntry> what = stat(target);
    if (!what.ok())
        return Result<void>(what.error());

    if (!what.value().isDir) {
        return runCommand(
            "rm " + quoted(remotePath(target)), target, QStringLiteral("Deleting %1").arg(target.path()));
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

    return runCommand(
        "rmdir " + quoted(remotePath(target)), target, QStringLiteral("Deleting %1").arg(target.path()));
}

Result<void> SftpFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    if (stat(to).ok()) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(to.path()));
    }
    const QByteArray command = "rename " + quoted(remotePath(from)) + ' ' + quoted(remotePath(to));
    return runCommand(command, from, QStringLiteral("Renaming %1").arg(from.path()));
}

Result<std::unique_ptr<QIODevice>> SftpFileSystem::openRead(const VfsUri& target, qint64 expectedSize)
{
    auto scratch = std::make_unique<QTemporaryFile>();
    if (!scratch->open()) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IoError, QStringLiteral("Could not open a local copy for %1").arg(target.path()));
    }

    const QByteArray url = urlFor(target, false);
    const QString what = QStringLiteral("Reading %1").arg(target.path());

    // Small and measured: one transfer over a warm connection, as it always was.
    // Unknown counts as large -- a caller that did not say is reading one file
    // for a preview or a checksum, where a handshake costs nothing worth saving.
    if (expectedSize >= 0 && expectedSize <= kSpanBytes) {
        auto lease = m_pool->take();
        if (!lease) {
            return Result<std::unique_ptr<QIODevice>>::failure(
                VfsError::IoError, QStringLiteral("Could not start an SFTP transfer"));
        }
        lease.setUrl(url);

        const net::Response response = m_pool->perform(lease, CancelToken(), scratch.get());
        const VfsError error = net::errorFor(response, what, net::StatusMeaning::ProtocolReply);
        if (error.isError())
            return Result<std::unique_ptr<QIODevice>>(error);

        return net::openDownloadedFile(std::move(scratch));
    }

    // Anything larger arrives a span at a time, each over a connection of its
    // own, appended to the same local copy.
    qint64 offset = 0;
    for (;;) {
        auto lease = m_pool->takeFresh();
        if (!lease) {
            return Result<std::unique_ptr<QIODevice>>::failure(
                VfsError::IoError, QStringLiteral("Could not start an SFTP transfer"));
        }
        lease.setUrl(url);

        const QByteArray range
            = QByteArray::number(offset) + '-' + QByteArray::number(offset + kSpanBytes - 1);
        curl_easy_setopt(lease.get(), CURLOPT_RANGE, range.constData());

        const net::Response response = m_pool->perform(lease, CancelToken(), scratch.get());
        const VfsError error = net::errorFor(response, what, net::StatusMeaning::ProtocolReply);
        if (error.isError())
            return Result<std::unique_ptr<QIODevice>>(error);

        const qint64 arrived = response.receivedBytes < 0 ? 0 : response.receivedBytes;
        offset += arrived;

        // A span that comes back short is the end of the file: a server clamps a
        // range that runs past the end rather than refusing it. One that comes
        // back long is a server that ignored the range and sent everything,
        // which means the file is already here.
        if (arrived < kSpanBytes || arrived > kSpanBytes)
            break;
        if (expectedSize > 0 && offset >= expectedSize)
            break;
    }

    return net::openDownloadedFile(std::move(scratch));
}

Result<void> SftpFileSystem::uploadTo(const VfsUri& target, QIODevice& payload, qint64 size)
{
    auto lease = m_pool->take();
    if (!lease)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Could not start an SFTP transfer"));

    const QByteArray url = urlFor(target, false);
    lease.setUrl(url);
    // Going the other way there is no equivalent of a range, so all that can be
    // done is to keep the transfer off a connection that has already been used.
    // An upload past the point where the session re-keys is still unfinished
    // business -- see TODO.md.
    if (size > kSpanBytes)
        lease.useOwnConnection();
    net::CurlPool::sendFrom(lease, payload, size);

    const net::Response response = m_pool->perform(lease, CancelToken());
    const VfsError error = net::errorFor(
        response, QStringLiteral("Writing %1").arg(target.path()), net::StatusMeaning::ProtocolReply);
    if (error.isError())
        return Result<void>(error);
    return {};
}

Result<std::unique_ptr<QIODevice>> SftpFileSystem::openWrite(const VfsUri& target)
{
    auto stream = std::make_unique<net::BufferedUpload>(
        [this, target](QIODevice& payload, qint64 size) { return uploadTo(target, payload, size); });
    if (!stream->open(QIODevice::WriteOnly)) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, stream->errorString());
    }
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(stream.release()));
}

Result<AccessInfo> SftpFileSystem::access(const VfsUri& target)
{
    const Result<FileEntry> what = stat(target);
    if (!what.ok())
        return Result<AccessInfo>(what.error());

    const FileEntry& entry = what.value();
    AccessInfo info;
    info.owner = QString();
    info.group = QString();
    info.nativeText = entry.permissions;

    if (entry.permissions.isEmpty()) {
        // The drive root, which was reached rather than listed. That it could be
        // listed at all is the one thing we do know.
        info.read = AccessInfo::Answer::Yes;
        return Result<AccessInfo>(info);
    }

    // Reported as what the mode bits say, not as a verdict about this account:
    // the listing does not reveal whether we are the owner, and a guess dressed
    // up as an answer is worse than the mode string on its own.
    info.read = entry.isReadable ? AccessInfo::Answer::Yes : AccessInfo::Answer::Unknown;
    info.write = entry.isWritable ? AccessInfo::Answer::Yes : AccessInfo::Answer::Unknown;
    info.createInside = entry.isDir ? info.write : AccessInfo::Answer::Unknown;
    return Result<AccessInfo>(info);
}

// ---- factory ---------------------------------------------------------------

QList<ConnectionField> SftpFileSystemFactory::connectionFields() const
{
    QList<ConnectionField> fields;

    ConnectionField host;
    host.key = QStringLiteral("host");
    host.label = QStringLiteral("Host");
    host.help = QStringLiteral("Name or address of the SSH server, e.g. nas.local");
    fields.append(host);

    ConnectionField user;
    user.key = QStringLiteral("user");
    user.label = QStringLiteral("User");
    user.help = QStringLiteral("The account to log in as");
    fields.append(user);

    ConnectionField password;
    password.key = QStringLiteral("password");
    password.label = QStringLiteral("Password");
    password.kind = ConnectionField::Password;
    password.required = false;
    password.help = QStringLiteral("Leave empty when logging in with a key");
    fields.append(password);

    ConnectionField port;
    port.key = QStringLiteral("port");
    port.label = QStringLiteral("Port");
    port.kind = ConnectionField::Number;
    port.defaultValue = 22;
    port.required = false;
    port.advanced = true;
    fields.append(port);

    ConnectionField key;
    key.key = QStringLiteral("privateKey");
    key.label = QStringLiteral("Private key file");
    key.required = false;
    key.advanced = true;
    key.help = QStringLiteral("Path to a private key, e.g. ~/.ssh/id_ed25519");
    fields.append(key);

    ConnectionField passphrase;
    passphrase.key = QStringLiteral("keyPassphrase");
    passphrase.label = QStringLiteral("Key passphrase");
    passphrase.kind = ConnectionField::Password;
    passphrase.required = false;
    passphrase.advanced = true;
    fields.append(passphrase);

    ConnectionField trust;
    trust.key = QStringLiteral("acceptNewHostKey");
    trust.label = QStringLiteral("Trust a host key seen for the first time");
    trust.kind = ConnectionField::Boolean;
    trust.defaultValue = true;
    trust.required = false;
    trust.advanced = true;
    trust.help
        = QStringLiteral("Records the key in ~/.ssh/known_hosts on first connection. A host whose key later "
                         "changes is refused either way.");
    fields.append(trust);

    return fields;
}

SftpSettings SftpFileSystemFactory::settingsFrom(const QVariantMap& config)
{
    SftpSettings settings;
    settings.host = config.value(QStringLiteral("host")).toString().trimmed();
    settings.username = config.value(QStringLiteral("user")).toString();
    settings.password = config.value(QStringLiteral("password")).toString();
    settings.privateKeyPath = config.value(QStringLiteral("privateKey")).toString();
    settings.privateKeyPassphrase = config.value(QStringLiteral("keyPassphrase")).toString();

    const int port = config.value(QStringLiteral("port")).toInt();
    settings.port = port > 0 ? port : 22;

    QString root = config.value(QStringLiteral("__root")).toString().trimmed();
    if (root.isEmpty())
        root = QStringLiteral("/");
    if (!root.startsWith(QLatin1Char('/')))
        root.prepend(QLatin1Char('/'));
    settings.remoteRoot = root;

    settings.acceptNewHostKey = config.value(QStringLiteral("acceptNewHostKey"), true).toBool();
    return settings;
}

FileSystemPtr SftpFileSystemFactory::create(const QVariantMap& config, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return FileSystemPtr {};
    };

    const SftpSettings settings = settingsFrom(config);
    if (settings.host.isEmpty())
        return fail(QStringLiteral("An SFTP drive needs a host"));
    if (settings.username.isEmpty())
        return fail(QStringLiteral("An SFTP drive needs a user name"));
    if (settings.password.isEmpty() && settings.privateKeyPath.isEmpty())
        return fail(QStringLiteral("An SFTP drive needs either a password or a private key"));

    const QString scheme = config.value(QStringLiteral("__scheme"), QStringLiteral("sftp")).toString();
    return std::make_shared<SftpFileSystem>(scheme, settings);
}

} // namespace mole
