#include "core/tasks/FolderSizesTask.h"

#include "core/vfs/DirectoryWalker.h"

#include <QLocale>

namespace mole {

FolderSizesTask::FolderSizesTask(FileSystemPtr fileSystem, QList<VfsUri> folders, QObject* parent)
    : Task(folders.size() == 1 ? QStringLiteral("Measure %1").arg(folders.first().fileName())
                               : QStringLiteral("Measure %1 folders").arg(folders.size()),
          parent)
    , m_fileSystem(std::move(fileSystem))
    , m_folders(std::move(folders))
{
    // Nobody is waiting on this to carry on working, so it belongs with the
    // background jobs rather than in front of the window.
    setBackground(true);
}

void FolderSizesTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("Nothing is mounted here")));
        return;
    }

    const QLocale locale;
    int done = 0;

    for (const VfsUri& folder : m_folders) {
        if (isCancelRequested())
            return;

        setStatusText(QStringLiteral("Measuring %1").arg(folder.fileName()));

        qint64 bytes = 0;
        qint64 files = 0;
        DirectoryWalker walker(m_fileSystem);
        walker.walk(folder, cancelToken(), [&](const FileEntry& entry, int) {
            if (!entry.isDir) {
                bytes += entry.size;
                ++files;
            }
            return DirectoryWalker::Action::Continue;
        });

        // Cancelled part way through a folder: the total would be wrong, and a
        // wrong number in a listing is worse than no number at all.
        if (isCancelRequested())
            return;

        // A folder the walk could not read completely still reports what it
        // managed. The walker records the errors and carries on, which is the
        // right trade here too: "at least this much" beats nothing at all, and
        // one unreadable subdirectory should not blank out the answer.
        emit folderSized(folder, bytes, files);

        ++done;
        setProgress(static_cast<int>(done * 100 / m_folders.size()));
    }

    setStatusText(m_folders.size() == 1
            ? QStringLiteral("Measured %1").arg(m_folders.first().fileName())
            : QStringLiteral("Measured %1 folders").arg(locale.toString(m_folders.size())));
}

} // namespace mole
