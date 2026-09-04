#include "core/tasks/QuerySpaceTask.h"

namespace mole {

QuerySpaceTask::QuerySpaceTask(FileSystemPtr fileSystem, VfsUri root, QString mountId, QObject* parent)
    : Task(QStringLiteral("Check free space on %1").arg(root.toString()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_root(std::move(root))
    , m_mountId(std::move(mountId))
{
    noteRunsOn(m_fileSystem);
    setBackground(true);
}

void QuerySpaceTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("Nothing is mounted here")));
        return;
    }

    // A backend that does not advertise it is not asked: skipping the call is
    // cheaper than a round trip that can only end in NotSupported.
    if (!m_fileSystem->capabilities().testFlag(VfsCapability::ReportsSpace)) {
        setStatusText(QStringLiteral("This drive does not report a capacity"));
        return;
    }

    Result<SpaceInfo> result = m_fileSystem->space(m_root);
    if (!result.ok()) {
        // Not a failure of the task: plenty of drives legitimately have no
        // size. Recording it as failed would light up the interface for
        // something entirely normal.
        setStatusText(result.error().message);
        return;
    }

    m_info = result.value();
    setStatusText(QStringLiteral("%1 of %2 used")
                      .arg(QLocale().formattedDataSize(m_info.usedBytes()),
                          QLocale().formattedDataSize(m_info.totalBytes)));
    emit spaceReady(m_mountId, m_info);
}

} // namespace mole
