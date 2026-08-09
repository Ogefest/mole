#pragma once

#include "core/vfs/IFileSystem.h"
#include "core/vfs/VfsTypes.h"

#include <QIODevice>
#include <QTemporaryFile>

#include <functional>
#include <memory>

namespace mole::net {

/// A write stream that collects the whole payload locally and ships it when it
/// is closed.
///
/// Every protocol here wants it this way. S3 must know the length before it can
/// sign the request, WebDAV servers are unreliable about chunked PUT, and a
/// resumable upload is a feature rather than a stream. Buffering to a temporary
/// file rather than to memory is what keeps a ten-gigabyte copy from being a
/// ten-gigabyte allocation.
///
/// Because the send happens in close(), and QIODevice::close() cannot report a
/// failure, the outcome is left in commitError() and in errorString(). Callers
/// use mole::closeAndReport() rather than remembering to look.
class BufferedUpload final : public QIODevice, public ICommitsOnClose
{
public:
    /// Sends the collected bytes. Called exactly once, from close(). The device
    /// is positioned at the start and `size` is the byte count.
    using Sink = std::function<Result<void>(QIODevice& payload, qint64 size)>;

    explicit BufferedUpload(Sink sink);
    ~BufferedUpload() override;

    bool open(OpenMode mode) override;
    void close() override;
    bool isSequential() const override { return true; }

    /// Meaningful only after close(). ok() until something goes wrong.
    VfsError commitError() const override { return m_error; }

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 size) override;

private:
    Sink m_sink;
    QTemporaryFile m_scratch;
    VfsError m_error;
    bool m_committed = false;
};

/// Opens a local, self-deleting copy of a downloaded payload for reading.
///
/// Remote reads land in a temporary file rather than being streamed straight
/// through, which buys random access -- the preview layer seeks, and a
/// sequential-only device would have to refetch to answer that.
Result<std::unique_ptr<QIODevice>> openDownloadedFile(std::unique_ptr<QTemporaryFile> file);

} // namespace mole::net
