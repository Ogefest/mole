#include "plugins/network/NfsFileSystem.h"

#include "core/vfs/PartialWrite.h"

#include <QDateTime>
#include <QHash>
#include <QIODevice>

#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <nfsc/libnfs.h>
#include <sys/stat.h>
#include <vector>

namespace mole {
namespace {

    constexpr QChar kSeparator = QLatin1Char('/');

    /// How long one remote procedure call may take before the connection is
    /// declared dead. libnfs takes milliseconds and rounds to whole seconds.
    ///
    /// The same twenty seconds the HTTP transport gives a socket, and for the
    /// same reason: it is long enough that a busy server is not mistaken for a
    /// dead one, and short enough that a dead one does not hold a copy open all
    /// afternoon. Without it libnfs waits for the kernel, which on a silently
    /// dropped connection means minutes.
    constexpr int kRpcPatienceMs = 20000;

    /// How many mounted connections to keep for a server nothing is using.
    /// Mounting is two round trips, so throwing every connection away would make
    /// a directory walk pay for one per directory; keeping all of them would hold
    /// a socket open for every file that was ever read at once.
    constexpr int kIdleMountsKept = 4;

    /// A single read is asked for a megabyte at a time. libnfs's read returns
    /// `int`, and a caller that asks for more than a protocol read can carry gets
    /// a short answer anyway -- so the limit is stated here rather than left to be
    /// discovered.
    constexpr qint64 kReadChunkBytes = 1 << 20;

    /// Whether this is the server answering a question about a file, rather than
    /// the connection underneath failing.
    ///
    /// The difference decides whether the context can be used again. libnfs keeps
    /// one TCP connection per context, and once that has gone every later call on
    /// it answers with the same failure -- so a broken one has to be closed
    /// rather than handed back to the pool, or the drive stays broken until Mole
    /// is restarted.
    bool isAnAnswerRatherThanABrokenConnection(int code)
    {
        switch (code) {
        case ENOENT:
        case EEXIST:
        case EACCES:
        case EPERM:
        case ENOTEMPTY:
        case ENOTDIR:
        case EISDIR:
        case EINVAL:
        case ENOSPC:
        case EDQUOT:
        case EROFS:
        case ENAMETOOLONG:
        case EXDEV:
            return true;
        default:
            return false;
        }
    }

    /// libnfs answers with a negative errno and keeps a sentence about what
    /// actually happened on the context. Both are worth having: the errno is what
    /// the interface can act on, the sentence is what a person can.
    VfsError errorFromNfs(int rc, nfs_context* context, const QString& what)
    {
        const int code = rc < 0 ? -rc : rc;
        const char* said = context ? nfs_get_error(context) : nullptr;
        const QString detail
            = said && *said ? QString::fromUtf8(said) : QString::fromLocal8Bit(std::strerror(code));

        switch (code) {
        case ENOENT:
            return VfsError::make(VfsError::NotFound, QStringLiteral("%1: there is nothing there").arg(what));
        case EACCES:
        case EPERM:
            return VfsError::make(VfsError::AccessDenied,
                QStringLiteral("%1: the export does not allow that (%2)").arg(what, detail));
        case EEXIST:
            return VfsError::make(
                VfsError::AlreadyExists, QStringLiteral("%1: there is already something there").arg(what));
        case ENOTEMPTY:
            return VfsError::make(
                VfsError::NotEmpty, QStringLiteral("%1: the directory is not empty").arg(what));
        case ENOTDIR:
            return VfsError::make(
                VfsError::NotADirectory, QStringLiteral("%1: that is not a directory").arg(what));
        case EISDIR:
            return VfsError::make(
                VfsError::IsADirectory, QStringLiteral("%1: that is a directory").arg(what));
        case ENOSPC:
        case EDQUOT:
            return VfsError::make(VfsError::IoError, QStringLiteral("%1: the export is full").arg(what));
        case ECONNREFUSED:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ETIMEDOUT:
        case ECONNRESET:
        case EPIPE:
            return VfsError::make(VfsError::NetworkError,
                QStringLiteral("%1: the server did not answer (%2)").arg(what, detail));
        default:
            break;
        }
        return VfsError::make(VfsError::IoError, QStringLiteral("%1: %2").arg(what, detail));
    }

