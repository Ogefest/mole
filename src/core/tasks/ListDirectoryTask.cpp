#include "core/tasks/ListDirectoryTask.h"

namespace mole {

ListDirectoryTask::ListDirectoryTask(FileSystemPtr fileSystem, VfsUri directory, QObject* parent)
    : Task(QStringLiteral("List %1").arg(directory.toString()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_directory(std::move(directory))
{
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
