#pragma once

#include "plugins/network/CurlTransport.h"
#include "plugins/network/S3Listing.h"
#include "plugins/network/S3Signer.h"

#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <memory>

namespace mole {

/// What an S3 drive is configured with.
///
/// The endpoint and the addressing style are ordinary fields rather than a
/// variant per provider, and that is the whole reason one engine serves AWS,
/// Backblaze B2, MinIO, Ceph, Wasabi and R2 alike -- see ADR-0011.
struct S3Settings
{
    QString accessKeyId;
    QString secretAccessKey;
    QString region = QStringLiteral("us-east-1");
    /// Host only, no scheme: "s3.us-east-005.backblazeb2.com". Empty means AWS,
    /// derived from the region.
    QString endpoint;
    QString bucket;
    /// What the user asked for: put the bucket in the path rather than in the host
    /// name. Required by MinIO and most Ceph installs; AWS and B2 accept either.
    /// Ask usesPathStyle() for what will actually happen.
    bool pathStyleAddressing = false;
    bool useHttps = true;
    bool verifyTls = true;
    /// Key prefix this drive is rooted at, without a trailing slash.
    QString prefix;

    /// Whether the bucket goes in the path. True when it was asked for, and also
    /// whenever the bucket name could not go in the host name even if we tried.
    bool usesPathStyle() const;

    /// Whether this bucket name can be a single host label.
    ///
    /// A wildcard certificate covers exactly one label, so a bucket with a dot in
    /// its name -- "my.backups" -- cannot be addressed through the host: TLS
    /// verification fails with "no alternative certificate subject name matches
    /// target host name". That reads like a broken server rather than like a name
    /// that was never going to work, so it is not left to be discovered.
    bool bucketFitsInHostName() const;

    /// The host requests go to, once the addressing style is applied.
    QString hostName() const;
    /// "s3.<region>.amazonaws.com" when no endpoint was given.
    QString resolvedEndpoint() const;
};

/// An S3-compatible object store as a drive.
///
/// S3 has no directories, so they are the usual convention: a zero-byte object
/// whose key ends in "/". A prefix that merely has keys under it is treated as a
/// directory too, because that is what a bucket written by anything else looks
/// like -- but only the marker is created, so what Mole writes is what the AWS
/// console would write.
class S3FileSystem final : public IFileSystem
{
public:
    S3FileSystem(QString scheme, S3Settings settings);

    QString scheme() const override { return m_scheme; }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override;

    /// Uploads begun and never finished, which S3 keeps -- and charges for --
    /// until somebody says otherwise. See DriveLeftover.
    Result<QList<DriveLeftover>> leftovers(
        std::chrono::seconds olderThan, const CancelToken& cancel) override;
    Result<void> discardLeftover(const DriveLeftover& leftover) override;

private:
    /// One signed request, described before it is sent.
    struct Call
    {
        QByteArray method = "GET";
        /// Object key, without a leading slash. Empty addresses the bucket.
        QString key;
        QList<QPair<QString, QString>> query;
        net::HeaderList headers;
        QIODevice* body = nullptr;
        qint64 bodySize = 0;
    };

    net::Response send(const Call& call, const CancelToken& cancel, QIODevice* sink = nullptr);

    // ---- multipart upload ------------------------------------------------
    //
    // How an object larger than one request gets there. It is also the only way
    // to send an object without knowing its length first: each part is a request
    // of its own, so only one part has to be measured and signed at a time, and
    // the local cost of writing a hundred-gigabyte object is one part.

    /// Starts one, and returns the id every later request has to carry.
    Result<QString> beginMultipart(const QString& key);
    /// Sends one part, and returns the tag the server gives it -- completing the
    /// upload means handing all of them back in order.
    Result<QByteArray> uploadPart(
        const QString& key, const QString& uploadId, int partNumber, QIODevice& body, qint64 size);
    /// Assembles the parts into the object.
    Result<void> completeMultipart(
        const QString& key, const QString& uploadId, const QList<QByteArray>& tags);
    /// Throws away an upload that cannot be finished. Best effort: it is
    /// housekeeping, and the failure that led here is the one worth reporting.
    /// Without it the parts sit in the bucket being charged for.
    void abandonMultipart(const QString& key, const QString& uploadId);
    /// Every unfinished upload under this drive's prefix, a page at a time.
    Result<QList<net::S3Upload>> listUnfinishedUploads(const CancelToken& cancel);
    /// The VFS error for a finished call, preferring the server's own words.
    VfsError errorFor(const net::Response& response, const QString& what) const;

    /// Object key for a uri, with the drive's prefix applied.
    QString keyFor(const VfsUri& uri) const;
    /// The drive's prefix as a key, without leading or trailing separators.
    /// Empty for a drive rooted at the bucket.
    QString rootKey() const;
    /// The other direction: a key back to the path a reader would recognise,
    /// with the drive's prefix taken off again.
    QString keyToPath(const QString& key) const;
    /// Lists one page under `prefix`, with or without a delimiter.
    Result<net::S3ListPage> listPage(
        const QString& prefix, bool delimited, const QString& token, int maxKeys, const CancelToken& cancel);
    /// Every key under a prefix, following the continuation tokens.
    Result<QList<net::S3Object>> allKeysUnder(const QString& prefix, const CancelToken& cancel);

    Result<void> putObject(const QString& key, QIODevice* body, qint64 size);
    Result<void> copyObject(const QString& fromKey, const QString& toKey);
    Result<void> deleteObject(const QString& key);
    /// True when a key exists exactly as given.
    bool objectExists(const QString& key);

    QString m_scheme;
    S3Settings m_settings;
    net::SigningIdentity m_identity;
    std::unique_ptr<net::CurlPool> m_pool;
};

class S3FileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("s3"); }
    QString displayName() const override { return QStringLiteral("S3 (AWS, Backblaze B2, MinIO, ...)"); }
    QString iconName() const override { return QStringLiteral("\U00002601"); }

    QList<ConnectionField> connectionFields() const override;
    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;

    static S3Settings settingsFrom(const QVariantMap& config);
};

} // namespace mole
