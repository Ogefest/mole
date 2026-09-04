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
    /// Where the record of which host had which key lives. Empty means the
    /// account's own `~/.ssh/known_hosts`, which is what every drive uses.
    ///
    /// It is settable so a test can hold the policy to account: a changed key
    /// has to be refused, and the only way to demonstrate that is to record a
    /// key, change it, and come back -- which is not something to do to the
    /// machine the suite is running on.
    QString knownHostsPath;
};

/// What every lease for this drive is prepared with.
///
/// A free function rather than something built inside the constructor, because
/// the identity of a drive is the whole of MOLE-374: it must be in the options
/// the pool applies to every handle, and there must be nowhere else it can be
/// put. A test can hold that here without a server.
net::TransportOptions transportOptionsFor(const SftpSettings& settings);

/// SFTP over libcurl.
///
/// Reads its directory listings out of the text libcurl produces from the
/// protocol's own attributes, and does everything that is not a transfer through
/// SFTP's quote commands -- mkdir, rm, rmdir, rename. That is the whole protocol
/// surface this needs, which is why no SSH library appears here directly.
class SftpFileSystem : public IFileSystem
{
public:
    SftpFileSystem(QString scheme, SftpSettings settings);

    QString scheme() const override { return m_scheme; }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive, const CancelToken& cancel = {}) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to, const CancelToken& cancel = {}) override;

    Result<std::unique_ptr<QIODevice>> openRead(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& cancel = {}) override;
    Result<std::unique_ptr<QIODevice>> openWrite(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& cancel = {}) override;

    Result<AccessInfo> access(const VfsUri& target) override;

protected:
    /// One directory listing, exactly as the server answered it and before
    /// anything has been made of it.
    ///
    /// The seam a test puts a server behind. Everything above it is where
    /// servers that answer differently are reconciled -- what a protocol error
    /// means, what a "." row that is not a directory means, and how a refusal to
    /// list is explained -- and that is the part worth holding to account on
    /// every change rather than on the days somebody has a server to hand.
    /// See tests/plugins/tst_SftpFileSystem.cpp.
    virtual net::Response fetchListing(const VfsUri& dir, const CancelToken& cancel);

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
    Result<void> runCommand(
        const QByteArray& command, const VfsUri& context, const QString& what, const CancelToken& cancel);

    /// Removes what the caller already knows about, without asking again.
    ///
    /// `remove()` began with `stat(target)`, which on this backend is a full
    /// listing of the parent -- and the recursion, which already holds each
    /// child's FileEntry and has just listed the parent once, called
    /// `remove(child.uri, true)` for every child, which listed the same parent
    /// again. A directory of n entries cost n+1 listings of n rows before a
    /// single `rm`, so deleting ten thousand files over SFTP parsed a hundred
    /// million listing rows. See MOLE-368.
    Result<void> removeEntry(const FileEntry& entry, bool recursive, const CancelToken& cancel);

    /// Sends one span of a file, appending to what the last one left. Called
    /// from a stream's own thread, like fetchSpan below.
    VfsError sendSpan(const VfsUri& target, QIODevice& source, bool append, const CancelToken& cancel);

    /// Fetches one span of a file into `sink`. Called from a stream's own
    /// thread, which is why it takes everything it needs as arguments and
    /// touches nothing but the connection pool, which is guarded.
    VfsError fetchSpan(const QByteArray& url, const QString& what, QIODevice& sink, qint64 offset,
        qint64 span, const CancelToken& cancel);

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
