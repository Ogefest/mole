#pragma once

#include "core/vfs/IFileSystem.h"

namespace mole {

/// Refuses a uri that names an earlier version of a file, on a drive that does
/// not know what one is.
///
/// Written once and put on every mount rather than added to each backend, for
/// the reason LoggingFileSystem's header gives about itself: a backend written
/// next year gets it without knowing this class exists. The alternative was a
/// guard at the top of every method of every backend -- some ninety of them --
/// which is ninety chances to forget one, and forgetting one is not a crash. It
/// is the current file shown as an earlier version.
///
/// A drive that answers understandsVersions() gets everything unchanged, so this
/// is a pass-through the moment a backend implements them.
class VersionGuard final : public IFileSystem
{
public:
    explicit VersionGuard(FileSystemPtr inner);

    /// The backend underneath, for anything that needs the real object.
    const FileSystemPtr& inner() const { return m_inner; }

    QString scheme() const override;
    VfsCapabilities capabilities() const override;
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
    FileActionList actionsFor(const VfsUri& target, const FileEntry& entry) override;
    Result<FileActionOutcome> invoke(
        const QString& id, const VfsUri& target, const CancelToken& cancel) override;
    Result<QStringList> entriesWithActions(const VfsUri& dir, const CancelToken& cancel) override;
    DriveOffers offers() const override;
    void probe(const VfsUri& target, const CancelToken& cancel) override;

private:
    /// Whether `target` is something the drive underneath can be asked about.
    bool passes(const VfsUri& target) const;
    static VfsError refusal(const VfsUri& target);

    FileSystemPtr m_inner;
};

/// Puts a drive behind the guard, unless it already is one. Null in, null out.
FileSystemPtr withVersionGuard(FileSystemPtr fileSystem);

} // namespace mole
