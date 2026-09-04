#include "core/tasks/QueryFileActionsTask.h"

namespace mole {

QueryFileActionsTask::QueryFileActionsTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent)
    : Task(QStringLiteral("Asking about %1").arg(target.fileName()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_target(std::move(target))
{
    noteRunsOn(m_fileSystem);
    setOneOfMany(true);
    setBackground(true);
}

void QueryFileActionsTask::run()
{
    if (!m_fileSystem)
        return;

    const Result<FileEntry> entry = m_fileSystem->stat(m_target);
    if (!entry.ok()) {
        // Not a failure anybody should be told about. The row is gone, or the
        // drive is not answering, and either way the listing in front of the
        // user is what says so.
        return;
    }

    m_actions = m_fileSystem->actionsFor(m_target, entry.value());
    setProgress(100);
}

} // namespace mole
