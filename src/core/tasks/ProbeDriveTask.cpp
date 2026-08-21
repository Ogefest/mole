#include "core/tasks/ProbeDriveTask.h"

namespace mole {

ProbeDriveTask::ProbeDriveTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent)
    : Task(QStringLiteral("Asking what %1 can do").arg(target.toString()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_target(std::move(target))
{
    // One of a crowd, and nobody's business: this is submitted on every
    // navigation and does nothing at all on all but the first per drive.
    setOneOfMany(true);
    setBackground(true);
}

DriveOffers ProbeDriveTask::offers() const
{
    return m_fileSystem ? m_fileSystem->offers() : DriveOffers();
}

void ProbeDriveTask::run()
{
    if (!m_fileSystem)
        return;

    // Never fails. A drive that cannot say what it offers is a drive that works
    // exactly as it did before anybody asked, and the person who opened the
    // folder asked for a listing -- they get one either way. The drive records
    // that the asking failed, and the log carries the reason.
    m_fileSystem->probe(m_target, cancelToken());
    setProgress(100);
}

} // namespace mole
