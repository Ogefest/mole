#pragma once

#include "plugins/network/CurlTransport.h"

#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <memory>

namespace mole {

/// What an SFTP drive needs to know to connect.
struct SftpSettings
{
    QString host;
    int port = 22;
    QString username;
    QString password;
    /// Key authentication, as an alternative or an addition to a password.
    QString privateKeyPath;
    QString privateKeyPassphrase;
    /// Absolute path on the server this drive is rooted at; everything the drive
    /// shows is underneath it.
    QString remoteRoot = QStringLiteral("/");
    /// Trust a host we have not met before, recording its key. A host whose key
    /// has *changed* is refused either way -- see ADR-0011.
    bool acceptNewHostKey = true;
};

/// SFTP over libcurl.
///
/// Reads its directory listings out of the text libcurl produces from the
/// protocol's own attributes, and does everything that is not a transfer through
/// SFTP's quote commands -- mkdir, rm, rmdir, rename. That is the whole protocol
/// surface this needs, which is why no SSH library appears here directly.
class SftpFileSystem final : public IFileSystem
{
public:
    SftpFileSystem(QString scheme, SftpSettings settings);

    QString scheme() const override { return m_scheme; }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target) override;

    Result<AccessInfo> access(const VfsUri& target) override;

private:
    /// Absolute path on the server for a uri in this drive.
    QString remotePath(const VfsUri& uri) const;
    QByteArray urlFor(const VfsUri& uri, bool asDirectory) const;

    /// Lists without trying to explain a failure. list() adds the explanation and
    /// stat() needs the plain version, which is also what keeps the two from
    /// calling each other in a circle.
    Result<FileEntryList> listRaw(const VfsUri& dir, const CancelToken& cancel);

    /// Runs one SFTP quote command against the parent of `context`, which is a
    /// directory known to exist -- curl needs a workable url even when all the
    /// work is in the command.
    Result<void> runCommand(const QByteArray& command, const VfsUri& context, const QString& what);

    Result<void> uploadTo(const VfsUri& target, QIODevice& payload, qint64 size);

    QString m_scheme;
    SftpSettings m_settings;
    std::unique_ptr<net::CurlPool> m_pool;
};

class SftpFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("sftp"); }
    QString displayName() const override { return QStringLiteral("SSH / SFTP"); }
    QString iconName() const override { return QStringLiteral("\U0001F5A5"); }

    QList<ConnectionField> connectionFields() const override;
    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;

    /// Builds the settings from a filled-in form. Public so a test can reach it
    /// without going through the registry.
    static SftpSettings settingsFrom(const QVariantMap& config);
};

} // namespace mole
