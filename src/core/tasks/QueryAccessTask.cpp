#include "core/tasks/QueryAccessTask.h"

namespace mole {

QueryAccessTask::QueryAccessTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent)
    : Task(QStringLiteral("Check access to %1").arg(target.fileName()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_target(std::move(target))
{
    setBackground(true);
}

void QueryAccessTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("Nothing is mounted here")));
        return;
    }

    // Not asked at all when the drive has not advertised it: a round trip that
    // can only end in NotSupported is a round trip worth skipping.
    if (!m_fileSystem->capabilities().testFlag(VfsCapability::ReportsAccess)) {
        setStatusText(QStringLiteral("This drive does not report access"));
        return;
    }

    Result<AccessInfo> result = m_fileSystem->access(m_target);
    if (!result.ok()) {
        // Plenty of drives legitimately cannot answer. Recording that as a
        // failed task would light up the interface over nothing.
        setStatusText(result.error().message);
        return;
    }

    emit accessReady(m_target, result.value());
    setStatusText(result.value().nativeText);
}

} // namespace mole
