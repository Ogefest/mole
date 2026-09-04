#include "plugins/network/SmbFileSystem.h"

#include "core/vfs/PartialWrite.h"

#include <QDateTime>
#include <QIODevice>
#include <QThread>

#include <cstring>
#include <libsmbclient.h>
#include <mutex>

namespace mole {
namespace {

    constexpr QChar kSeparator = QLatin1Char('/');

    /// libsmbclient reports failures through errno, like the C library it is
    /// modelled on. Turned into something the interface can act on, because
    /// "operation failed" is the one answer nobody can do anything with.
    VfsError errorFromErrno(int code, const QString& what)
    {
        switch (code) {
        case ENOENT:
            return VfsError::make(VfsError::NotFound, QStringLiteral("%1: there is nothing there").arg(what));
        case EACCES:
        case EPERM:
            return VfsError::make(VfsError::AccessDenied, QStringLiteral("%1: permission denied").arg(what));
        case EEXIST:
            return VfsError::make(
                VfsError::AlreadyExists, QStringLiteral("%1: there is already something there").arg(what));
        case ENOTEMPTY:
            return VfsError::make(
                VfsError::NotEmpty, QStringLiteral("%1: the directory is not empty").arg(what));
        case ENOSPC:
            return VfsError::make(VfsError::IoError, QStringLiteral("%1: the share is full").arg(what));
        case ENOTDIR:
            return VfsError::make(
                VfsError::NotADirectory, QStringLiteral("%1: that is not a directory").arg(what));
        case EISDIR:
            return VfsError::make(
                VfsError::IsADirectory, QStringLiteral("%1: that is a directory").arg(what));
        case ECONNREFUSED:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ETIMEDOUT:
            return VfsError::make(
                VfsError::NetworkError, QStringLiteral("%1: the server did not answer").arg(what));
        case EINVAL:
            // What libsmbclient answers for a name the protocol will not carry,
            // which is a different thing from a network that is down.
            return VfsError::make(
                VfsError::IoError, QStringLiteral("%1: the server refused that name").arg(what));
        default:
            break;
        }
        return VfsError::make(VfsError::IoError,
            QStringLiteral("%1: %2").arg(what, QString::fromLocal8Bit(std::strerror(code))));
    }

    /// A file on the share, as a QIODevice.
    ///
    /// Seekable, which is what lets SMB advertise RandomAccessRead: a preview
    /// reads one page from the middle of a file rather than the file.
    class SmbFile final : public QIODevice, public ICommitsOnClose
    {
    public:
        /// `owner` is the drive as a shared pointer, held for this device's own
        /// lifetime.
        ///
        /// The raw pointer beside it is what everything here dereferences, and
        /// the destructor dereferences it too -- closeHandle() and
        /// discardStaging() both build a Session from it. So a device outliving
        /// its drive was a use-after-free inside libsmbclient, and "whoever
        /// opened it is holding the drive" was a convention about callers rather
        /// than a property of the code. Empty `owner` means the drive is not
        /// owned by a shared_ptr at all -- a stack instance in a test -- where
        /// its own scope is the answer. See MOLE-364.
        SmbFile(SmbFileSystem& drive, FileSystemPtr owner, int handle, qint64 size, QString what)
            : m_owner(std::move(owner))
            , m_drive(&drive)
            , m_handle(handle)
            , m_size(size)
            , m_what(std::move(what))
        {
        }

        /// A write goes under a working name and is renamed into place when it is
        /// closed. Without it an abandoned write -- a cancelled copy, or a caller
        /// that gave up -- leaves a partial file under the name somebody asked
        /// for, indistinguishable from a file that is simply that size. Same rule
        /// as the local disk: ADR-0020 and ADR-0021.
        /// `replacing` is whether the destination was already there when the
        /// write began, which is the one thing the commit cannot work out for
        /// itself. See commitPartialWrite().
        void commitOnCloseTo(VfsUri staging, VfsUri target, bool replacing)
        {
            m_staging = std::move(staging);
            m_target = std::move(target);
            m_replacing = replacing;
            m_commits = true;
        }

        ~SmbFile() override
        {
            closeHandle();
            // Destroyed without being closed is an abandoned write, and what was
            // written is not a file anybody asked for.
            if (m_commits && !m_committed)
                discardStaging();
        }

