#include "core/tasks/StatTask.h"

namespace mole {

StatTask::StatTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent)
    : Task(QStringLiteral("Read %1").arg(target.fileName()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_target(std::move(target))
{
    noteRunsOn(m_fileSystem);
    setBackground(true);
}

void StatTask::run()
{
    if (!m_fileSystem) {
        emit entryReady(m_target, false, FileEntry {});
        setStatusText(QStringLiteral("Nothing is mounted here"));
        return;
    }

    const Result<FileEntry> found = m_fileSystem->stat(m_target);
    if (!found.ok()) {
        // Not a failed task. A file that is not there, or a drive that will not
        // say, is an answer -- and the caller has a name and a uri of its own to
        // carry on with.
        emit entryReady(m_target, false, FileEntry {});
        setStatusText(found.error().message);
        return;
    }

    emit entryReady(m_target, true, found.value());
    setStatusText(found.value().name);
}

} // namespace mole