    /// Mounted connections nothing is using, one bag per server-and-export.
    struct MountPool
    {
        std::mutex guard;
        std::vector<nfs_context*> idle;
    };

    std::mutex& poolsGuard()
    {
        static std::mutex only;
        return only;
    }

    /// Deliberately never destroyed, and a raw `new` rather than a plain static for
    /// that reason: the pools it holds outlive every lease, and a hash destroyed at
    /// static-destruction time would throw away the only handle to them while a task
    /// thread could still be holding one. Keeping the handle in the data segment is
    /// also what makes the intent legible to LeakSanitizer, whose check runs after
    /// static destructors -- a destroyed hash left the pools genuinely unreachable
    /// and reported as leaked, together with any idle context they still held.
    QHash<QString, MountPool*>& pools()
    {
        static auto* all = new QHash<QString, MountPool*>;
        return *all;
    }

    /// The pool for one mount. Pools themselves are never freed -- there is one
    /// per server-and-export a session ever touched, and a stable address is what
    /// lets a lease hand its context back without holding a lock the whole time.
    MountPool& poolFor(const QString& key)
    {
        const std::lock_guard<std::mutex> lock(poolsGuard());
        MountPool*& pool = pools()[key];
        if (!pool)
            pool = new MountPool;
        return *pool;
    }

    nfs_context* mountFresh(const NfsSettings& settings, QString* failure)
    {
        nfs_context* context = nfs_init_context();
        if (!context) {
            if (failure)
                *failure = QStringLiteral("there is no memory for another NFS connection");
            return nullptr;
        }

        nfs_set_timeout(context, kRpcPatienceMs);
        // One reconnect, not endless: a connection that dropped once is worth
        // rebuilding, and a server that keeps dropping it should be reported
        // rather than retried behind the caller's back for ever.
        nfs_set_autoreconnect(context, 1);
        // Left at the ids this process runs as unless somebody says otherwise --
        // see the note on NfsSettings for why this is not authentication.
        if (settings.uid >= 0)
            nfs_set_uid(context, settings.uid);
        if (settings.gid >= 0)
            nfs_set_gid(context, settings.gid);

        const QByteArray host = settings.host.toUtf8();
        const QByteArray exported = settings.exportPath.toUtf8();
        const int rc = nfs_mount(context, host.constData(), exported.constData());
        if (rc < 0) {
            if (failure) {
                *failure = errorFromNfs(rc, context, QStringLiteral("Mounting %1").arg(settings.exportPath))
                               .message;
            }
            nfs_destroy_context(context);
            return nullptr;
        }
        return context;
    }

} // namespace

QString NfsSettings::mountKey() const
{
    // Everything the mount depends on, and nothing that does not: two drives
    // rooted at different folders inside one export are the same connection.
    return QStringLiteral("%1\n%2\n%3\n%4").arg(host, exportPath).arg(uid).arg(gid);
}

NfsFileSystem::Mount::Mount(const NfsSettings& settings)
    : m_key(settings.mountKey())
{
    MountPool& pool = poolFor(m_key);
    {
        const std::lock_guard<std::mutex> lock(pool.guard);
        if (!pool.idle.empty()) {
            m_context = pool.idle.back();
            pool.idle.pop_back();
            return;
        }
    }
    m_context = mountFresh(settings, &m_failure);
}

NfsFileSystem::Mount::~Mount()
{
    giveBack();
}

NfsFileSystem::Mount::Mount(Mount&& other) noexcept
    : m_key(std::move(other.m_key))
    , m_context(other.m_context)
    , m_reusable(other.m_reusable)
    , m_failure(std::move(other.m_failure))
{
    other.m_context = nullptr;
}

NfsFileSystem::Mount& NfsFileSystem::Mount::operator=(Mount&& other) noexcept
{
    if (this == &other)
        return *this;
    giveBack();
    m_key = std::move(other.m_key);
    m_context = other.m_context;
    m_reusable = other.m_reusable;
    m_failure = std::move(other.m_failure);
    other.m_context = nullptr;
    return *this;
}

void NfsFileSystem::Mount::giveBack()
{
    if (!m_context)
        return;
    nfs_context* context = m_context;
    m_context = nullptr;

    if (m_reusable) {
        MountPool& pool = poolFor(m_key);
        const std::lock_guard<std::mutex> lock(pool.guard);
        if (pool.idle.size() < size_t(kIdleMountsKept)) {
            pool.idle.push_back(context);
            return;
        }
    }
    nfs_destroy_context(context);
}

void NfsFileSystem::forgetPooledMounts()
{
    QList<MountPool*> known;
    {
        const std::lock_guard<std::mutex> lock(poolsGuard());
        known = pools().values();
    }
    for (MountPool* pool : known) {
        const std::lock_guard<std::mutex> lock(pool->guard);
        for (nfs_context* context : pool->idle)
            nfs_destroy_context(context);
        pool->idle.clear();
    }
}

namespace {

