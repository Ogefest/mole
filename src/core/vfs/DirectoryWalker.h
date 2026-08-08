#pragma once

#include "core/vfs/IFileSystem.h"

#include <functional>

namespace mole {

/// Depth-first traversal over any backend. Shared by the indexer, the live
/// search and (later) duplicate detection, so the awkward parts -- cancellation,
/// unreadable directories, symlink loops -- are solved once.
class DirectoryWalker
{
public:
    struct Options
    {
        bool followSymlinks = false;
        bool includeHidden = true;
        /// -1 means unlimited. 0 lists only the root's direct children.
        int maxDepth = -1;
    };

    /// What the walker should do after the visitor has seen an entry.
    enum class Action {
        Continue, ///< keep going, descending into directories
        SkipSubtree, ///< do not descend into this directory, keep walking
        Stop ///< end the whole traversal successfully (e.g. result cap reached)
    };

    /// Called for every entry found, in no guaranteed order.
    using Visitor = std::function<Action(const FileEntry& entry, int depth)>;

    // Two overloads rather than a defaulted argument: Options carries default
    // member initializers, and those are not usable in a default argument of
    // the class that encloses them.
    explicit DirectoryWalker(FileSystemPtr fileSystem);
    DirectoryWalker(FileSystemPtr fileSystem, Options options);

    /// Walks `root`. Directories that cannot be read are recorded in errors()
    /// and skipped rather than aborting the whole traversal -- one permission
    /// denied deep in /var must not kill a 300k-file scan.
    Result<void> walk(const VfsUri& root, const CancelToken& cancel, const Visitor& visit);

    qint64 visitedCount() const { return m_visited; }
    const QList<VfsError>& errors() const { return m_errors; }
    /// True when the visitor asked to Stop rather than the tree running out.
    bool stoppedEarly() const { return m_stoppedEarly; }

private:
    FileSystemPtr m_fileSystem;
    Options m_options;
    qint64 m_visited = 0;
    bool m_stoppedEarly = false;
    QList<VfsError> m_errors;
};

} // namespace mole