        VfsError commitError() const override { return m_commitFailure; }

        void close() override
        {
            QIODevice::close();
            closeHandle();
            if (m_commits && !m_committed)
                commit();
        }

        bool isSequential() const override { return false; }
        qint64 size() const override { return m_size >= 0 ? m_size : QIODevice::size(); }

        bool seek(qint64 position) override
        {
            if (position < 0)
                return false;
            const SmbFileSystem::Session session(*m_drive);
            if (!session.ok())
                return false;
            const off_t landed = smbc_lseek(m_handle, position, SEEK_SET);
            if (landed < 0) {
                setErrorString(errorFromErrno(errno, m_what).message);
                return false;
            }
            return QIODevice::seek(position);
        }

        /// Meaningful once a read has answered -1.
        VfsError failure() const { return m_failure; }

    protected:
        qint64 readData(char* data, qint64 maxSize) override
        {
            if (maxSize <= 0)
                return 0;
            const SmbFileSystem::Session session(*m_drive);
            if (!session.ok())
                return -1;
            const ssize_t got = smbc_read(m_handle, data, static_cast<size_t>(maxSize));
            if (got < 0) {
                m_failure = errorFromErrno(errno, m_what);
                setErrorString(m_failure.message);
                return -1;
            }
            return got;
        }

        qint64 writeData(const char* data, qint64 size) override
        {
            if (size <= 0)
                return 0;
            const SmbFileSystem::Session session(*m_drive);
            if (!session.ok())
                return -1;
            const ssize_t put = smbc_write(m_handle, data, static_cast<size_t>(size));
            if (put < 0) {
                m_failure = errorFromErrno(errno, m_what);
                setErrorString(m_failure.message);
                return -1;
            }
            return put;
        }

    private:
        void closeHandle()
        {
            if (m_handle < 0)
                return;
            const SmbFileSystem::Session session(*m_drive);
            if (session.ok())
                smbc_close(m_handle);
            m_handle = -1;
        }

        /// Puts the finished bytes under the name that was asked for.
        ///
        /// Through the shared helper, which is the whole point: this used to
        /// unlink the destination and rename onto it, whatever was there. A
        /// destination that appeared *while the write was in flight* is data
        /// nobody asked this write to touch, and it was being destroyed -- on a
        /// share, where two people writing the same name is the ordinary case
        /// rather than the exotic one. The removal an overwrite still needs is
        /// the default replace(), which is exactly the unlink-then-rename that
        /// was here: a share refuses to rename onto a name that exists, where a
        /// POSIX rename would replace it. See ADR-0020, ADR-0087 and MOLE-346.
        ///
        /// No Session held here. Every call the helper makes takes its own, and
        /// the session lock is not recursive -- holding one across them is the
        /// trap remove() documents.
        void commit()
        {
            m_committed = true;
            m_commitFailure = commitPartialWrite(*m_drive, m_staging, m_target, m_replacing);
        }

        void discardStaging()
        {
            m_committed = true;
            const SmbFileSystem::Session session(*m_drive);
            if (!session.ok())
                return;
            smbc_unlink(m_drive->urlFor(m_staging).toUtf8().constData());
        }

        /// Held, never dereferenced: this is the one that keeps the drive alive.
        FileSystemPtr m_owner;
        SmbFileSystem* m_drive = nullptr;
        int m_handle = -1;
        qint64 m_size = -1;
        QString m_what;
        VfsError m_failure;
        bool m_commits = false;
        bool m_committed = false;
        bool m_replacing = false;
        VfsUri m_staging;
        VfsUri m_target;
        VfsError m_commitFailure;
    };

} // namespace

QString SmbSettings::origin() const
{
    QString bare = host.trimmed();
    while (bare.endsWith(kSeparator))
        bare.chop(1);
    return QStringLiteral("smb://") + bare + kSeparator + share;
}

/// What the authentication callback is handed back, because libsmbclient gives
/// a callback nothing but the pointer it was told to keep.
struct SmbFileSystem::Credentials
{
    QByteArray username;
    QByteArray password;
    QByteArray domain;
};

namespace {

