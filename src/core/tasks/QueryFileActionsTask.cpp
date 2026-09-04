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

QueryFileActionsTask::QueryFileActionsTask(
    FileSystemPtr fileSystem, VfsUri target, FileEntry known, QObject* parent)
    : QueryFileActionsTask(std::move(fileSystem), std::move(target), parent)
{
    m_known = std::move(known);
    m_haveEntry = true;
}

void QueryFileActionsTask::run()
{
    if (!m_fileSystem)
        return;

    // Handed the row, so nothing to ask. This is the whole of the difference
    // between a cursor step that costs a request and one that costs nothing.
    if (m_haveEntry) {
        m_actions = m_fileSystem->actionsFor(m_target, m_known);
        setProgress(100);
        return;
    }

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
