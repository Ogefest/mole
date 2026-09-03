#pragma once

#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <QFile>

namespace mole {

/// The reference backend: plain local disk through QFileInfo/QDirIterator.
/// Read it as the worked example when writing sftp/s3/webdav.
class LocalFileSystem final : public IFileSystem
{
public:
    QString scheme() const override { return QStringLiteral("file"); }
    VfsCapabilities capabilities() const override;

    /// What the platform does, which is what the volume usually does: NTFS and a
    /// default APFS volume fold case, ext4 does not. A volume that disagrees with
    /// its platform -- a case-sensitive APFS one, a FAT stick on Linux -- is not
    /// asked, because Qt has no portable way to ask it and guessing wrong in the
    /// permissive direction is what loses a file.
    Qt::CaseSensitivity pathCaseSensitivity() const override
    {
        return VfsUri::caseSensitivityFor(QStringLiteral("file"));
    }

    /// What the local filesystem accepts, which is the platform's answer. A
    /// stricter volume mounted on it -- a FAT stick, a share -- is not asked,
    /// because Qt has no portable way to ask one.
    NameRules nameRules() const override { return NameRules::forPlatform(); }

    /// "rwxr-xr--" on a platform that has such a thing, and empty on one that
    /// does not.
    ///
    /// One answer, asked by both the listing and the details, because this class
    /// used to hold two. access() guarded the question with an #ifdef and
    /// entryFromInfo() eighty lines above it did not, so on Windows a file's
    /// details drawer showed no mode and the listing it was opened from showed
    /// rw-rw-rw- for the same file.
    ///
    /// Public and taking a platform so the question can be asked about a system
    /// this build is not running on -- which is the only way the Windows answer
    /// is ever checked.
    static QString modeString(QFile::Permissions permissions, HostPlatform platform = hostPlatform());

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<SpaceInfo> space(const VfsUri& target) override;
    Result<AccessInfo> access(const VfsUri& target) override;
    Result<FileEntry> stat(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<QString> readLink(const VfsUri& link) override;
    Result<void> makeLink(const VfsUri& link, const QString& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;

    /// One step where the platform has one: rename(2) puts a file over a file
    /// with no instant in between at which the name has nothing at it. Across
    /// two kinds -- a directory arriving over a file -- no filesystem can, and
    /// it falls back to the interface's remove-then-rename. See ADR-0087.
    Result<void> replace(const VfsUri& from, const VfsUri& to) override;

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override;

    // ---- earlier states of a file, where the filesystem keeps them ---------
    //
    // The cheapest source of earlier versions there will ever be: no library, no
    // privileged call, no daemon. A dataset that exposes its snapshots hangs
    // them under a fixed hidden directory at its own root, and an earlier state
    // of a file is the same relative path inside one of them -- an ordinary path,
    // readable with the readdir and stat this class already uses.
    //
    // ADR-0051 settled what Mole does with them: it lists and opens what the
    // filesystem exposes and manages nothing. Nothing here creates, deletes or
    // rolls back to a snapshot, and nothing reads a filesystem property through
    // any library or command.

    /// True, and it means it: a uri naming a version resolves into the snapshot
    /// that carries it, so every read goes through the ordinary path.
    bool understandsVersions() const override { return true; }

    FileActionList actionsFor(const VfsUri& target, const FileEntry& entry) override;
    Result<FileActionOutcome> invoke(
        const QString& id, const VfsUri& target, const CancelToken& cancel) override;
    Result<QStringList> entriesWithActions(const VfsUri& dir, const CancelToken& cancel) override;

    /// The action this backend contributes. Public so a test can name it without
    /// spelling the string twice.
    static QString versionsActionId() { return QStringLiteral("org.mole.local.versions"); }

    /// Where a dataset exposes its snapshots, relative to its own root.
    ///
    /// One convention, named. It is what makes discovery possible at all: the
    /// question is *does this ancestor have that directory*. A subvolume snapshot
    /// that can live anywhere has nothing to discover, so it is not this ticket
    /// and not this constant -- a second convention is a second one of these, and
    /// nothing above the backend changes when it arrives.
    static QString snapshotDirectory() { return QStringLiteral(".zfs/snapshot"); }

protected:
    Result<QStringList> askWhatIsOffered(const VfsUri& target, const CancelToken& cancel) override;

private:
    /// The path to read for `uri`, which is the path itself unless the uri names
    /// a version -- then it is that path inside the snapshot. Empty when the uri
    /// is not local, or names a version of something under no snapshot root.
    QString localPathFor(const VfsUri& uri) const;
    /// The nearest ancestor of `localPath` that exposes snapshots, or empty.
    /// `localPath` is in '/' form.
    QString snapshotRootFor(const QString& localPath) const;
    /// What is under a root's snapshot directory, newest name last. One readdir.
    static QStringList snapshotsUnder(const QString& root);
    /// `localPath` as it appears inside `snapshot`.
    static QString insideSnapshot(const QString& root, const QString& snapshot, const QString& localPath);
};

class LocalFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("file"); }
    QString displayName() const override { return QStringLiteral("Local disk"); }
    QString iconName() const override { return QStringLiteral("drive-harddisk"); }

    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;
};

} // namespace mole
