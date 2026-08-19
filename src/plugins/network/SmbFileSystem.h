#pragma once

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
/// libsmbclient's context is not safe to share between threads, and Mole calls
/// a backend from whichever pool thread picked the work up. So a context
/// belongs to a thread and is built the first time that thread asks for one --
/// the same shape SqliteTable uses for its connections, and DelimitedStore for
/// its. The alternative, one context behind a mutex, would serialise every
/// listing in the application behind every read.
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

private:
    /// This thread's context, built on first use. Null when one cannot be made,
    /// which is a configuration that cannot work rather than a network fault.
    _SMBCCTX* context() const;

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
    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;

    static SmbSettings settingsFrom(const QVariantMap& config);
};

} // namespace mole
