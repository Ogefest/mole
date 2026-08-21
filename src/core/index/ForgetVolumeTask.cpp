#include "core/index/ForgetVolumeTask.h"

namespace mole {

ForgetVolumeTask::ForgetVolumeTask(IndexDatabase* index, qint64 volumeId, QString label, QObject* parent)
    : Task(QStringLiteral("Forget the index of %1").arg(label), parent)
    , m_index(index)
    , m_volumeId(volumeId)
{
}

void ForgetVolumeTask::run()
{
    if (!m_index || !m_index->isOpen()) {
        const QString reason = QStringLiteral("The index is not open");
        emit removed(false, reason);
        fail(VfsError::make(VfsError::IoError, reason));
        return;
    }

    const Result<void> gone = m_index->removeVolume(m_volumeId);
    if (!gone.ok()) {
        emit removed(false, gone.error().message);
        fail(gone.error());
        return;
    }

    setStatusText(QStringLiteral("Forgotten"));
    emit removed(true, QString());
}

} // namespace mole