    /// A file on the export, as a QIODevice.
    ///
    /// It holds its connection for as long as it is open, which is the point: the
    /// handle inside is only meaningful to the context it was opened on, and the
    /// thread that reads a file is not always the one that opened it.
    class NfsFile final : public QIODevice, public ICommitsOnClose
    {
    public:
        NfsFile(NfsFileSystem::Mount mount, struct nfsfh* handle, qint64 size, QString what)
            : m_mount(std::move(mount))
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
        void commitOnCloseTo(QString staging, QString target)
        {
            m_staging = std::move(staging);
            m_target = std::move(target);
            m_commits = true;
        }

        ~NfsFile() override
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
            if (position < 0 || !m_handle)
                return false;
            uint64_t landed = 0;
            const int rc = nfs_lseek(m_mount.context(), m_handle, position, SEEK_SET, &landed);
            if (rc < 0) {
                note(rc);
                return false;
            }
            return QIODevice::seek(position);
        }

        /// Meaningful once a read has answered -1.
        VfsError failure() const { return m_failure; }

    protected:
        qint64 readData(char* data, qint64 maxSize) override
        {
            if (maxSize <= 0 || !m_handle)
                return maxSize <= 0 ? 0 : -1;
            const uint64_t want = uint64_t(qMin(maxSize, kReadChunkBytes));
            const int got = nfs_read(m_mount.context(), m_handle, want, data);
            if (got < 0) {
                note(got);
                return -1;
            }
            return got;
        }

        qint64 writeData(const char* data, qint64 size) override
        {
            if (size <= 0 || !m_handle)
                return size <= 0 ? 0 : -1;
            const uint64_t want = uint64_t(qMin(size, kReadChunkBytes));
            const int put = nfs_write(m_mount.context(), m_handle, want, data);
            if (put < 0) {
                note(put);
                return -1;
            }
            return put;
        }

    private:
        /// Records a failure and, when it was the connection rather than the
        /// file, makes sure this one is not handed to the next caller.
        void note(int rc)
        {
            m_failure = errorFromNfs(rc, m_mount.context(), m_what);
            setErrorString(m_failure.message);
            if (!isAnAnswerRatherThanABrokenConnection(rc < 0 ? -rc : rc))
                m_mount.abandon();
        }

        void closeHandle()
        {
            if (!m_handle)
                return;
            nfs_close(m_mount.context(), m_handle);
            m_handle = nullptr;
        }

