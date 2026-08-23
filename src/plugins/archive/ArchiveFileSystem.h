#pragma once

#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <QDateTime>
#include <QHash>
#include <QMutex>

namespace mole {

/// Browses inside a zip/tar/7z/... as if it were a drive.
///
/// Mounting an archive rather than special-casing it in the browser is what
/// makes "open a .zip and walk around in it" fall out of the existing design:
/// listing, searching, previewing and indexing already work on any mount.
///
/// Read-only. Writing into archives would mean rewriting the container, which
/// is a different feature with different failure modes.
class ArchiveFileSystem final : public IFileSystem
{
public:
    explicit ArchiveFileSystem(QString archivePath);

    /// The uri authority that addresses this archive. Percent-encoded so the
    /// host path cannot be confused with the path inside the archive.
    static QString authorityFor(const QString& archivePath);
    static QString archivePathFromAuthority(const QString& authority);

    QString scheme() const override { return QStringLiteral("archive"); }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;
    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;

    QString archivePath() const { return m_archivePath; }

private:
    struct Node
    {
        bool isDir = false;
        qint64 size = 0;
        QDateTime modified;
    };

    /// Reads the central directory once and caches it. Cheap for zip, a full
    /// pass for stream formats like tar.gz -- either way, only once.
    Result<void> ensureIndexed();

    QString m_archivePath;
    mutable QMutex m_mutex;
    QHash<QString, Node> m_nodes;
    bool m_indexed = false;
};

class ArchiveFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("archive"); }
    QString displayName() const override { return QStringLiteral("Archive (zip, tar, 7z, ...)"); }
    QString iconName() const override { return QStringLiteral("\U0001F5DC"); }

    QList<ConnectionField> connectionFields() const override;
    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;

    QStringList mountableFileSuffixes() const override { return supportedSuffixes(); }
    QVariantMap configForFile(const QString& localPath) const override;
    VfsUri rootUriForFile(const QString& localPath) const override;
    /// Rebuilds the config from a root uri, because the authority is the path.
    /// See IFileSystemFactory::configForRoot().
    QVariantMap configForRoot(const VfsUri& root) const override;

    /// Suffixes that should offer "open as drive" in the browser.
    static QStringList supportedSuffixes();
    static bool looksLikeArchive(const QString& fileName);
};

} // namespace mole
