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
    /// What a link member points at, as the archive stores it -- so extracting a
    /// tree of links puts links on the disk rather than empty files. Reading
    /// only: nothing here can be written. See ADR-0092.
    Result<QString> readLink(const VfsUri& link) override;
    Result<std::unique_ptr<QIODevice>> openRead(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& cancel = {}) override;

    QString archivePath() const { return m_archivePath; }

private:
    struct Node
    {
        bool isDir = false;
        qint64 size = 0;
        QDateTime modified;
        /// Which header this entry is, counted from zero as the index is built.
        ///
        /// Because a name does not identify a member: `zip -u` appends a new
        /// version rather than replacing the old one, so two headers carry the
        /// same name, and the index keeps the last while a reader looking the
        /// name up found the first. The listing described one and every read
        /// handed back the other. See MOLE-352.
        int ordinal = -1;
        /// What a symbolic link points at, as the archive states it. Empty for
        /// everything else.
        QString linkTarget;
        bool isSymlink = false;
        /// The member a hard link names. Its bytes are the ones to read: a hard
        /// link entry carries none of its own.
        QString hardLinkTo;
        /// A pipe, a socket, a device node or a link to nothing -- an archive can
        /// hold all of them, and each one copied as an empty file is a copy that
        /// silently did not happen. See SpecialKind and MOLE-333.
        SpecialKind special = SpecialKind::None;
    };

    /// Reads the central directory once and caches it. Cheap for zip, a full
    /// pass for stream formats like tar.gz -- either way, only once.
    Result<void> ensureIndexed();

    /// One node, shaped as the listing and stat() both want it.
    FileEntry entryFor(const VfsUri& uri, const QString& name, const Node& node) const;

    /// Where a link points *inside this archive*, or empty when it points at
    /// something the archive does not contain.
    QString resolvedLinkTarget(const QString& from, const QString& target) const;

    QString m_archivePath;
    mutable QMutex m_mutex;
    QHash<QString, Node> m_nodes;
    /// The children of each directory, built with the index.
    ///
    /// list() used to scan every key for a prefix, which for a kernel tarball --
    /// eighty thousand entries and as many directories to walk -- is hundreds of
    /// millions of string comparisons, all under the mutex. See MOLE-352.
    QHash<QString, QStringList> m_children;
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