        /// Puts the finished bytes under the name that was asked for.
        void commit()
        {
            m_committed = true;
            if (!m_mount.ok()) {
                m_commitFailure = VfsError::make(
                    VfsError::IoError, QStringLiteral("%1: the connection was lost").arg(m_what));
                return;
            }
            // Nothing is removed first. An NFS rename replaces what is there, the
            // way a POSIX one does, so unlike a share (ADR-0048) an overwrite
            // needs no window in which the destination does not exist.
            const QByteArray from = m_staging.toUtf8();
            const QByteArray to = m_target.toUtf8();
            const int rc = nfs_rename(m_mount.context(), from.constData(), to.constData());
            if (rc < 0) {
                m_commitFailure = errorFromNfs(rc, m_mount.context(), m_what);
                nfs_unlink(m_mount.context(), from.constData());
            }
        }

        void discardStaging()
        {
            m_committed = true;
            if (m_mount.ok())
                nfs_unlink(m_mount.context(), m_staging.toUtf8().constData());
        }

        NfsFileSystem::Mount m_mount;
        struct nfsfh* m_handle = nullptr;
        qint64 m_size = -1;
        QString m_what;
        VfsError m_failure;
        bool m_commits = false;
        bool m_committed = false;
        QString m_staging;
        QString m_target;
        VfsError m_commitFailure;
    };

} // namespace

NfsFileSystem::NfsFileSystem(QString scheme, NfsSettings settings)
    : m_scheme(std::move(scheme))
    , m_settings(std::move(settings))
{
}

NfsFileSystem::~NfsFileSystem() = default;

VfsCapabilities NfsFileSystem::capabilities() const
{
    // No ReportsSpace: libnfs will answer statvfs, and an export's free space is
    // the server's whole filesystem rather than what this drive may use, which is
    // a number read as fact and wrong more often than not.
    return VfsCapability::Read | VfsCapability::Write | VfsCapability::Create | VfsCapability::Delete
        | VfsCapability::Rename | VfsCapability::MakeDirectory | VfsCapability::RandomAccessRead;
}

QString NfsFileSystem::pathFor(const VfsUri& uri) const
{
    QString root = m_settings.remoteRoot;
    while (root.endsWith(kSeparator))
        root.chop(1);
    while (root.startsWith(kSeparator))
        root.remove(0, 1);

    QString path = uri.path();
    while (path.startsWith(kSeparator))
        path.remove(0, 1);

    // Absolute and inside the export, which is the only shape libnfs takes: the
    // export is the root, and there is no way to name anything above it.
    QString joined;
    if (!root.isEmpty())
        joined += kSeparator + root;
    if (!path.isEmpty())
        joined += kSeparator + path;
    return joined.isEmpty() ? QString(kSeparator) : joined;
}

