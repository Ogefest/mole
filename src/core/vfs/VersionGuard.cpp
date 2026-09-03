#include "core/vfs/VersionGuard.h"

#include <utility>

namespace mole {

VersionGuard::VersionGuard(FileSystemPtr inner)
    : m_inner(std::move(inner))
{
}

bool VersionGuard::passes(const VfsUri& target) const
{
    return !target.hasVersion() || m_inner->understandsVersions();
}

VfsError VersionGuard::refusal(const VfsUri& target)
{
    // The version is in the message. Somebody looking at a bookmark that has
    // stopped working needs to know it names a state of the file rather than
    // the file, which is not visible from the name of it.
    return VfsError::make(VfsError::NotSupported,
        QStringLiteral("This drive cannot open an earlier version of a file (%1 of %2)")
            .arg(target.version(), target.withoutVersion().toString()));
}

QString VersionGuard::scheme() const
{
    return m_inner->scheme();
}

VfsCapabilities VersionGuard::capabilities() const
{
    return m_inner->capabilities();
}

Qt::CaseSensitivity VersionGuard::pathCaseSensitivity() const
{
    return m_inner->pathCaseSensitivity();
}

NameRules VersionGuard::nameRules() const
{
    return m_inner->nameRules();
}

bool VersionGuard::understandsVersions() const
{
    return m_inner->understandsVersions();
}

Result<FileEntryList> VersionGuard::list(const VfsUri& dir, const CancelToken& cancel)
{
    if (!passes(dir))
        return refusal(dir);
    return m_inner->list(dir, cancel);
}

Result<FileEntry> VersionGuard::stat(const VfsUri& target)
{
    if (!passes(target))
        return refusal(target);
    return m_inner->stat(target);
}

Result<void> VersionGuard::makeDirectory(const VfsUri& target)
{
    if (!passes(target))
        return Result<void>(refusal(target));
    return m_inner->makeDirectory(target);
}

Result<void> VersionGuard::remove(const VfsUri& target, bool recursive)
{
    if (!passes(target))
        return Result<void>(refusal(target));
    return m_inner->remove(target, recursive);
}

Result<void> VersionGuard::rename(const VfsUri& from, const VfsUri& to)
{
    if (!passes(from))
        return Result<void>(refusal(from));
    if (!passes(to))
        return Result<void>(refusal(to));
    return m_inner->rename(from, to);
}

Result<void> VersionGuard::replace(const VfsUri& from, const VfsUri& to)
{
    if (!passes(from))
        return Result<void>(refusal(from));
    if (!passes(to))
        return Result<void>(refusal(to));
    return m_inner->replace(from, to);
}

Result<std::unique_ptr<QIODevice>> VersionGuard::openRead(const VfsUri& target, qint64 expectedSize)
{
    if (!passes(target))
        return refusal(target);
    return m_inner->openRead(target, expectedSize);
}

Result<std::unique_ptr<QIODevice>> VersionGuard::openWrite(const VfsUri& target, qint64 expectedSize)
{
    if (!passes(target))
        return refusal(target);
    return m_inner->openWrite(target, expectedSize);
}

Result<SpaceInfo> VersionGuard::space(const VfsUri& target)
{
    if (!passes(target))
        return refusal(target);
    return m_inner->space(target);
}

Result<AccessInfo> VersionGuard::access(const VfsUri& target)
{
    if (!passes(target))
        return refusal(target);
    return m_inner->access(target);
}

Result<QList<DriveLeftover>> VersionGuard::leftovers(
    std::chrono::seconds olderThan, const CancelToken& cancel)
{
    return m_inner->leftovers(olderThan, cancel);
}

Result<void> VersionGuard::discardLeftover(const DriveLeftover& leftover)
{
    return m_inner->discardLeftover(leftover);
}

Result<FileEntryList> VersionGuard::search(
    const VfsUri& root, const QString& pattern, const CancelToken& cancel)
{
    if (!passes(root))
        return refusal(root);
    return m_inner->search(root, pattern, cancel);
}

FileActionList VersionGuard::actionsFor(const VfsUri& target, const FileEntry& entry)
{
    // Nothing rather than a refusal: there is nowhere in a list of actions to
    // put an error, and a drive that cannot reach the node can do nothing to it.
    if (!passes(target))
        return {};
    return m_inner->actionsFor(target, entry);
}

Result<FileActionOutcome> VersionGuard::invoke(
    const QString& id, const VfsUri& target, const CancelToken& cancel)
{
    if (!passes(target))
        return refusal(target);
    return m_inner->invoke(id, target, cancel);
}

Result<QStringList> VersionGuard::entriesWithActions(const VfsUri& dir, const CancelToken& cancel)
{
    if (!passes(dir))
        return refusal(dir);
    return m_inner->entriesWithActions(dir, cancel);
}

DriveOffers VersionGuard::offers() const
{
    return m_inner->offers();
}

void VersionGuard::probe(const VfsUri& target, const CancelToken& cancel)
{
    // Probed at the node itself rather than refused: what a drive can offer is a
    // property of the drive, and a versioned uri names the same place.
    m_inner->probe(target.withoutVersion(), cancel);
}

FileSystemPtr withVersionGuard(FileSystemPtr fileSystem)
{
    if (!fileSystem || std::dynamic_pointer_cast<VersionGuard>(fileSystem))
        return fileSystem;
    return std::make_shared<VersionGuard>(std::move(fileSystem));
}

} // namespace mole
