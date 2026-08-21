#include "core/tasks/QueryFolderActionsTask.h"

namespace mole {

QueryFolderActionsTask::QueryFolderActionsTask(FileSystemPtr fileSystem, VfsUri directory, QObject* parent)
    : Task(QStringLiteral("Asking about the files in %1").arg(directory.fileName()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_directory(std::move(directory))
{
    setOneOfMany(true);
    setBackground(true);
}

void QueryFolderActionsTask::run()
{
    if (!m_fileSystem)
        return;

    const Result<QStringList> named = m_fileSystem->entriesWithActions(m_directory, cancelToken());
    if (!named.ok()) {
        // Never anybody's error. The listing is what somebody asked for and they
        // have it; a mark that does not appear is a mark that does not appear.
        return;
    }
    m_names = named.value();
    setProgress(100);
}

} // namespace mole