Result<FileEntryList> NfsFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    Mount mount(m_settings);
    if (!mount.ok())
        return Result<FileEntryList>::failure(VfsError::NetworkError, mount.failure());

    const QString what = QStringLiteral("Listing %1").arg(dir.path());
    const QByteArray path = pathFor(dir).toUtf8();
    struct nfsdir* handle = nullptr;
    const int opened = nfs_opendir(mount.context(), path.constData(), &handle);
    if (opened < 0) {
        if (!isAnAnswerRatherThanABrokenConnection(-opened))
            mount.abandon();
        return Result<FileEntryList>(errorFromNfs(opened, mount.context(), what));
    }

    // What a directory entry did not answer for itself. READDIRPLUS brings the
    // attributes back with the names, so an ordinary listing is one round trip
    // and this stays empty -- but a server that only offers plain READDIR
    // answers with names alone, and a listing without sizes or dates is half a
    // listing. Those are looked up afterwards rather than inside the loop.
    QList<int> incomplete;
    FileEntryList entries;

    while (struct nfsdirent* entry = nfs_readdir(mount.context(), handle)) {
        if (cancel.isCancelled()) {
            nfs_closedir(mount.context(), handle);
            return Result<FileEntryList>::failure(VfsError::Cancelled, QStringLiteral("cancelled"));
        }

        const QString name = QString::fromUtf8(entry->name);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;

        FileEntry made;
        made.name = name;
        made.uri = dir.child(name);

        const mode_t mode = mode_t(entry->mode);
        if (mode == 0 || S_ISLNK(mode)) {
            // A link is followed, so a link to a directory reads as a directory.
            entries.append(made);
            incomplete.append(int(entries.size()) - 1);
            continue;
        }
        // Sockets, pipes and devices are not files and nothing in a file manager
        // can do anything with them.
        if (!S_ISREG(mode) && !S_ISDIR(mode))
            continue;

        made.isDir = S_ISDIR(mode);
        made.size = made.isDir ? 0 : qint64(entry->size);
        made.modified = QDateTime::fromSecsSinceEpoch(qint64(entry->mtime.tv_sec));
        entries.append(made);
    }
    nfs_closedir(mount.context(), handle);

    for (const int index : std::as_const(incomplete)) {
        if (cancel.isCancelled())
            return Result<FileEntryList>::failure(VfsError::Cancelled, QStringLiteral("cancelled"));

        FileEntry& made = entries[index];
        struct nfs_stat_64 details
        {
        };
        const QByteArray childPath = pathFor(made.uri).toUtf8();
        if (nfs_stat64(mount.context(), childPath.constData(), &details) == 0) {
            made.isDir = S_ISDIR(mode_t(details.nfs_mode));
            made.size = made.isDir ? 0 : qint64(details.nfs_size);
            made.modified = QDateTime::fromSecsSinceEpoch(qint64(details.nfs_mtime));
        }
    }
    return Result<FileEntryList>(entries);
}

Result<FileEntry> NfsFileSystem::stat(const VfsUri& target)
{
    Mount mount(m_settings);
    if (!mount.ok())
        return Result<FileEntry>::failure(VfsError::NetworkError, mount.failure());

    const QByteArray path = pathFor(target).toUtf8();
    struct nfs_stat_64 details
    {
    };
    const int rc = nfs_stat64(mount.context(), path.constData(), &details);
    if (rc < 0) {
        if (!isAnAnswerRatherThanABrokenConnection(-rc))
            mount.abandon();
        return Result<FileEntry>(
            errorFromNfs(rc, mount.context(), QStringLiteral("Looking at %1").arg(target.path())));
    }

    FileEntry entry;
    entry.uri = target;
    entry.name = target.fileName();
    entry.isDir = S_ISDIR(mode_t(details.nfs_mode));
    entry.size = entry.isDir ? 0 : qint64(details.nfs_size);
    entry.modified = QDateTime::fromSecsSinceEpoch(qint64(details.nfs_mtime));
    return Result<FileEntry>(entry);
}

Result<void> NfsFileSystem::makeDirectory(const VfsUri& target)
{
    Mount mount(m_settings);
    if (!mount.ok())
        return VfsError::make(VfsError::NetworkError, mount.failure());

    const QByteArray path = pathFor(target).toUtf8();
    const int rc = nfs_mkdir(mount.context(), path.constData());
    if (rc < 0) {
        if (!isAnAnswerRatherThanABrokenConnection(-rc))
            mount.abandon();
        return errorFromNfs(rc, mount.context(), QStringLiteral("Creating %1").arg(target.path()));
    }
    return {};
}

