#include "core/vfs/DirectoryWalker.h"

namespace mole {

DirectoryWalker::DirectoryWalker(FileSystemPtr fileSystem)
    : m_fileSystem(std::move(fileSystem))
{
}

DirectoryWalker::DirectoryWalker(FileSystemPtr fileSystem, Options options)
    : m_fileSystem(std::move(fileSystem))
    , m_options(options)
{
}

Result<void> DirectoryWalker::walk(const VfsUri& root, const CancelToken& cancel, const Visitor& visit)
{
    return walk(root, cancel, visit, {});
}

Result<void> DirectoryWalker::walk(const VfsUri& root, const CancelToken& cancel, const Visitor& visit,
    const DirectoryVisitor& directoryDone)
{
    m_visited = 0;
    m_stoppedEarly = false;
    m_errors.clear();

    if (!m_fileSystem)
        return Result<void>::failure(VfsError::NotSupported, QStringLiteral("No backend for walk"));

    struct Pending
    {
        VfsUri uri;
        int depth;
    };

    // One list for both orders: depth first takes from the back, breadth first
    // from the front. The front is an index rather than takeFirst(), which would
    // move the whole list along on every directory.
    QList<Pending> pending { { root, 0 } };
    qsizetype front = 0;
    const bool breadthFirst = m_options.order == Order::BreadthFirst;

    while (front < pending.size()) {
        if (cancel.isCancelled())
            return Result<void>::failure(VfsError::Cancelled, QStringLiteral("Walk cancelled"));

        Pending current;
        if (breadthFirst) {
            current = pending.at(front++);
            // Let the consumed half go once it is most of the list, so a long walk
            // does not hold every directory it has ever seen.
            if (front > 1024 && front * 2 > pending.size()) {
                pending.remove(0, front);
                front = 0;
            }
        } else {
            current = pending.takeLast();
        }

        Result<FileEntryList> listing = m_fileSystem->list(current.uri, cancel);
        if (!listing.ok()) {
            if (listing.error().code == VfsError::Cancelled)
                return Result<void>(listing.error());
            m_errors.append(listing.error());
            continue;
        }

        for (const FileEntry& entry : listing.value()) {
            if (cancel.isCancelled())
                return Result<void>::failure(VfsError::Cancelled, QStringLiteral("Walk cancelled"));

            if (entry.isHidden && !m_options.includeHidden)
                continue;

            ++m_visited;
            const Action action = visit(entry, current.depth);
            if (action == Action::Stop) {
                m_stoppedEarly = true;
                return {};
            }

            if (!entry.isDir || action == Action::SkipSubtree)
                continue;
            // Reported by the visitor above and not entered: what a link points
            // at is somewhere else's business, and a link into a parent is a
            // walk that never ends. See ADR-0092.
            if (entry.isSymlink)
                continue;
            if (m_options.maxDepth >= 0 && current.depth >= m_options.maxDepth)
                continue;

            pending.append({ entry.uri, current.depth + 1 });
        }

        // Reached only by the loop running out: a cancel and a Stop both return
        // from inside it, so getting here means the whole listing was seen.
        if (directoryDone)
            directoryDone(current.uri, listing.value());
    }

    return {};
}

} // namespace mole
