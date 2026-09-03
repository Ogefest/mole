#pragma once

#include "core/vfs/IFileSystem.h"

#include <QString>

namespace mole {

/// Wraps a drive so every operation on it leaves the same line in the log.
///
/// Written once and applied to every mount rather than added to each backend,
/// so local disk, SFTP, S3, an archive and whatever a plugin brings all report
/// themselves identically -- and a backend written next year gets it without
/// knowing this class exists.
///
/// The wrapper is always in place, whether the log is on or not. Diagnosing a
/// fault through a different code path from the one that has the fault is how a
/// problem disappears when it is looked at; the only thing the switch changes is
/// whether a line is written.
class LoggingFileSystem final : public IFileSystem
{
public:
    /// `name` is what the drive is called in the sidebar, so a log covering
    /// several drives says which one each line is about.
    LoggingFileSystem(FileSystemPtr inner, QString name);

    /// The backend underneath, for anything that needs the real object.
    const FileSystemPtr& inner() const { return m_inner; }

    QString scheme() const override;
    VfsCapabilities capabilities() const override;

    /// The volume underneath answers all four of these, and a wrapper that let
    /// IFileSystem's defaults answer instead is not observing a drive -- it is
    /// replacing it with a plausible one. The defaults are deliberately the
    /// permissive and the unsupported answer, so nothing fails loudly: a copy on
    /// to a Windows volume goes back to failing one file at a time part way
    /// through, the guard against moving a directory into its own subtree gets
    /// case folding wrong on a volume that ignores case, and a drive still
    /// advertising ReportsLeftovers through capabilities() below can never be
    /// found to have any. See MOLE-282 and ADR-0070.
    Qt::CaseSensitivity pathCaseSensitivity() const override;
    NameRules nameRules() const override;
    bool understandsVersions() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;
    Result<void> makeDirectory(const VfsUri& target) override;
    Result<QString> readLink(const VfsUri& link) override;
    Result<void> makeLink(const VfsUri& link, const QString& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;
    Result<void> replace(const VfsUri& from, const VfsUri& to) override;
    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<SpaceInfo> space(const VfsUri& target) override;
    Result<AccessInfo> access(const VfsUri& target) override;
    Result<QList<DriveLeftover>> leftovers(
        std::chrono::seconds olderThan, const CancelToken& cancel) override;
    Result<void> discardLeftover(const DriveLeftover& leftover) override;
    Result<FileEntryList> search(
        const VfsUri& root, const QString& pattern, const CancelToken& cancel) override;

    /// The drive underneath decides what it can do, not the wrapper. A decorator
    /// that answered for itself would leave every drive-contributed action
    /// unreachable the moment the mount was wrapped -- which is every mount.
    FileActionList actionsFor(const VfsUri& target, const FileEntry& entry) override;
    Result<FileActionOutcome> invoke(
        const QString& id, const VfsUri& target, const CancelToken& cancel) override;
    /// Also the drive underneath's. What was discovered about a drive belongs to
    /// that drive, and a wrapper keeping an answer of its own would be a second
    /// drive that is asked separately and can disagree.
    Result<QStringList> entriesWithActions(const VfsUri& dir, const CancelToken& cancel) override;
    DriveOffers offers() const override;
    void probe(const VfsUri& target, const CancelToken& cancel) override;

private:
    FileSystemPtr m_inner;
    QString m_name;
};

/// Puts a drive behind the wrapper, unless it already is one. Null in, null out.
FileSystemPtr withLogging(FileSystemPtr fileSystem, const QString& name);

} // namespace mole