Result<void> NfsFileSystem::remove(const VfsUri& target, bool recursive)
{
    const Result<FileEntry> what = stat(target);
    if (!what.ok())
        return Result<void>(what.error());

    if (what.value().isDir && recursive) {
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

    Mount mount(m_settings);
    if (!mount.ok())
        return VfsError::make(VfsError::NetworkError, mount.failure());

    const QByteArray path = pathFor(target).toUtf8();
    const int rc = what.value().isDir ? nfs_rmdir(mount.context(), path.constData())
                                      : nfs_unlink(mount.context(), path.constData());
    if (rc < 0) {
        if (!isAnAnswerRatherThanABrokenConnection(-rc))
            mount.abandon();
        return errorFromNfs(rc, mount.context(), QStringLiteral("Removing %1").arg(target.path()));
    }
    return {};
}

Result<void> NfsFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    Mount mount(m_settings);
    if (!mount.ok())
        return VfsError::make(VfsError::NetworkError, mount.failure());

    const QByteArray fromPath = pathFor(from).toUtf8();
    const QByteArray toPath = pathFor(to).toUtf8();

    // Refused rather than allowed to replace, which is what every other backend
    // here does and what the conformance suite requires: a rename that silently
    // overwrites is how a bulk rename destroys a file nobody mentioned. NFS,
    // being POSIX, will replace if it is let -- so the check is ours to make. The
    // commit of a finished write does not come through here, and says why.
    struct nfs_stat_64 already
    {
    };
    if (nfs_stat64(mount.context(), toPath.constData(), &already) == 0) {
        return VfsError::make(VfsError::AlreadyExists,
            QStringLiteral("Renaming %1: there is already something called %2")
                .arg(from.path(), to.fileName()));
    }

    const int rc = nfs_rename(mount.context(), fromPath.constData(), toPath.constData());
    if (rc < 0) {
        if (!isAnAnswerRatherThanABrokenConnection(-rc))
            mount.abandon();
        return errorFromNfs(
            rc, mount.context(), QStringLiteral("Renaming %1 to %2").arg(from.path(), to.fileName()));
    }
    return {};
}

Result<std::unique_ptr<QIODevice>> NfsFileSystem::openRead(const VfsUri& target, qint64 expectedSize)
{
    Mount mount(m_settings);
    if (!mount.ok())
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::NetworkError, mount.failure());

    const QString what = QStringLiteral("Reading %1").arg(target.path());
    const QByteArray path = pathFor(target).toUtf8();
    struct nfsfh* handle = nullptr;
    const int rc = nfs_open(mount.context(), path.constData(), O_RDONLY, &handle);
    if (rc < 0) {
        if (!isAnAnswerRatherThanABrokenConnection(-rc))
            mount.abandon();
        return Result<std::unique_ptr<QIODevice>>(errorFromNfs(rc, mount.context(), what));
    }

    // Always asked, even when the caller said how big the file is, because it is
    // also the only thing that will refuse a directory. NFS has no open: libnfs
    // looks the name up and hands back a handle, and a directory answers that
    // lookup as readily as a file does -- so an unchecked read of a directory
    // succeeds here and then answers every read with EISDIR, which arrives as a
    // failed copy rather than as a refused one.
    struct nfs_stat_64 details
    {
    };
    const int described = nfs_fstat64(mount.context(), handle, &details);
    if (described == 0 && S_ISDIR(mode_t(details.nfs_mode))) {
        nfs_close(mount.context(), handle);
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IsADirectory, QStringLiteral("%1: that is a directory").arg(what));
    }

    // Nothing is staged and nothing is held: the export is read through as it is
    // read from, so a file larger than the local disk is a file this can read.
    // That is what ADR-0014 asks of every backend.
    qint64 length = expectedSize;
    if (length < 0 && described == 0)
        length = qint64(details.nfs_size);

    auto file = std::make_unique<NfsFile>(std::move(mount), handle, length, what);
    if (!file->open(QIODevice::ReadOnly))
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, file->errorString());
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(file.release()));
}

Result<std::unique_ptr<QIODevice>> NfsFileSystem::openWrite(const VfsUri& target, qint64)
{
    Mount mount(m_settings);
    if (!mount.ok())
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::NetworkError, mount.failure());

    const QString what = QStringLiteral("Writing %1").arg(target.path());
    const VfsUri staging = partialWriteOf(target);
    const QString stagingPath = pathFor(staging);
    const QByteArray path = stagingPath.toUtf8();

    struct nfsfh* handle = nullptr;
    int rc = nfs_open2(mount.context(), path.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644, &handle);
    if (rc < 0) {
        // Older libnfs will not create through open, and answers as if the file
        // were simply not there. Creating it is the same request said the other
        // way round, so it is worth one more call before giving up.
        if (-rc == ENOENT)
            rc = nfs_creat(mount.context(), path.constData(), 0644, &handle);
        if (rc < 0) {
            if (!isAnAnswerRatherThanABrokenConnection(-rc))
                mount.abandon();
            return Result<std::unique_ptr<QIODevice>>(errorFromNfs(rc, mount.context(), what));
        }
    }

    auto file = std::make_unique<NfsFile>(std::move(mount), handle, -1, what);
    file->commitOnCloseTo(stagingPath, pathFor(target));
    if (!file->open(QIODevice::WriteOnly))
        return Result<std::unique_ptr<QIODevice>>::failure(VfsError::IoError, file->errorString());
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(file.release()));
}

