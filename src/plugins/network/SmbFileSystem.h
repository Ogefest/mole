#pragma once

#include "core/platform/HostPlatform.h"
#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <QString>

#include <memory>

struct _SMBCCTX;

namespace mole {

/// What an SMB drive is configured with.
struct SmbSettings
{
    QString host;
    /// The share, without slashes: the `photos` of `\\\\server\\photos`.
    QString share;
    QString username;
    QString password;
    /// Windows domain or workgroup. Empty is right for almost everything that
    /// is not an Active Directory domain.
    QString domain;
    /// Path inside the share this drive is rooted at.
    QString remoteRoot;

    /// `smb://host/share`, without a trailing slash.
    QString origin() const;
};

/// Windows and NAS shares, through libsmbclient.
///
/// Samba's own client library rather than a mount: a Mole drive is virtual and
/// in-application, which is the same reason SSHFS was dropped rather than
/// written -- see ADR-0011. Mounting would need root on the machine running
/// Mole, would not port, and would put a drive in the operating system's
/// namespace where every other application can see it.
///
/// THREADS
/// -------
/// **One session for the process, and every operation serialised behind it.**
/// That is a real cost -- a listing waits for a read -- and it is not the shape
/// this started out as. A context per thread was tried first, the way
/// SqliteTable keeps a connection per thread, and it aborts inside Samba's own
/// allocator.
///
/// The reason is that the context's function pointers are not the entry points.
/// libsmbclient's plain `smbc_*` wrappers do bookkeeping around each call that
/// Samba's internals depend on -- a talloc stackframe -- and calling
/// `smbc_getFunctionStat(ctx)(...)` directly skips it. Samba then runs with no
/// stackframe, leaks into its arena, and aborts later somewhere unrelated:
/// "no talloc stackframe", then "Bad talloc magic value". The wrappers act on
/// one global context, so using them means having one.
///
/// The mutex therefore protects two process-wide things rather than one: the
/// context every wrapper acts on, and the credentials the authentication
/// callback reads back, which belong to whichever drive is asking.
class SmbFileSystem final : public IFileSystem
{
public:
    SmbFileSystem(QString scheme, SmbSettings settings);
    ~SmbFileSystem() override;

    SmbFileSystem(const SmbFileSystem&) = delete;
    SmbFileSystem& operator=(const SmbFileSystem&) = delete;

    QString scheme() const override { return m_scheme; }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override;

    /// The url libsmbclient wants for a uri: smb://host/share/root/path.
    QString urlFor(const VfsUri& uri) const;

    /// What the authentication callback is handed back. Public because
    /// libsmbclient's callback is a free function and gets nothing but the
    /// pointer it was told to keep.
    struct Credentials;

    /// Holds the session for the length of one operation and points the
    /// authentication callback at this drive's credentials.
    ///
    /// Everything that talks to a share takes one of these first, including a
    /// read from an open file: the wrappers act on the global context whoever
    /// last claimed it set up.
    class Session
    {
    public:
        explicit Session(const SmbFileSystem& drive);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        /// False when no session could be started at all, which is a machine
        /// that cannot do SMB rather than a share that cannot be reached.
        bool ok() const { return m_ok; }

    private:
        bool m_ok = false;
    };

private:
    QString m_scheme;
    SmbSettings m_settings;
    /// Held so the authentication callback can find the credentials.
    std::unique_ptr<Credentials> m_credentials;
};

class SmbFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("smb"); }
    QString displayName() const override { return QStringLiteral("Windows share (SMB)"); }
    QString iconName() const override { return QStringLiteral("\U0001F5A5"); }

    QList<ConnectionField> connectionFields() const override;
    /// Not a kind of drive on Windows, where a share is \\server\share and is
    /// reached by the local filesystem. Offering it there -- greyed out or not --
    /// would point somebody at a library they do not need.
    bool isApplicable() const override { return hostPlatform() != HostPlatform::Windows; }
    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;

    static SmbSettings settingsFrom(const QVariantMap& config);
};

} // namespace mole
