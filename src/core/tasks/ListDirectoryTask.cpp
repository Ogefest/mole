#include "core/tasks/ListDirectoryTask.h"

namespace mole {

ListDirectoryTask::ListDirectoryTask(FileSystemPtr fileSystem, VfsUri directory, QObject* parent)
    : Task(QStringLiteral("List %1").arg(directory.toString()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_directory(std::move(directory))
{
    noteRunsOn(m_fileSystem);
    // One of a crowd: the browser lists a folder on every navigation and cancels it on
    // every keystroke that narrows a filter. See Task::isOneOfMany() and ADR-0064.
    setOneOfMany(true);
}

void ListDirectoryTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(
            VfsError::NotFound, QStringLiteral("Nothing is mounted for %1").arg(m_directory.toString())));
        return;
    }

    Result<FileEntryList> listing = m_fileSystem->list(m_directory, cancelToken());
    if (!listing.ok()) {
        fail(listing.error());
        return;
    }

    // Auto-connection from this worker thread to a UI-thread receiver becomes
    // a queued delivery, so the payload is copied for us.
    emit listed(m_directory, listing.value());
    setProgress(100);
}

} // namespace mole
