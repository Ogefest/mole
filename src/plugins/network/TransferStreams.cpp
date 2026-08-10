#include "plugins/network/TransferStreams.h"

#include <algorithm>
#include <cstring>

namespace mole::net {

VfsUri partialUploadOf(const VfsUri& target)
{
    return target.parent().child(target.fileName() + kPartialUploadSuffix);
}

bool isPartialUpload(const QString& name)
{
    return name.endsWith(kPartialUploadSuffix);
}

VfsError commitUpload(IFileSystem& fs, const VfsUri& staging, const VfsUri& target)
{
    // A copy clears the way before it starts, so the destination ought to be
    // free. Ought is not is: something else may have put a file there in the
    // minutes this one spent going up, and a rename that quietly replaced it
    // would destroy data this transfer was never asked to touch. So the
    // emptiness is checked rather than assumed.
    const Result<FileEntry> occupied = fs.stat(target);
    if (occupied.ok()) {
        fs.remove(staging, false);
        return VfsError::make(VfsError::AlreadyExists,
            QStringLiteral("%1 appeared while it was being written, so the upload was not put in place")
                .arg(target.path()));
    }
    if (occupied.error().code != VfsError::NotFound) {
        // Not there is one answer; could not find out is another, and only the
        // first of them makes a rename safe. Guessing here is guessing about
        // whether somebody else's file is about to be replaced.
        fs.remove(staging, false);
        return occupied.error();
    }

    const Result<void> renamed = fs.rename(staging, target);
    if (!renamed.ok()) {
        fs.remove(staging, false);
        return renamed.error();
    }
    return VfsError::ok();
}

BufferedUpload::BufferedUpload(Sink sink, Commit commit)
    : m_sink(std::move(sink))
    , m_commit(std::move(commit))
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
        QIODevice::close();
        return;
    }

    if (m_commit) {
        m_error = m_commit();
        if (m_error.isError())
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

// ---- StreamingUpload -------------------------------------------------------

namespace {
    /// How much is allowed to sit between a transfer and the code at the other
    /// end of it. Large enough that neither waits on the other over a hiccup,
    /// small enough to be irrelevant next to the file being carried -- and it is
    /// the entire memory cost of a copy of any size.
    constexpr qint64 kBufferBytes = 8 * 1024 * 1024;
} // namespace

/// What the send reads from: it takes from the writer's buffer and stops at the
/// end of the span, so the next transfer can pick up where this one left off.
class StreamingUpload::Source final : public QIODevice
{
public:
    Source(StreamingUpload& owner, qint64 span)
        : m_owner(owner)
        , m_remaining(span)
    {
        QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    qint64 served() const { return m_served; }

protected:
    qint64 writeData(const char*, qint64) override { return -1; }
    qint64 readData(char* data, qint64 maxSize) override
    {
        if (m_remaining <= 0)
            return 0; // this span is full; the next transfer continues the file
        const qint64 taken = m_owner.take(data, std::min(maxSize, m_remaining));
        if (taken > 0) {
            m_remaining -= taken;
            m_served += taken;
        }
        return taken;
    }

private:
    StreamingUpload& m_owner;
    qint64 m_remaining = 0;
    qint64 m_served = 0;
};

StreamingUpload::StreamingUpload(Send send, qint64 spanBytes, Commit commit)
    : m_send(std::move(send))
    , m_spanBytes(spanBytes)
    , m_commit(std::move(commit))
{
}

StreamingUpload::~StreamingUpload()
{
    // Abandoned rather than closed means a cancelled or failed copy. Sending
    // what was collected would put a truncated file on the server and call it
    // finished, so the transfer is stopped instead.
    m_cancel.cancel();
    finish();
}

bool StreamingUpload::open(OpenMode mode)
{
    if (!(mode & QIODevice::WriteOnly)) {
        setErrorString(QStringLiteral("an upload stream can only be opened for writing"));
        return false;
    }
    return QIODevice::open(mode | QIODevice::Unbuffered);
}

qint64 StreamingUpload::writeData(const char* data, qint64 size)
{
    if (size <= 0)
        return 0;

    // The first write is what starts the transfer: a stream opened and never
    // written to should not touch the network.
    if (!m_thread.joinable())
        startSending();

    std::unique_lock<std::mutex> lock(m_mutex);
    m_drained.wait(lock, [this] { return m_failed || m_buffered < kBufferBytes; });
    if (m_failed) {
        setErrorString(m_error.message);
        return -1;
    }

    m_chunks.emplace_back(data, static_cast<int>(size));
    m_buffered += size;
    m_filled.notify_all();
    return size;
}

void StreamingUpload::close()
{
    if (isOpen()) {
        // An empty file still has to be created, and nothing has started yet
        // when nothing was written.
        if (!m_thread.joinable() && !m_cancel.isCancelled())
            startSending();
        finish();
    }
    QIODevice::close();
}

void StreamingUpload::startSending()
{
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_writerClosed = false;
        m_failed = false;
    }

    m_thread = std::thread([this] {
        bool append = false;
        for (;;) {
            Source source(*this, m_spanBytes);
            const VfsError failed = m_send(source, m_spanBytes, append, m_cancel);
            if (failed.isError()) {
                const std::lock_guard<std::mutex> guard(m_mutex);
                m_error = failed;
                m_failed = true;
                // Wakes a writer that is waiting for room it will never get.
                m_drained.notify_all();
                return;
            }
            // A span that took less than it was offered means the writer has
            // closed and the file is complete.
            if (source.served() < m_spanBytes)
                return;
            append = true;
        }
    });
}

qint64 StreamingUpload::take(char* data, qint64 maxSize)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_filled.wait(lock, [this] { return !m_chunks.empty() || m_writerClosed || m_cancel.isCancelled(); });

    if (m_cancel.isCancelled())
        return -1; // readFromDevice turns this into an aborted transfer
    if (m_chunks.empty())
        return 0; // the writer has closed: end of the file

    QByteArray& front = m_chunks.front();
    const qint64 taken = std::min<qint64>(maxSize, front.size());
    std::memcpy(data, front.constData(), static_cast<size_t>(taken));
    m_buffered -= taken;
    if (taken == front.size())
        m_chunks.pop_front();
    else
        front.remove(0, static_cast<int>(taken));

    m_drained.notify_all();
    return taken;
}