    void supplyCredentials(SMBCCTX* context, const char*, const char*, char* workgroup, int workgroupLength,
        char* username, int usernameLength, char* password, int passwordLength)
    {
        const auto* credentials
            = static_cast<const SmbFileSystem::Credentials*>(smbc_getOptionUserData(context));
        if (!credentials)
            return;
        // Truncated rather than overrun: libsmbclient hands over fixed buffers
        // and takes no responsibility for what is put in them.
        const auto copyInto = [](char* into, int room, const QByteArray& from) {
            if (room <= 0)
                return;
            const int fits = qMin(room - 1, static_cast<int>(from.size()));
            std::memcpy(into, from.constData(), static_cast<size_t>(fits));
            into[fits] = '\0';
        };
        copyInto(workgroup, workgroupLength, credentials->domain);
        copyInto(username, usernameLength, credentials->username);
        copyInto(password, passwordLength, credentials->password);
    }

} // namespace

SmbFileSystem::SmbFileSystem(QString scheme, SmbSettings settings)
    : m_scheme(std::move(scheme))
    , m_settings(std::move(settings))
    , m_credentials(std::make_unique<Credentials>())
{
    m_credentials->username = m_settings.username.toUtf8();
    m_credentials->password = m_settings.password.toUtf8();
    m_credentials->domain = m_settings.domain.toUtf8();
}

SmbFileSystem::~SmbFileSystem() = default;

namespace {

    /// Serialises every SMB operation in the process. See the note on the class
    /// for why there is only one session to serialise behind.
    std::mutex& sessionGuard()
    {
        static std::mutex only;
        return only;
    }

    /// The one context every `smbc_*` wrapper acts on, built on first use and
    /// never freed: `smbc_free_context()` tears down state libsmbclient keeps
    /// globally, and freeing it while the process still means to do SMB aborts
    /// inside Samba's allocator.
    SMBCCTX* theSession()
    {
        static SMBCCTX* held = nullptr;
        if (held)
            return held;

        SMBCCTX* fresh = smbc_new_context();
        if (!fresh)
            return nullptr;

        smbc_setFunctionAuthDataWithContext(fresh, supplyCredentials);
        smbc_setOptionUseKerberos(fresh, 0);
        smbc_setOptionFallbackAfterKerberos(fresh, 1);

        // The dialect is left to Samba's own configuration. smbc_setOptionProtocols()
        // takes `char*` and the library takes an interest in the memory afterwards;
        // handing it a string literal corrupted the heap a few operations later.
        // Samba has not offered SMB1 by default for years.
        if (!smbc_init_context(fresh)) {
            smbc_free_context(fresh, 1);
            return nullptr;
        }
        // The wrappers act on whatever this points at, which is the whole reason
        // there is one session rather than one per thread.
        smbc_set_context(fresh);
        held = fresh;
        return held;
    }

} // namespace

SmbFileSystem::Session::Session(const SmbFileSystem& drive)
{
    sessionGuard().lock();
    SMBCCTX* context = theSession();
    if (!context) {
        sessionGuard().unlock();
        return;
    }
    // Whose credentials the callback will read. Set on the way in to every
    // operation, because one session serves every drive in turn.
    smbc_setOptionUserData(context, const_cast<Credentials*>(drive.m_credentials.get()));
    m_ok = true;
}

SmbFileSystem::Session::~Session()
{
    if (m_ok)
        sessionGuard().unlock();
}

VfsCapabilities SmbFileSystem::capabilities() const
{
    // No ReportsSpace yet: libsmbclient offers statvfs and not every server
    // answers it honestly, and a capacity that is wrong is read as fact.
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead;
}

QString SmbFileSystem::urlFor(const VfsUri& uri) const
{
    QString root = m_settings.remoteRoot;
    while (root.endsWith(kSeparator))
        root.chop(1);
    while (root.startsWith(kSeparator))
        root.remove(0, 1);

    QString path = uri.path();
    while (path.startsWith(kSeparator))
        path.remove(0, 1);

    QString joined = m_settings.origin();
    if (!root.isEmpty())
        joined += kSeparator + root;
    if (!path.isEmpty())
        joined += kSeparator + path;
    return joined;
}

Result<FileEntryList> SmbFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    const Session session(*this);
    if (!session.ok()) {
        return Result<FileEntryList>::failure(
            VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
    }

    const QString what = QStringLiteral("Listing %1").arg(dir.path());
    const QByteArray url = urlFor(dir).toUtf8();
    const int handle = smbc_opendir(url.constData());
    if (handle < 0)
        return Result<FileEntryList>(errorFromErrno(errno, what));

    // Read the whole directory first, and only then ask about each entry.
    //
    // Nothing may touch this context while the directory handle is open.
    // `readdir` hands back a pointer into a buffer belonging to that handle, and
    // any other operation on the same context is free to reuse it -- so a `stat`
    // in this loop leaves the iteration walking memory that has been handed to
    // somebody else. It shows up as an abort inside Samba's own allocator, a
    // long way from here and during whatever happened to run next.
    //
    // The cost is holding the names of one directory, which is what a listing is
    // about to return anyway.
    struct Found
    {
        QString name;
        bool isDir = false;
    };
    QList<Found> found;

    while (struct smbc_dirent* entry = smbc_readdir(handle)) {
        if (cancel.isCancelled()) {
            smbc_closedir(handle);
            return Result<FileEntryList>::failure(VfsError::Cancelled, QStringLiteral("cancelled"));
        }

        // From namelen, and never from the pointer alone. `smbc_dirent` ends in a
        // flexible array declared `char name[1]`, so the name is as long as
        // namelen says and the array's *type* claims one byte -- which is enough
        // for QString::fromUtf8 to bind to its QByteArrayView overload, take the
        // array's extent, and hand back a one-character name. Every file in every
        // listing came back as its own first letter.
        const QString name = QString::fromUtf8(entry->name, qstrnlen(entry->name, entry->namelen));
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        // Printers, IPC endpoints and the rest of what a server offers. They are
        // not files and nothing in a file manager can do anything with them.
        if (entry->smbc_type != SMBC_FILE && entry->smbc_type != SMBC_DIR)
            continue;
        found.append(Found { name, entry->smbc_type == SMBC_DIR });
    }
    smbc_closedir(handle);

    // Size and time need a stat of their own: a directory entry carries the name
    // and the kind and nothing else. Asked for rather than left out, because a
    // listing without sizes or dates is half a listing.
    FileEntryList entries;
    entries.reserve(found.size());
    for (const Found& one : std::as_const(found)) {
        if (cancel.isCancelled())
            return Result<FileEntryList>::failure(VfsError::Cancelled, QStringLiteral("cancelled"));

        FileEntry made;
        made.name = one.name;
        made.uri = dir.child(one.name);
        made.isDir = one.isDir;

        struct stat details
        {
        };
        const QByteArray childUrl = urlFor(made.uri).toUtf8();
        if (smbc_stat(childUrl.constData(), &details) == 0) {
            made.size = made.isDir ? 0 : static_cast<qint64>(details.st_size);
            made.modified = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(details.st_mtime));
        }
        entries.append(made);
    }
    return Result<FileEntryList>(entries);
}

