#pragma once

#include "core/vfs/IFileSystem.h"

#include <functional>

namespace mole {

/// Depth-first traversal over any backend. Shared by the indexer, the live
/// search and duplicate detection, so the awkward parts -- cancellation,
/// unreadable directories, links -- are solved once.
///
/// **A link is reported and never descended into.** Not an option: it is the
/// rule for the whole application, written down in ADR-0092, and following one
/// is what turns a directory loop into a walk that ends when the kernel refuses
/// a path. There used to be a followSymlinks option here, with a seen-set that
/// was keyed on the uri -- so every pass through a loop produced a new uri and
/// the guard caught nothing. No caller ever set it, which is the only reason
/// that never cost anybody a scan.
class DirectoryWalker
{
public:
    struct Options
    {
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

    /// Called once every entry of `dir` has been through the visitor, with the
    /// whole listing -- including what the visitor declined.
    ///
    /// A caller holding an older record of a directory needs to know what is
    /// in it now, not what matched: the difference between the two is what has
    /// been deleted since. Not called for a directory the walk could not read,
    /// or for one it stopped in the middle of, because neither is a directory
    /// anybody has seen the whole of.
    using DirectoryVisitor = std::function<void(const VfsUri& dir, const FileEntryList& entries)>;

    // Two overloads rather than a defaulted argument: Options carries default
    // member initializers, and those are not usable in a default argument of
    // the class that encloses them.
    explicit DirectoryWalker(FileSystemPtr fileSystem);
    DirectoryWalker(FileSystemPtr fileSystem, Options options);

    /// Walks `root`. Directories that cannot be read are recorded in errors()
    /// and skipped rather than aborting the whole traversal -- one permission
    /// denied deep in /var must not kill a 300k-file scan.
    Result<void> walk(const VfsUri& root, const CancelToken& cancel, const Visitor& visit);
    /// The same, telling `directoryDone` when each directory has been seen whole.
    Result<void> walk(const VfsUri& root, const CancelToken& cancel, const Visitor& visit,
        const DirectoryVisitor& directoryDone);

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
