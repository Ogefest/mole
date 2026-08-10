#pragma once

#include "core/vfs/IFileSystem.h"
#include "core/vfs/VfsTypes.h"

#include <QIODevice>
#include <QTemporaryFile>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace mole::net {

/// The suffix an upload wears while it is still being written.
///
/// A process killed outright cannot tidy up after itself: it does not get to
/// delete what it had written, and whatever is on the server when it dies stays
/// there. So the protection cannot be an action taken afterwards -- it has to be
/// the name the bytes were going under all along. `report.pdf.mole-partial` is
/// visibly not `report.pdf`; nothing opens it by mistake, nothing syncs it as
/// the real thing, and finding leftovers is reading a listing rather than
/// consulting a bookkeeping file that the same kill would have truncated.
///
/// See ADR-0020.
inline constexpr QLatin1String kPartialUploadSuffix(".mole-partial");

/// Where an upload to `target` is written before it is finished.
VfsUri partialUploadOf(const VfsUri& target);

/// Whether this is the wreckage of an upload rather than a file somebody meant
/// to have.
bool isPartialUpload(const QString& name);

/// Puts a finished upload under the name it was asked for.
///
/// Expressed against IFileSystem because all three backends that need it do the
/// same two things -- check the destination is still free, then rename -- and
/// the difference between SFTP, FTP and WebDAV is entirely in how those two
/// calls are carried out.
///
/// A failure here is the upload's failure, and leaves nothing behind: bytes
/// under a name nothing will ever open are litter, not a result.
VfsError commitUpload(IFileSystem& fs, const VfsUri& staging, const VfsUri& target);

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

    /// Runs once the payload has arrived intact, to make it visible under the
    /// name it was asked for. Its failure is the stream's failure: a commit that
    /// did not happen is a file that is not there, whatever the sink managed.
    using Commit = std::function<VfsError()>;

    explicit BufferedUpload(Sink sink, Commit commit = {});
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
    Commit m_commit;
    QTemporaryFile m_scratch;
    VfsError m_error;
    bool m_committed = false;
};

/// A write stream that sends as the caller writes, instead of collecting the
/// whole payload first.
///
/// The mirror image of StreamingDownload and for the same reason: staging a
/// hundred-gigabyte file locally before sending any of it needs a hundred
/// gigabytes that a copy between two drives should never have had to touch.
/// Combined with a streamed read it is what makes a copy from one remote drive
/// to another cost a few megabytes of memory rather than twice the file.
///
/// Only for protocols that can send without knowing the length up front, which
/// is why S3 and WebDAV keep BufferedUpload: a signature needs the length, and
/// chunked PUT is unreliable across WebDAV servers.
///
/// Sending in spans, each appending to what the last one left, keeps any single
/// transfer clear of the fault described in SftpFileSystem. The price is that an
/// upload which fails part way leaves part of a file on the server, where a
/// staged one left nothing -- so the backend deletes what it managed to write.
class StreamingUpload final : public QIODevice, public ICommitsOnClose
{
public:
    /// Sends what `source` hands over -- no more than `span` bytes -- appending
    /// to the file rather than replacing it when `append` is set. Runs on the
    /// stream's own thread and must poll `cancel`.
    using Send
        = std::function<VfsError(QIODevice& source, qint64 span, bool append, const CancelToken& cancel)>;

    /// Runs once every span has gone up and nothing has gone wrong, to make the
    /// file visible under the name it was asked for. Its failure is the
    /// stream's failure, and it does not run at all for a stream that was
    /// cancelled or abandoned -- there is nothing finished to put in place.
    using Commit = std::function<VfsError()>;

    StreamingUpload(Send send, qint64 spanBytes, Commit commit = {});
    ~StreamingUpload() override;

    bool open(OpenMode mode) override;
    void close() override;
    bool isSequential() const override { return true; }

    /// Meaningful only after close(), as for any stream that commits there.
    VfsError commitError() const override { return m_error; }

protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 size) override;

