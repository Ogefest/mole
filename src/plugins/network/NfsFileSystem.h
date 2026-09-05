#pragma once

#include "core/platform/HostPlatform.h"
#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <QString>

struct nfs_context;

namespace mole {

/// What an NFS drive is configured with.
struct NfsSettings
{
    QString host;
    /// The export as the server publishes it -- `/srv/media`, the thing
    /// `showmount -e` prints. Not a path inside the export.
    QString exportPath;
    /// Path inside the export this drive is rooted at.
    QString remoteRoot;

    /// Who the server is told this is.
    ///
    /// NFS has no user authentication worth the name: the export list decides
    /// *who may mount*, and after that the server believes whatever user id the
    /// client claims for *what may be done*. So these are not credentials and
    /// must not be offered as any -- they are a claim. Negative means "the ids
    /// this process is running as", which is right whenever the accounts on both
    /// machines line up, and is the only sensible default.
    int uid = -1;
    int gid = -1;

    /// Everything that decides which mount a drive is talking to. Two drives
    /// that agree on this can share a connection; two that do not, cannot.
    QString mountKey() const;
};

/// An NFS export, through libnfs.
///
/// A userspace client rather than a kernel mount, for the reason ADR-0011 gives
/// for dropping SSHFS: a Mole drive is virtual and in-application. Mounting
/// would need root on the machine running Mole, would not port, and would put
/// the export into the operating system's namespace where every other
/// application can see it.
class NfsFileSystem final : public IFileSystem
{
public:
    NfsFileSystem(QString scheme, NfsSettings settings);
    ~NfsFileSystem() override;

    NfsFileSystem(const NfsFileSystem&) = delete;
    NfsFileSystem& operator=(const NfsFileSystem&) = delete;

    /// A mounted connection, on loan.
    ///
    /// A libnfs context is a connection and a position in the protocol, and it
    /// is not safe to use from two threads at once -- so a drive cannot simply
    /// hold one, because Mole calls a backend from whichever pool thread picked
    /// the work up. It cannot be one context per thread either: a handle belongs
    /// to the context it was opened on, and an open file is handed to whatever
    /// reads it, which is a different thread from the one that opened it as
    /// often as not.
    ///
    /// So a context is leased. An operation borrows one for its own duration; an
    /// open file borrows one for as long as it is open, which is what keeps its
    /// handle and its context together on whatever thread does the reading.
    /// Mounting costs two round trips, so returned contexts are kept and reused.
    /// ADR-0050 records the alternatives.
    class Mount
    {
    public:
        Mount() = default;
        explicit Mount(const NfsSettings& settings);
        ~Mount();

        Mount(Mount&& other) noexcept;
        Mount& operator=(Mount&& other) noexcept;
        Mount(const Mount&) = delete;
        Mount& operator=(const Mount&) = delete;

        bool ok() const { return m_context != nullptr; }
        nfs_context* context() const { return m_context; }
        /// Why there is no connection, meaningful when ok() is false.
        ///
        /// The whole error and not just its words: an export that refuses this
        /// client is AccessDenied, and reporting every failed mount as
        /// NetworkError told the sidebar a drive was unreachable when it was
        /// reachable and saying no. See MOLE-373.
        const VfsError& failure() const { return m_failure; }

        /// Close this connection instead of returning it for reuse. A context
        /// whose transport has failed answers every later call with the same
        /// failure, so handing it back would poison the drive.
        void abandon() { m_reusable = false; }

    private:
        void giveBack();

        QString m_key;
        nfs_context* m_context = nullptr;
        bool m_reusable = true;
        VfsError m_failure;
    };

    QString scheme() const override { return m_scheme; }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive, const CancelToken& cancel = {}) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to, const CancelToken& cancel = {}) override;
    /// One nfs_rename(), because a POSIX rename replaces atomically -- the
    /// default's remove-then-rename would open a window this protocol does
    /// not need. See ADR-0087.
    Result<void> replace(const VfsUri& from, const VfsUri& to, const CancelToken& cancel = {}) override;

    Result<std::unique_ptr<QIODevice>> openRead(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& cancel = {}) override;
    Result<std::unique_ptr<QIODevice>> openWrite(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& cancel = {}) override;

    /// The path inside the export for a uri: the drive's root, then the uri.
    /// Always absolute, because that is what libnfs asks for.
    QString pathFor(const VfsUri& uri) const;

    const NfsSettings& settings() const { return m_settings; }

    /// Closes every pooled connection. For tests and for shutdown -- nothing in
    /// ordinary use needs it, and calling it while an operation is in flight is
    /// not the mistake it looks like: a leased context is not in the pool.
    static void forgetPooledMounts();

    // ---- whether libnfs can be asked for a context at all ------------------
    //
    // **libnfs 6.0 segfaults in nfs_init_context() where getlogin() answers
    // nothing.** It calls rpc_set_username() with the process's login name and
    // strdup()s it without looking, and getlogin() answers nothing in a
    // container, under a systemd service, in a cron job, and in any session with
    // no utmp entry -- while getpwuid(getuid()) still names the account, which
    // libnfs does not ask. Nothing can be caught after the call, because the
    // crash is inside the call that hands the context back, so the only place to
    // stand is in front of it. See MOLE-411.

    /// Whether this session has a login name -- `getlogin()`, which is what
    /// libnfs reads and the only thing that decides this.
    static bool sessionHasALoginName();

    /// Whether the libnfs this was built against reads that name on the way to a
    /// context. True for 6.x, asked of the declaration rather than of a version
    /// number -- see the note in the implementation.
    static bool libraryReadsTheLoginName();

    /// Why a context cannot be asked for, given those two facts, and a valid
    /// error when it can.
    ///
    /// **Pure, and takes the facts rather than reading them**, so the decision is
    /// assertable on a machine with libnfs 5 and a login name -- which is every
    /// machine here, and none of the ones this crashes on.
    static VfsError whyThereIsNoNfsHere(bool haveLoginName, bool libraryReadsIt);

private:
    QString m_scheme;
    NfsSettings m_settings;
};

class NfsFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("nfs"); }
    QString displayName() const override { return QStringLiteral("NFS export"); }
    QString iconName() const override { return QStringLiteral("\U0001F5C4"); }

    QList<ConnectionField> connectionFields() const override;
    /// Not a kind of drive on Windows either: an NFS export is reached through
    /// the operating system's own client there, not through libnfs.
    bool isApplicable() const override { return hostPlatform() != HostPlatform::Windows; }
    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;

    static NfsSettings settingsFrom(const QVariantMap& config);
};

} // namespace mole
