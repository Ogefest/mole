#pragma once

#include "plugins/network/CurlTransport.h"
#include "plugins/network/WebdavListing.h"

#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <memory>

namespace mole {

/// What a WebDAV drive is configured with.
struct WebdavSettings
{
    /// Full base url of the collection, e.g.
    /// "https://cloud.example.com/remote.php/dav/files/lukasz".
    QString baseUrl;
    QString username;
    QString password;
    bool verifyTls = true;
    /// Path inside the base url this drive is rooted at.
    QString remoteRoot;

    /// Scheme and host, without a trailing slash: "https://cloud.example.com".
    QString origin() const;
    /// Path part of the base url, without a trailing slash.
    QString basePath() const;
};

/// WebDAV over libcurl, which is what reaches Nextcloud and ownCloud.
class WebdavFileSystem final : public IFileSystem
{
public:
    WebdavFileSystem(QString scheme, WebdavSettings settings);

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

private:
    /// Server path for a uri: the base path plus the drive root plus the uri.
    QString remotePath(const VfsUri& uri) const;
    QByteArray urlFor(const VfsUri& uri) const;

    struct Call
    {
        QByteArray method = "GET";
        QByteArray url;
        QByteArray body;
        net::HeaderList headers;
        QIODevice* payload = nullptr;
        /// -1 sends without a length, which curl does as chunked transfer
        /// encoding. Only for a payload whose size genuinely is not known yet.
        qint64 payloadSize = 0;
    };
    net::Response send(const Call& call, const CancelToken& cancel, QIODevice* sink = nullptr);

    /// One span of a ranged read, with the ETag the read began on attached.
    ///
    /// The unit StreamingDownload asks for. `validator` empty means the server
    /// gave no ETag, and then the file's identity is checked the slower way --
    /// see MOLE-370.
    VfsError fetchSpan(const QByteArray& url, const QByteArray& validator, const QString& what,
        QIODevice& sink, qint64 offset, qint64 span, const CancelToken& cancel);

    /// One PROPFIND, at the given depth.
    Result<QList<net::WebdavEntry>> propfind(const VfsUri& target, int depth, const CancelToken& cancel);
    /// Sends a payload as the body of a PUT. A `size` of -1 means the length is
    /// not known and the request goes out chunked.
    Result<void> uploadFrom(const VfsUri& target, QIODevice& payload, qint64 size, const CancelToken& cancel);

    QString m_scheme;
    WebdavSettings m_settings;
    std::unique_ptr<net::CurlPool> m_pool;
};

class WebdavFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("webdav"); }
    QString displayName() const override { return QStringLiteral("WebDAV (Nextcloud, ownCloud, ...)"); }
    QString iconName() const override { return QStringLiteral("\U0001F310"); }

    QList<ConnectionField> connectionFields() const override;
    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;

    static WebdavSettings settingsFrom(const QVariantMap& config);
};

} // namespace mole