void StreamingUpload::finish()
{
    if (!m_thread.joinable())
        return;

    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_writerClosed = true;
        m_filled.notify_all();
    }
    m_thread.join();

    bool sent = false;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        if (m_failed && !m_error.isError())
            m_error = VfsError::make(VfsError::IoError, QStringLiteral("the upload did not finish"));
        sent = !m_failed && !m_error.isError();
    }

    // Only a transfer that arrived intact earns the name it was asked for. An
    // abandoned stream has cancelled itself before getting here, and a failed
    // one has nothing worth putting in place.
    if (sent && m_commit && !m_cancel.isCancelled())
        m_error = m_commit();

    if (m_error.isError())
        setErrorString(m_error.message);
}

// ---- StreamingDownload -----------------------------------------------------

/// What the fetch writes into: every block libcurl hands over goes to the
/// reader's buffer, and a full buffer blocks here until the reader catches up.
class StreamingDownload::Sink final : public QIODevice
{
public:
    explicit Sink(StreamingDownload& owner)
        : m_owner(owner)
    {
        QIODevice::open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    }

    qint64 written() const { return m_written; }

protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 size) override
    {
        // A refusal aborts the transfer, which is exactly what should happen
        // when whoever asked for the file has stopped reading it.
        if (!m_owner.deliver(data, size))
            return -1;
        m_written += size;
        return size;
    }

private:
    StreamingDownload& m_owner;
    qint64 m_written = 0;
};

StreamingDownload::StreamingDownload(Fetch fetch, qint64 size, qint64 spanBytes)
    : m_fetch(std::move(fetch))
    , m_size(size)
    , m_spanBytes(spanBytes)
{
}

StreamingDownload::~StreamingDownload()
{
    stopFetching();
}

bool StreamingDownload::open(OpenMode mode)
{
    if (!(mode & QIODevice::ReadOnly)) {
        setErrorString(QStringLiteral("a download stream can only be opened for reading"));
        return false;
    }
    // Unbuffered: QIODevice's own read buffer would ask for large blocks and
    // hold them, which only puts a second buffer in front of the one that is
    // already there for this purpose.
    return QIODevice::open(mode | QIODevice::Unbuffered);
}

void StreamingDownload::close()
{
    stopFetching();
    QIODevice::close();
}

VfsError StreamingDownload::error() const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return m_error;
}

void StreamingDownload::startFetching(qint64 offset)
{
    stopFetching();

    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_cancel = CancelToken();
        m_chunks.clear();
        m_buffered = 0;
        m_bufferedFrom = offset;
        m_finished = false;
        m_running = true;
        m_error = VfsError::ok();
    }

    m_thread = std::thread([this, offset] {
        qint64 at = offset;
        while (!m_cancel.isCancelled() && at < m_size) {
            const qint64 span = std::min(m_spanBytes, m_size - at);
            Sink sink(*this);
            const VfsError failed = m_fetch(sink, at, span, m_cancel);
            if (failed.isError()) {
                const std::lock_guard<std::mutex> guard(m_mutex);
                // A cancelled fetch is the reader letting go, not a fault.
                if (!m_cancel.isCancelled())
                    m_error = failed;
                break;
            }
            at += sink.written();
            // Short of what was asked for, with no error, is the end of the
            // file -- a server clamps a range that runs past it.
            if (sink.written() < span)
                break;
        }

        const std::lock_guard<std::mutex> guard(m_mutex);
        m_finished = true;
        m_filled.notify_all();
    });
}

