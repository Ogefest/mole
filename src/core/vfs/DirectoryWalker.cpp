#include "core/vfs/DirectoryWalker.h"

#include <QSet>

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

    QList<Pending> stack { { root, 0 } };
    // Guards against symlink cycles when the caller opts into following them.
    QSet<QString> seen;

    while (!stack.isEmpty()) {
        if (cancel.isCancelled())
            return Result<void>::failure(VfsError::Cancelled, QStringLiteral("Walk cancelled"));

        const Pending current = stack.takeLast();

        if (m_options.followSymlinks) {
            const QString key = current.uri.toString();
            if (seen.contains(key))
                continue;
            seen.insert(key);
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
            if (entry.isSymlink && !m_options.followSymlinks)
                continue;
            if (m_options.maxDepth >= 0 && current.depth >= m_options.maxDepth)
                continue;

            stack.append({ entry.uri, current.depth + 1 });
        }
    }

    return {};
}

} // namespace mole