private:
    class Source;
    friend class Source;

    void startSending();
    void finish();
    /// Called from the sending thread. Blocks until there is something to send,
    /// and returns 0 when this span has had its fill or the writer has closed.
    qint64 take(char* data, qint64 maxSize);

    Send m_send;
    qint64 m_spanBytes = 0;
    Commit m_commit;

    std::thread m_thread;
    CancelToken m_cancel;

    mutable std::mutex m_mutex;
    std::condition_variable m_filled;
    std::condition_variable m_drained;
    std::deque<QByteArray> m_chunks;
    qint64 m_buffered = 0;
    bool m_writerClosed = false;
    bool m_failed = false;
    VfsError m_error;
};

/// Opens a local, self-deleting copy of a downloaded payload for reading.
///
/// Used for files small enough that fetching the whole thing up front is free,
/// where it buys random access for nothing. Anything larger is streamed --
/// see StreamingDownload, and the size at which each backend switches over.
Result<std::unique_ptr<QIODevice>> openDownloadedFile(std::unique_ptr<QTemporaryFile> file);

/// A read stream that fetches from the server while the caller reads, instead
/// of downloading the whole file first.
///
/// The reason it exists is arithmetic: a backup file is a hundred gigabytes, and
/// downloading it to a temporary file before the copy can write its first byte
/// needs a hundred gigabytes of local scratch space that the machine has not
/// got -- for a copy whose destination has plenty of room. Streaming needs a few
/// megabytes whatever the file weighs. It also means a preview of a huge file
/// reads the first page and stops, rather than fetching the lot to look at the
/// beginning.
///
/// The transfer runs on a thread of its own and fills a bounded buffer; reads
/// take from it and block while it is empty. That inversion is what libcurl's
/// blocking interface forces: it pushes bytes at a write callback at its own
/// pace, and a QIODevice is asked for them at the caller's. The buffer between
/// them is what lets each go at its own speed, and its size is the only memory
/// the whole arrangement costs.
///
/// Fetching is by span so that no single transfer runs long enough to meet the
/// fault described in SftpFileSystem; the stream asks for the next one as the
/// last runs out, and the caller sees one continuous file.
class StreamingDownload final : public QIODevice
{
public:
    /// Fetches up to `span` bytes from `offset` into `sink`, which is written to
    /// as the bytes arrive. Runs on the stream's own thread, and must poll
    /// `cancel` -- a caller that gives up must not wait for a whole span.
    using Fetch
        = std::function<VfsError(QIODevice& sink, qint64 offset, qint64 span, const CancelToken& cancel)>;

    /// `size` is the length of the file, which the stream must know: a device
    /// that cannot say where it ends cannot be read by anything that asks.
    StreamingDownload(Fetch fetch, qint64 size, qint64 spanBytes);
    ~StreamingDownload() override;

    bool open(OpenMode mode) override;
    void close() override;

    bool isSequential() const override { return false; }
    qint64 size() const override { return m_size; }
    /// Backwards, or forwards past what is buffered, throws away the transfer in
    /// flight and starts another at the new position. Correct, and expensive
    /// enough that nothing should seek around a stream by choice.
    bool seek(qint64 position) override;

    /// What went wrong, once a read has returned -1.
    VfsError error() const;

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    class Sink;
    friend class Sink;

    void startFetching(qint64 offset);
    void stopFetching();
    /// Called from the fetch thread. False means the reader has gone away.
    bool deliver(const char* data, qint64 size);

    Fetch m_fetch;
    qint64 m_size = 0;
    qint64 m_spanBytes = 0;

    std::thread m_thread;
    CancelToken m_cancel;

    mutable std::mutex m_mutex;
    std::condition_variable m_filled;
    std::condition_variable m_drained;
    std::deque<QByteArray> m_chunks;
    qint64 m_buffered = 0;
    /// Where the buffered bytes start, so a short seek can be answered from
    /// what has already arrived instead of starting a new transfer.
    qint64 m_bufferedFrom = 0;
    bool m_running = false;
    bool m_finished = false;
    VfsError m_error;
};

} // namespace mole::net