void StreamingDownload::stopFetching()
{
    if (!m_thread.joinable()) {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_running = false;
        return;
    }

    // Both halves are needed: the token stops the transfer, and waking the
    // writer is what lets it notice while it is waiting for room.
    m_cancel.cancel();
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_running = false;
        m_drained.notify_all();
    }
    m_thread.join();

    const std::lock_guard<std::mutex> guard(m_mutex);
    m_chunks.clear();
    m_buffered = 0;
}

bool StreamingDownload::deliver(const char* data, qint64 size)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_drained.wait(lock, [this] { return !m_running || m_buffered < kBufferBytes; });
    if (!m_running)
        return false;

    m_chunks.emplace_back(data, static_cast<int>(size));
    m_buffered += size;
    m_filled.notify_all();
    return true;
}

qint64 StreamingDownload::readData(char* data, qint64 maxSize)
{
    if (maxSize <= 0)
        return 0;
    if (pos() >= m_size)
        return 0;

    // Started on the first read rather than on open(), so a device nobody reads
    // never touches the network.
    if (!m_thread.joinable() && !m_finished)
        startFetching(pos());

    std::unique_lock<std::mutex> lock(m_mutex);

    // Filled completely rather than with whatever happens to have arrived. A
    // short read is legal and every caller here would have to be written to
    // expect one; a stream that behaves like a file is a stream that can be
    // dropped in where a file used to be.
    qint64 taken = 0;
    while (taken < maxSize) {
        m_filled.wait(lock, [this] { return !m_chunks.empty() || m_finished; });

        if (m_chunks.empty()) {
            if (taken > 0)
                break; // hand over what arrived; the next read reports the end
            if (m_error.isError()) {
                setErrorString(m_error.message);
                return -1;
            }
            // Finished with nothing left, short of the length the file was said
            // to have: the transfer stopped early, and calling that the end of
            // the file is how a truncated copy gets reported as a whole one.
            if (pos() < m_size) {
                m_error = VfsError::make(VfsError::IoError,
                    QStringLiteral("the transfer stopped after %1 of %2 bytes").arg(pos()).arg(m_size));
                setErrorString(m_error.message);
                return -1;
            }
            return 0;
        }

        QByteArray& front = m_chunks.front();
        const qint64 wanted = std::min<qint64>(maxSize - taken, front.size());
        std::memcpy(data + taken, front.constData(), static_cast<size_t>(wanted));
        taken += wanted;
        m_buffered -= wanted;
        m_bufferedFrom += wanted;
        if (wanted == front.size())
            m_chunks.pop_front();
        else
            front.remove(0, static_cast<int>(wanted));

        m_drained.notify_all();
    }

    return taken;
}

bool StreamingDownload::seek(qint64 position)
{
    if (position < 0 || position > m_size)
        return false;
    if (!QIODevice::seek(position))
        return false;

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (position == m_bufferedFrom)
            return true;

        // Forwards, and not far: skip over those bytes rather than starting
        // another transfer. They are on their way whether they are wanted or
        // not, and waiting for them costs a fraction of a handshake. Waiting
        // rather than checking what has already arrived is deliberate -- whether
        // it has is a matter of timing, and a device whose behaviour depends on
        // how fast the network was is a device nothing can be tested against.
        const bool worthSkipping = position > m_bufferedFrom && position - m_bufferedFrom <= kBufferBytes;
        if (worthSkipping && m_thread.joinable()) {
            while (m_bufferedFrom < position) {
                if (m_chunks.empty()) {
                    if (m_finished || m_error.isError())
                        break;
                    m_filled.wait(lock, [this] { return !m_chunks.empty() || m_finished; });
                    continue;
                }

                QByteArray& front = m_chunks.front();
                const qint64 skip = std::min<qint64>(position - m_bufferedFrom, front.size());
                m_buffered -= skip;
                m_bufferedFrom += skip;
                if (skip == front.size())
                    m_chunks.pop_front();
                else
                    front.remove(0, static_cast<int>(skip));
            }
            m_drained.notify_all();
            if (m_bufferedFrom == position)
                return true;
        }
    }

    // Backwards, or far enough ahead that fetching the gap would cost more than
    // starting again.
    startFetching(position);
    return true;
}

} // namespace mole::net