Result<FileEntry> SmbFileSystem::stat(const VfsUri& target)
{
    const Session session(*this);
    if (!session.ok()) {
        return Result<FileEntry>::failure(
            VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
    }

    const QByteArray url = urlFor(target).toUtf8();
    struct stat details
    {
    };
    if (smbc_stat(url.constData(), &details) != 0)
        return Result<FileEntry>(errorFromErrno(errno, QStringLiteral("Looking at %1").arg(target.path())));

    FileEntry entry;
    entry.uri = target;
    entry.name = target.fileName();
    entry.isDir = S_ISDIR(details.st_mode);
    entry.size = entry.isDir ? 0 : static_cast<qint64>(details.st_size);
    entry.modified = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(details.st_mtime));
    return Result<FileEntry>(entry);
}

Result<void> SmbFileSystem::makeDirectory(const VfsUri& target)
{
    const Session session(*this);
    if (!session.ok())
        return VfsError::make(VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));

    const QByteArray url = urlFor(target).toUtf8();
    if (smbc_mkdir(url.constData(), 0755) != 0)
        return errorFromErrno(errno, QStringLiteral("Creating %1").arg(target.path()));
    return {};
}

Result<void> SmbFileSystem::remove(const VfsUri& target, bool recursive)
{
    // No session taken here. What follows calls stat(), list() and remove()
    // again, each of which takes one -- and the guard is not recursive, so
    // holding it across them would deadlock on the first child.
    const Result<FileEntry> what = stat(target);
    if (!what.ok())
        return Result<void>(what.error());

    if (!what.value().isDir) {
        const Session session(*this);
        if (!session.ok())
            return VfsError::make(
                VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
        const QByteArray url = urlFor(target).toUtf8();
        if (smbc_unlink(url.constData()) != 0)
            return errorFromErrno(errno, QStringLiteral("Removing %1").arg(target.path()));
        return {};
    }

    if (recursive) {
        // Depth first, because a directory cannot go until what is in it has.
        const Result<FileEntryList> inside = list(target, CancelToken());
        if (!inside.ok())
            return Result<void>(inside.error());
        for (const FileEntry& child : inside.value()) {
            const Result<void> gone = remove(child.uri, true);
            if (!gone.ok())
                return gone;
        }
    }

    const Session session(*this);
    if (!session.ok())
        return VfsError::make(VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
    const QByteArray url = urlFor(target).toUtf8();
    if (smbc_rmdir(url.constData()) != 0)
        return errorFromErrno(errno, QStringLiteral("Removing %1").arg(target.path()));
    return {};
}

Result<void> SmbFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    const Session session(*this);
    if (!session.ok())
        return VfsError::make(VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));

    const QByteArray fromUrl = urlFor(from).toUtf8();
    const QByteArray toUrl = urlFor(to).toUtf8();

    // Refused rather than allowed to replace, which is what every other backend
    // here does and what the conformance suite requires: a rename that silently
    // overwrites is how a bulk rename destroys a file nobody mentioned. Samba's
    // rename will overwrite if it is let, so the check is ours to make.
    // Somebody who means to replace calls replace(), whose default is the
    // unlink-then-rename a share needs.
    struct stat already
    {
    };
    if (smbc_stat(toUrl.constData(), &already) == 0) {
        // **Whether something is in the way is not the same question as whether
        // it is somebody else.** A share that does not distinguish case answers
        // this stat for `Beta-Renamed.log` with the very file being renamed, so
        // the guard refused a case-only rename and there was no way to perform one
        // at all. That is what the conformance suite's case-only rename exists to
        // catch, and it caught it here the first time the live tier ran against
        // Samba; `RenamePlan` reasons the same way one layer up -- the file in the
        // way is the file being renamed.
        //
        // Asked of pathCaseSensitivity() rather than of the two stats: Samba
        // answers a stat for either spelling and derives the inode it reports from
        // the name it was asked with, so device and inode say two files where
        // there is one. That was tried here first and did not work.
        const bool sameEntry = pathCaseSensitivity() == Qt::CaseInsensitive
            && from.path().compare(to.path(), Qt::CaseInsensitive) == 0;
        if (!sameEntry) {
            return VfsError::make(VfsError::AlreadyExists,
                QStringLiteral("Renaming %1: there is already something called %2")
                    .arg(from.path(), to.fileName()));
        }
    }

    if (smbc_rename(fromUrl.constData(), toUrl.constData()) != 0) {
        return errorFromErrno(errno, QStringLiteral("Renaming %1 to %2").arg(from.path(), to.fileName()));
    }
    return {};
}

