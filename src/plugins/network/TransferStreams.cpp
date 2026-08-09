#include "plugins/network/TransferStreams.h"

namespace mole::net {

BufferedUpload::BufferedUpload(Sink sink)
    : m_sink(std::move(sink))
{
}

BufferedUpload::~BufferedUpload()
{
    // A device destroyed without being closed has been abandoned -- a cancelled
    // or failed copy. Sending a truncated payload would be worse than sending
    // nothing, so the buffer is simply dropped.
    m_committed = true;
}

bool BufferedUpload::open(OpenMode mode)
{
    if (!(mode & QIODevice::WriteOnly)) {
        setErrorString(QStringLiteral("an upload stream can only be opened for writing"));
        return false;
    }
    if (!m_scratch.open()) {
        setErrorString(QStringLiteral("could not open a temporary file to stage the upload: %1")
                           .arg(m_scratch.errorString()));
        return false;
    }
    return QIODevice::open(mode | QIODevice::Unbuffered);
}

qint64 BufferedUpload::readData(char*, qint64)
{
    return -1;
}

qint64 BufferedUpload::writeData(const char* data, qint64 size)
{
    const qint64 written = m_scratch.write(data, size);
    if (written != size)
        setErrorString(QStringLiteral("could not stage the upload: %1").arg(m_scratch.errorString()));
    return written;
}

void BufferedUpload::close()
{
    if (m_committed) {
        QIODevice::close();
        return;
    }
    m_committed = true;

    if (!m_scratch.flush()) {
        m_error = VfsError::make(
            VfsError::IoError, QStringLiteral("could not stage the upload: %1").arg(m_scratch.errorString()));
        setErrorString(m_error.message);
        QIODevice::close();
        return;
    }

    const qint64 size = m_scratch.size();
    m_scratch.seek(0);

    const Result<void> sent = m_sink(m_scratch, size);
    if (!sent.ok()) {
        m_error = sent.error();
        setErrorString(m_error.message);
    }
    QIODevice::close();
}

Result<std::unique_ptr<QIODevice>> openDownloadedFile(std::unique_ptr<QTemporaryFile> file)
{
    if (!file->seek(0)) {
        return Result<std::unique_ptr<QIODevice>>::failure(
            VfsError::IoError, QStringLiteral("could not rewind the downloaded copy"));
    }
    return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(file.release()));
}

} // namespace mole::net
