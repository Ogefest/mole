#include "plugins/network/FtpFileSystem.h"

#include "plugins/network/TransferStreams.h"
#include "plugins/network/UnixListing.h"

#include "core/platform/Staging.h"

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

    /// The same figure, and the same reasoning, for a streamed read: one ranged
    /// fetch carries the file and the stream clamps the span to what is left of
    /// it, so in practice there is one transfer per read.
    constexpr qint64 kDownloadSpanBytes = kUploadSpanBytes;

    /// Below this a read is fetched whole into a temporary file, as every FTP
    /// read used to be. A small file over a pooled connection costs one request
    /// and no thread, where streaming it would cost a stat to learn its length
    /// and a thread to carry it -- and nothing is at risk, because what made
    /// staging a fault is a file too big to stage.
    ///
    /// The same figure SFTP uses, deliberately: two backends disagreeing about
    /// when a file is large would be two behaviours to explain.
    constexpr qint64 kFetchWholeBelow = 64LL * 1024 * 1024;

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

    // `/%2F` first, because libcurl implements RFC 1738 for FTP: a path in the
    // URL is a sequence of CWDs **relative to the login directory**, and an
    // absolute path has to be spelled with an escaped slash. The QUOTE commands
    // beside these urls -- MKD, DELE, RMD, RNFR/RNTO -- carry the absolute path,
    // so the two agreed only while the login directory happened to be "/". That
    // is true of the chrooted account the live suite uses and false of an
    // ordinary one whose home is /home/alice: the listing showed
    // /home/alice/notes.txt, `DELE /notes.txt` went for the root's, and
    // commitPartialWrite()'s rename failed at the end of every upload -- so every
    // upload was removed as litter. See MOLE-349.
    const QString path = remotePath(uri);
    url += "/%2F";
    if (path.size() > 1)
        url += net::encodePath(path.mid(1));
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

net::Response FtpFileSystem::fetchListing(const VfsUri& dir, const CancelToken& cancel, bool machineReadable)
{
    auto lease = m_pool->take();
    if (!lease) {
        net::Response failed;
        failed.code = CURLE_FAILED_INIT;
        failed.detail = QStringLiteral("could not start an FTP transfer");
        return failed;
    }

    lease.setUrl(urlFor(dir, true));
    // MLSD instead of LIST. A server that does not know it answers 500 or 502,
    // which is a reply and not a broken connection, so the caller can try the
    // human format instead.
    if (machineReadable)
        curl_easy_setopt(lease.get(), CURLOPT_CUSTOMREQUEST, "MLSD");
    applySettings(lease);

    net::Response response = m_pool->perform(lease, cancel);
    if (machineReadable)
        curl_easy_setopt(lease.get(), CURLOPT_CUSTOMREQUEST, nullptr);
    return response;
}

Result<QList<net::ListingRow>> FtpFileSystem::rowsFrom(
    const QByteArray& body, bool machineReadable, const QDateTime& now)
{
    const QList<net::ListingRow> rows
        = machineReadable ? net::parseMlsdListing(body, now) : net::parseUnixListing(body, now);

    // A body that said something and parsed to nothing is a format this build
    // does not understand, and the one answer it must not be turned into is "the
    // directory is empty". That sentence is what a mirror deletes against, and it
    // is what an IIS server in its default mode -- or any appliance printing its
    // own thing -- used to produce. An empty body is a different matter: that
    // really is an empty directory. See MOLE-349.
    if (rows.isEmpty() && !body.trimmed().isEmpty()) {
        return Result<QList<net::ListingRow>>::failure(
            VfsError::IoError, QStringLiteral("the listing format is not understood"));
    }
    return Result<QList<net::ListingRow>>(rows);
}

bool FtpFileSystem::commandIsSendable(const QByteArray& command)
{
    return !command.contains('\r') && !command.contains('\n');
}

