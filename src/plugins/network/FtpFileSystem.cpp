#include "plugins/network/FtpFileSystem.h"

#include "plugins/network/TransferStreams.h"
#include "plugins/network/UnixListing.h"

#include <QTemporaryFile>

namespace mole {
namespace {

    /// How much one transfer of a streamed upload carries before the next one
    /// appends to it.
    ///
    /// Unlike SFTP, FTP has no reason to want several: the span loop there exists
    /// to keep every connection clear of an SSH re-key fault, and paying for a
    /// login and a data channel per span here would buy nothing. So this is a
    /// ceiling rather than a working figure -- past any file anybody will hand
    /// this, and present rather than removed so that a file which does exceed it
    /// continues with APPE instead of failing.
    constexpr qint64 kUploadSpanBytes = 1024LL * 1024 * 1024 * 1024;

} // namespace

FtpFileSystem::FtpFileSystem(QString scheme, FtpSettings settings)
    : m_scheme(std::move(scheme))
    , m_settings(std::move(settings))
{
    net::TransportOptions options;
    options.username = m_settings.username;
    options.password = m_settings.password;
    options.verifyTls = m_settings.verifyTls;
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

VfsError FtpFileSystem::sendSpan(
    const VfsUri& target, QIODevice& source, bool append, const CancelToken& cancel)
{
    auto lease = m_pool->take();
    if (!lease)
        return VfsError::make(VfsError::IoError, QStringLiteral("Could not start an FTP transfer"));

    lease.setUrl(urlFor(target, false));
    applySettings(lease);
    if (append) {
        // APPE rather than STOR, which is how a second span continues the file
        // instead of replacing it. Every server that can be written to at all
        // has it; it is in the protocol beside STOR rather than an extension.
        curl_easy_setopt(lease.get(), CURLOPT_APPEND, 1L);
    }
    // No length: a stream being written as it is sent does not know one, and
    // FTP does not need one -- the data connection closing is the end of the file.
    net::CurlPool::sendFrom(lease, source, -1);

    const net::Response response = m_pool->perform(lease, cancel);
    const VfsError error = net::errorFor(
        response, QStringLiteral("Writing %1").arg(target.path()), net::StatusMeaning::ProtocolReply);

    if (error.isError()) {
        // What arrived is part of a file, and leaving it would be litter under a
        // name nothing will open. Always the working name openWrite() invented
        // for this transfer, so removing it takes nothing that was not ours.
        remove(target, false);
    }
    return error;
}

Result<std::unique_ptr<QIODevice>> FtpFileSystem::openWrite(const VfsUri& target, qint64)
{
    // Streamed rather than staged, which is what makes a file bigger than the
    // local disk writable at all: staging collected the whole payload into a
    // temporary file before sending any of it, so a 200 GB upload wanted 200 GB
    // of local scratch space for a transfer whose destination had room to spare.
    // FTP was the last backend still doing that -- see ADR-0014.
    //
    // Under a working name until it is finished, because a process killed
    // mid-transfer does not get to delete what it wrote, and a half-sent file
    // under the name somebody asked for is the one outcome worth ruling out. The
    // rename at the end is a single server-side operation. See ADR-0020.
    const VfsUri staging = partialWriteOf(target);
    // Asked before the transfer, not after it: only an answer from before
    // the write began can tell an overwrite from a file that turned up while
    // this one was going over the wire.
    const bool replacing = stat(target).ok();

    auto send = [this, staging](QIODevice& source, qint64, bool append, const CancelToken& cancel) {
        return sendSpan(staging, source, append, cancel);
    };
    auto commit = [this, staging, target, replacing] {
        return commitPartialWrite(*this, staging, target, replacing);
    };

    auto stream
        = std::make_unique<net::StreamingUpload>(std::move(send), kUploadSpanBytes, std::move(commit));
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

    settings.verifyTls = config.value(QStringLiteral("verifyTls"), true).toBool();

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
