#include "core/tasks/ReadFileTask.h"

namespace mole {

ReadFileTask::ReadFileTask(FileSystemPtr fileSystem, VfsUri target, qint64 maxBytes, QObject* parent)
    : Task(QStringLiteral("Read %1").arg(target.fileName()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_target(std::move(target))
    , m_maxBytes(maxBytes)
{
}

void ReadFileTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(
            VfsError::NotFound, QStringLiteral("Nothing is mounted for %1").arg(m_target.toString())));
        return;
    }

    Result<std::unique_ptr<QIODevice>> device = m_fileSystem->openRead(m_target);
    if (!device.ok()) {
        fail(device.error());
        return;
    }

    QIODevice* stream = device.value().get();
    if (m_maxBytes < 0) {
        m_contents = stream->readAll();
    } else {
        m_contents = stream->read(m_maxBytes);
        // One extra byte tells us whether there was more, without reading it all.
        m_truncated = !stream->atEnd();
    }
    stream->close();

    setProgress(100);
    setStatusText(QStringLiteral("%1 bytes").arg(m_contents.size()));
}

} // namespace mole
