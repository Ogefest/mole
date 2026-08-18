#pragma once

#include "plugins/network/CurlTransport.h"

#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <memory>

namespace mole {

/// What an FTP drive is configured with.
struct FtpSettings
{
    enum class Security {
        /// Refuse to connect without TLS. The right answer whenever the server
        /// offers it, because FTP's own login is plain text.
        Require,
        /// Use TLS when the server offers it, plain otherwise.
        Try,
        /// Never. Only for a server on a network you already trust.
        None
    };

    QString host;
    int port = 21;
    QString username;
    QString password;
    Security security = Security::Try;
    /// Passive mode, which is what works through almost every firewall.
    bool passive = true;
    /// Check the server's certificate against the machine's trust store.
    ///
    /// On by default and worth leaving on. Off is for a server whose
    /// certificate is honestly self-signed -- one on your own network, or a
    /// disposable one a test suite is pointed at -- where the alternative is
    /// either no encryption at all or teaching every client machine to trust a
    /// certificate authority invented for the occasion. S3 already carries the
    /// same setting for the same reason.
    bool verifyTls = true;
    QString remoteRoot = QStringLiteral("/");
};

/// FTP and FTPS over libcurl.
///
/// The listing is whatever the server prints for LIST, so it goes through the
/// same lenient parser as SFTP -- with the difference that here an unrecognised
/// line really can appear, and is dropped rather than guessed at.
class FtpFileSystem final : public IFileSystem
{
public:
    FtpFileSystem(QString scheme, FtpSettings settings);

    QString scheme() const override { return m_scheme; }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override;

private:
    QString remotePath(const VfsUri& uri) const;
    QByteArray urlFor(const VfsUri& uri, bool asDirectory) const;
    void applySettings(const net::CurlPool::Lease& lease) const;

    Result<FileEntryList> listRaw(const VfsUri& dir, const CancelToken& cancel);
    /// Runs raw FTP commands against a directory that exists.
    Result<void> runCommands(const QList<QByteArray>& commands, const VfsUri& context, const QString& what);
    /// Sends what `source` hands over, appending to the file rather than
    /// replacing it when `append` is set. One span of a streamed upload; runs on
    /// the stream's own thread.
    VfsError sendSpan(const VfsUri& target, QIODevice& source, bool append, const CancelToken& cancel);
    /// One ranged fetch into `sink`. Both ends of the range are set, and the
    /// server honours both -- see the comment on the definition for why that had
    /// to be measured rather than assumed.
    VfsError fetchSpan(const QByteArray& url, const QString& what, QIODevice& sink, qint64 offset,
        qint64 span, const CancelToken& cancel);

    QString m_scheme;
    FtpSettings m_settings;
    std::unique_ptr<net::CurlPool> m_pool;
};

class FtpFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("ftp"); }
    QString displayName() const override { return QStringLiteral("FTP / FTPS"); }
    QString iconName() const override { return QStringLiteral("\U0001F4C1"); }

    QList<ConnectionField> connectionFields() const override;
    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;

    static FtpSettings settingsFrom(const QVariantMap& config);
};

} // namespace mole