Result<FileEntryList> FtpFileSystem::listRaw(const VfsUri& dir, const CancelToken& cancel)
{
    // MLSD first. `ls -l` is a format meant for a person to read: its columns are
    // the server's locale, a date more than six months old carries no year, and
    // IIS in its default mode prints something else entirely. The parser drops
    // what it cannot read, so a non-empty listing from any of those became an
    // empty directory -- which a mirror deletes against and a copy finds nothing
    // to do. MLSD is machine-readable, in UTC and in no locale, and every server
    // written this century answers it. See RFC 3659 and MOLE-349.
    bool wasMachineReadable = true;
    net::Response response = fetchListing(dir, cancel, true);
    VfsError error = net::errorFor(
        response, QStringLiteral("Listing %1").arg(dir.path()), net::StatusMeaning::ProtocolReply);

    if (error.isError() && !net::wasCancelled(response)) {
        wasMachineReadable = false;
        response = fetchListing(dir, cancel, false);
        error = net::errorFor(
            response, QStringLiteral("Listing %1").arg(dir.path()), net::StatusMeaning::ProtocolReply);
    }
    if (error.isError())
        return Result<FileEntryList>(error);

    const Result<QList<net::ListingRow>> parsed
        = rowsFrom(response.body, wasMachineReadable, QDateTime::currentDateTime());
    if (!parsed.ok()) {
        return Result<FileEntryList>::failure(
            parsed.error().code, QStringLiteral("Listing %1: %2").arg(dir.path(), parsed.error().message));
    }

    FileEntryList entries;
    for (const net::ListingRow& row : parsed.value()) {
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
        // Named rather than defaulted, so a twelfth code has to come past here.
        // See MOLE-373.
        case VfsError::None:
        case VfsError::AccessDenied:
        case VfsError::NotSupported:
        case VfsError::NotADirectory:
        case VfsError::IsADirectory:
        case VfsError::NotALink:
        case VfsError::AlreadyExists:
        case VfsError::NotEmpty:
        case VfsError::IoError:
        case VfsError::NetworkError:
        case VfsError::Unknown:
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
    const QList<QByteArray>& commands, const VfsUri& context, const QString& what, const CancelToken& cancel)
{
    // A control channel is line-based, so a name carrying CR or LF is a second
    // command. `DELE a\r\nRMD /` is one file name to a filesystem -- ADR-0070 says
    // a backend takes the names it is given rather than the ones it likes -- and
    // two commands to a server. Refused here rather than escaped, because there
    // is no escape: the protocol has no way to say "this byte is part of the
    // argument". See MOLE-349.
    for (const QByteArray& command : commands) {
        if (!commandIsSendable(command)) {
            return Result<void>::failure(VfsError::IoError,
                QStringLiteral("%1: a name with a line break in it cannot be sent to an FTP server")
                    .arg(what));
        }
    }

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

    const net::Response response = m_pool->perform(lease, cancel);
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
    return runCommands({ "MKD " + remotePath(target).toUtf8() }, target,
        QStringLiteral("Creating %1").arg(target.path()), CancelToken());
}

Result<void> FtpFileSystem::remove(const VfsUri& target, bool recursive, const CancelToken& cancel)
{
    if (cancel.isCancelled())
        return Result<void>::failure(VfsError::Cancelled, QStringLiteral("Cancelled"));

    // The one stat, at the top. See removeEntry().
    const Result<FileEntry> what = stat(target);
    if (!what.ok())
        return Result<void>(what.error());
    return removeEntry(what.value(), recursive, cancel);
}

Result<void> FtpFileSystem::removeEntry(const FileEntry& entry, bool recursive, const CancelToken& cancel)
{
    if (cancel.isCancelled())
        return Result<void>::failure(VfsError::Cancelled, QStringLiteral("Cancelled"));

    if (!entry.isDir) {
        return runCommands({ "DELE " + remotePath(entry.uri).toUtf8() }, entry.uri,
            QStringLiteral("Deleting %1").arg(entry.uri.path()), cancel);
    }

    const Result<FileEntryList> children = listRaw(entry.uri, cancel);
    if (!children.ok())
        return Result<void>(children.error());

    if (!children.value().isEmpty()) {
        if (!recursive) {
            return Result<void>::failure(
                VfsError::NotEmpty, QStringLiteral("%1 is not empty").arg(entry.uri.path()));
        }
        for (const FileEntry& child : children.value()) {
            const Result<void> removed = removeEntry(child, true, cancel);
            if (!removed.ok())
                return removed;
        }
    }

    return runCommands({ "RMD " + remotePath(entry.uri).toUtf8() }, entry.uri,
        QStringLiteral("Deleting %1").arg(entry.uri.path()), cancel);
}

Result<void> FtpFileSystem::rename(const VfsUri& from, const VfsUri& to, const CancelToken& cancel)
{
    if (cancel.isCancelled())
        return Result<void>::failure(VfsError::Cancelled, QStringLiteral("Cancelled"));
    if (stat(to).ok()) {
        return Result<void>::failure(
            VfsError::AlreadyExists, QStringLiteral("%1 already exists").arg(to.path()));
    }
    // RNFR then RNTO, which is the only rename FTP has and has to arrive as one
    // pair on one connection.
    return runCommands({ "RNFR " + remotePath(from).toUtf8(), "RNTO " + remotePath(to).toUtf8() }, from,
        QStringLiteral("Renaming %1").arg(from.path()), cancel);
}

VfsError FtpFileSystem::fetchSpan(const QByteArray& url, const QString& what, QIODevice& sink, qint64 offset,
    qint64 span, const CancelToken& cancel)
{
    auto lease = m_pool->take();
    if (!lease)
        return VfsError::make(VfsError::IoError, QStringLiteral("Could not start an FTP transfer"));

    lease.setUrl(url);
    // Every lease, not only the first: the encryption, the passive mode and the
    // rest are what this drive was configured with, and a span that skipped them
    // would be a span that talked to the server differently from the one before.
    applySettings(lease);

    // Both ends of the range, which was the doubt this ticket existed to settle.
    // If a server honoured only the REST offset and ignored the end, one span
    // would keep delivering to the end of the file and the next would re-fetch
    // bytes already handed over -- a read that silently duplicates a span, which
    // is worse than one that needs scratch space. Measured against a real server
    // instead of read out of the documentation: it delivers exactly what it is
    // asked for, and rangedFetchDeliversExactlyTheSpanItAsksFor holds that.
    const QByteArray range = QByteArray::number(offset) + '-' + QByteArray::number(offset + span - 1);
    curl_easy_setopt(lease.get(), CURLOPT_RANGE, range.constData());

    const net::Response response = m_pool->perform(lease, cancel, &sink);
    return net::errorFor(response, what, net::StatusMeaning::ProtocolReply);
}

Result<std::unique_ptr<QIODevice>> FtpFileSystem::openRead(
    const VfsUri& target, qint64 expectedSize, const CancelToken& cancel)
{
    if (cancel.isCancelled()) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::Cancelled, QStringLiteral("Cancelled"));
    }
    const QByteArray url = urlFor(target, false);
    const QString what = QStringLiteral("Reading %1").arg(target.path());

    // Nobody said how big it is, so ask. A stream has to know where the file
    // ends before it starts. On FTP that costs a listing of the parent, which is
    // why a caller that already knows the size passes it.
    qint64 length = expectedSize;
    if (length < 0) {
        const Result<FileEntry> entry = stat(target);
        if (!entry.ok())
            return Result<std::unique_ptr<QIODevice>>(entry.error());
        length = entry.value().size;
    }

    // Small enough to hold: fetched whole, over a warm connection, as before.
    if (length <= kFetchWholeBelow) {
        auto scratch = std::make_unique<QTemporaryFile>();
        QString staging;
        if (!staging::openFile(*scratch, &staging)) {
            return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError,
                QStringLiteral("Could not open a local copy for %1: %2").arg(target.path(), staging));
        }

        auto lease = m_pool->take();
        if (!lease) {
            return Result<std::unique_ptr<QIODevice>>::failure(
                VfsError::IoError, QStringLiteral("Could not start an FTP transfer"));
        }

        lease.setUrl(url);
        applySettings(lease);

        const net::Response response = m_pool->perform(lease, cancel, scratch.get());
        const VfsError error = net::errorFor(response, what, net::StatusMeaning::ProtocolReply);
        if (error.isError())
            return Result<std::unique_ptr<QIODevice>>(error);

        return net::openDownloadedFile(std::move(scratch));
    }

    // Anything larger is streamed a span at a time, so a file bigger than the
    // scratch space is a file this can read. This is the mirror of MOLE-34 on
    // the write side, and the amendment to ADR-0014 now covers both directions.
    auto fetch = [this, url, what](QIODevice& sink, qint64 offset, qint64 span, const CancelToken& cancel) {
        return fetchSpan(url, what, sink, offset, span, cancel);
    };

    // The same validator SFTP uses, for the same reason: FTP has nothing like an
    // ETag either, so which file this is has to be asked as a fresh stat before
    // every later span. See MOLE-348 and the note in SftpFileSystem::openRead.
    QString openedAs;
    if (const Result<FileEntry> opened = stat(target); opened.ok())
        openedAs = net::identityOf(opened.value());

    auto stream = std::make_unique<net::StreamingDownload>(std::move(fetch), length, kDownloadSpanBytes);
    stream->keepAlive(sharedSelf());
    if (!openedAs.isEmpty()) {
        stream->checkBeforeEverySpan([this, target, openedAs]() -> VfsError {
            const Result<FileEntry> now = stat(target);
            if (!now.ok())
                return now.error();
            return net::identityOf(now.value()) == openedAs ? VfsError::ok()
                                                            : net::fileChangedWhileBeingRead();
        });
    }
    if (!stream->open(QIODevice::ReadOnly)) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, stream->errorString());
    }
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(stream.release()));
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

