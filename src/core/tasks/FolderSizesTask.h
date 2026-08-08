#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <QList>

namespace mole {

/// Totals up what each of several folders contains, one folder at a time.
///
/// The question "which of these is the big one" does not need an analysis and
/// its report; it needs a number per folder, in the listing, without leaving it.
/// So this walks each folder with the same `DirectoryWalker` the analysis and the
/// indexer use -- one traversal implementation, with cancellation and unreadable
/// directories already solved -- and announces each total as it lands rather than
/// at the end, because the first answer is useful before the last one arrives.
class FolderSizesTask final : public Task
{
    Q_OBJECT

public:
    FolderSizesTask(FileSystemPtr fileSystem, QList<VfsUri> folders, QObject* parent = nullptr);

signals:
    /// One per folder, delivered on the receiver's thread as the walk finishes
    /// it. `bytes` counts files only; directory entries have a size of their own
    /// on most backends and adding it would be counting the shelves as stock.
    void folderSized(const mole::VfsUri& folder, qint64 bytes, qint64 fileCount);

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    QList<VfsUri> m_folders;
};

} // namespace mole