Result<std::unique_ptr<QIODevice>> SmbFileSystem::openRead(const VfsUri& target, qint64 expectedSize)
{
    const Session session(*this);
    if (!session.ok()) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
    }

    const QString what = QStringLiteral("Reading %1").arg(target.path());
    const QByteArray url = urlFor(target).toUtf8();
    const int handle = smbc_open(url.constData(), O_RDONLY, 0);
    if (handle < 0)
        return Result<std::unique_ptr<QIODevice>>(errorFromErrno(errno, what));

    // Nothing is staged and nothing is held: the share is read through as it is
    // read from, so a file larger than the local disk is a file this can read.
    // That is what ADR-0014 asks of every backend.
    qint64 length = expectedSize;
    if (length < 0) {
        struct stat details
        {
        };
        if (smbc_fstat(handle, &details) == 0)
            length = static_cast<qint64>(details.st_size);
    }

    auto file = std::make_unique<SmbFile>(*this, sharedSelf(), handle, length, what);
    if (!file->open(QIODevice::ReadOnly)) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, file->errorString());
    }
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(file.release()));
}

Result<std::unique_ptr<QIODevice>> SmbFileSystem::openWrite(const VfsUri& target, qint64)
{
    // A folder standing at the destination is refused before a byte goes over
    // the wire: it is not an old version of the file and there is nothing to
    // weigh up. The same call answers whether this is an overwrite, which only
    // an answer from before the write began can tell from a file that turned up
    // while this one was in flight. See MOLE-336. Before the session below rather
    // than after it: the call takes a stat of its own, and the lock is not
    // recursive.
    bool replacing = false;
    if (VfsError folder = refuseWritingOntoAFolder(*this, target, &replacing); folder.isError())
        return folder;

    const Session session(*this);
    if (!session.ok()) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IoError, QStringLiteral("This machine cannot start an SMB session"));
    }

    const QString what = QStringLiteral("Writing %1").arg(target.path());
    const VfsUri staging = partialWriteOf(target);
    const QByteArray url = urlFor(staging).toUtf8();
    const int handle = smbc_open(url.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (handle < 0)
        return Result<std::unique_ptr<QIODevice>>(errorFromErrno(errno, what));

    auto file = std::make_unique<SmbFile>(*this, sharedSelf(), handle, -1, what);
    file->commitOnCloseTo(staging, target, replacing);
    if (!file->open(QIODevice::WriteOnly)) {
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, file->errorString());
    }
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(file.release()));
}

