#include "core/tasks/MakeDirectoryTask.h"

namespace mole {

MakeDirectoryTask::MakeDirectoryTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent)
    : Task(QStringLiteral("New folder %1").arg(target.fileName()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_target(std::move(target))
{
    noteRunsOn(m_fileSystem);
    noteTouching(m_target);
}

void MakeDirectoryTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("Nothing is mounted here")));
        return;
    }

    const Result<void> made = m_fileSystem->makeDirectory(m_target);
    if (!made.ok()) {
        // Reported through the signal as well as by failing, because the window
        // says so where the name was typed rather than only in the task strip.
        m_failure = made.error().message;
        emit refused(m_failure);
        fail(made.error());
        return;
    }

    emit created(m_target);
    setStatusText(m_target.fileName());
}

} // namespace mole