// ---- the factory -----------------------------------------------------------

QList<ConnectionField> NfsFileSystemFactory::connectionFields() const
{
    QList<ConnectionField> fields;

    ConnectionField host;
    host.key = QStringLiteral("host");
    host.label = QStringLiteral("Server");
    host.help = QStringLiteral("Name or address of the machine holding the export");
    fields.append(host);

    ConnectionField exported;
    exported.key = QStringLiteral("export");
    exported.label = QStringLiteral("Export");
    exported.help = QStringLiteral("The export path as the server publishes it, for example /srv/media");
    fields.append(exported);

    ConnectionField root;
    root.key = QStringLiteral("root");
    root.label = QStringLiteral("Folder inside the export");
    root.required = false;
    root.advanced = true;
    fields.append(root);

    ConnectionField uid;
    uid.key = QStringLiteral("uid");
    uid.label = QStringLiteral("User id to claim");
    uid.help = QStringLiteral("Leave empty to use this machine's own. NFS trusts whatever is claimed here, "
                              "so it decides what the export will allow -- it is not a password");
    uid.required = false;
    uid.advanced = true;
    fields.append(uid);

    ConnectionField gid;
    gid.key = QStringLiteral("gid");
    gid.label = QStringLiteral("Group id to claim");
    gid.required = false;
    gid.advanced = true;
    fields.append(gid);

    return fields;
}

NfsSettings NfsFileSystemFactory::settingsFrom(const QVariantMap& config)
{
    NfsSettings settings;
    settings.host = config.value(QStringLiteral("host")).toString().trimmed();
    settings.exportPath = config.value(QStringLiteral("export")).toString().trimmed();
    settings.remoteRoot = config.value(QStringLiteral("root")).toString();

    // An export pasted the way `showmount` prints it -- `server:/srv/media` --
    // is what people have in front of them, so it is taken rather than refused.
    if (settings.exportPath.contains(QLatin1Char(':'))) {
        const QString pasted = settings.exportPath;
        if (settings.host.isEmpty())
            settings.host = pasted.section(QLatin1Char(':'), 0, 0).trimmed();
        settings.exportPath = pasted.section(QLatin1Char(':'), 1);
    }
    while (settings.exportPath.endsWith(kSeparator) && settings.exportPath.size() > 1)
        settings.exportPath.chop(1);
    if (!settings.exportPath.isEmpty() && !settings.exportPath.startsWith(kSeparator))
        settings.exportPath.prepend(kSeparator);

    bool ok = false;
    const int uid = config.value(QStringLiteral("uid")).toString().trimmed().toInt(&ok);
    settings.uid = ok ? uid : -1;
    const int gid = config.value(QStringLiteral("gid")).toString().trimmed().toInt(&ok);
    settings.gid = ok ? gid : -1;
    return settings;
}

FileSystemPtr NfsFileSystemFactory::create(const QVariantMap& config, QString* errorOut)
{
    const NfsSettings settings = settingsFrom(config);
    if (settings.host.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("An NFS drive needs a server");
        return nullptr;
    }
    if (settings.exportPath.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("An NFS drive needs an export");
        return nullptr;
    }
    return std::make_shared<NfsFileSystem>(QStringLiteral("nfs"), settings);
}

} // namespace mole