Result<std::unique_ptr<QIODevice>> FtpFileSystem::openWrite(
    const VfsUri& target, qint64, const CancelToken& cancel)
{
    if (cancel.isCancelled()) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::Cancelled, QStringLiteral("Cancelled"));
    }
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
    // A folder standing at the destination is refused before a byte goes over
    // the wire: it is not an old version of the file and there is nothing to
    // weigh up. The same call answers whether this is an overwrite, which only
    // an answer from before the write began can tell from a file that turned up
    // while this one was in flight. See MOLE-336.
    bool replacing = false;
    if (VfsError folder = refuseWritingOntoAFolder(*this, target, &replacing); folder.isError())
        return folder;

    auto send = [this, staging](QIODevice& source, qint64, bool append, const CancelToken& cancel) {
        return sendSpan(staging, source, append, cancel);
    };
    auto commit = [this, staging, target, replacing] {
        return commitPartialWrite(*this, staging, target, replacing);
    };

    auto stream
        = std::make_unique<net::StreamingUpload>(std::move(send), kUploadSpanBytes, std::move(commit));
    stream->keepAlive(sharedSelf());
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

    ConnectionField verify;
    verify.key = QStringLiteral("verifyTls");
    verify.label = QStringLiteral("Verify the TLS certificate");
    verify.kind = ConnectionField::Boolean;
    verify.defaultValue = true;
    verify.required = false;
    verify.advanced = true;
    // Read by settingsFrom() and documented on the settings struct since the
    // backend was written, with no field to set it from -- so a self-signed FTPS
    // server could not be reached at all, and the one setting that decides
    // whether "Require TLS" means anything was unreachable. WebDAV and S3 both
    // offer it. See MOLE-349.
    verify.help = QStringLiteral("Turn off only for a server with a self-signed certificate you trust");
    fields.append(verify);

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