// ---- the factory -----------------------------------------------------------

QList<ConnectionField> SmbFileSystemFactory::connectionFields() const
{
    QList<ConnectionField> fields;

    ConnectionField host;
    host.key = QStringLiteral("host");
    host.label = QStringLiteral("Server");
    host.help = QStringLiteral("Name or address of the machine, for example fileserver or 10.0.0.4");
    fields.append(host);

    ConnectionField share;
    share.key = QStringLiteral("share");
    share.label = QStringLiteral("Share");
    share.help = QStringLiteral("The share name, without slashes");
    fields.append(share);

    ConnectionField user;
    user.key = QStringLiteral("user");
    user.label = QStringLiteral("User");
    user.required = false;
    fields.append(user);

    ConnectionField password;
    password.key = QStringLiteral("password");
    password.label = QStringLiteral("Password");
    password.kind = ConnectionField::Password;
    password.required = false;
    fields.append(password);

    ConnectionField domain;
    domain.key = QStringLiteral("domain");
    domain.label = QStringLiteral("Domain");
    domain.help = QStringLiteral("Only for an Active Directory domain; leave empty otherwise");
    domain.required = false;
    domain.advanced = true;
    fields.append(domain);

    ConnectionField root;
    root.key = QStringLiteral("root");
    root.label = QStringLiteral("Folder inside the share");
    root.required = false;
    root.advanced = true;
    fields.append(root);

    return fields;
}

SmbSettings SmbFileSystemFactory::settingsFrom(const QVariantMap& config)
{
    SmbSettings settings;
    settings.host = config.value(QStringLiteral("host")).toString().trimmed();
    settings.share = config.value(QStringLiteral("share")).toString().trimmed();
    settings.username = config.value(QStringLiteral("user")).toString();
    settings.password = config.value(QStringLiteral("password")).toString();
    settings.domain = config.value(QStringLiteral("domain")).toString();
    settings.remoteRoot = config.value(QStringLiteral("root")).toString();

    // A share pasted as \\server\photos or //server/photos is what people have
    // in front of them, so it is taken rather than refused.
    QString share = settings.share;
    share.replace(QLatin1Char('\\'), kSeparator);
    while (share.startsWith(kSeparator))
        share.remove(0, 1);
    if (settings.host.isEmpty() && share.contains(kSeparator)) {
        settings.host = share.section(kSeparator, 0, 0);
        share = share.section(kSeparator, 1);
    }
    while (share.endsWith(kSeparator))
        share.chop(1);
    settings.share = share;
    return settings;
}

FileSystemPtr SmbFileSystemFactory::create(const QVariantMap& config, QString* errorOut)
{
    const SmbSettings settings = settingsFrom(config);
    if (settings.host.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("An SMB drive needs a server");
        return nullptr;
    }
    if (settings.share.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("An SMB drive needs a share");
        return nullptr;
    }
    return std::make_shared<SmbFileSystem>(QStringLiteral("smb"), settings);
}

} // namespace mole
