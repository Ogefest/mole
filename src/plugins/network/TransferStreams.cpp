#include "plugins/network/TransferStreams.h"

#include "plugins/network/CurlTransport.h"

#include "core/platform/Staging.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cstring>

namespace mole::net {

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
    // Refusing when there is nowhere to stage is the whole point of this device:
    // the bytes are acknowledged to the caller before they reach the server, so a
    // stage that cannot be trusted has to fail at open() rather than at send. Qt
    // does not refuse a staging directory that is not there -- it puts the file in
    // the filesystem root, which succeeds for anybody who can write there -- so
    // the question goes through staging::openFile(), which asks first and creates
    // the file where it said it would. See MOLE-297 and MOLE-304.
    QString why;
    if (!staging::openFile(m_scratch, &why)) {
        setErrorString(QStringLiteral("could not stage the upload: %1").arg(why));
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
    m_drained.wait(lock, [this] { return m_failed || m_buffered < kStreamBufferBytes; });
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

StreamingDownload::StreamingDownload(Fetch fetch, qint64 size, qint64 spanBytes, std::chrono::seconds budget)
    : m_fetch(std::move(fetch))
    , m_size(size)
    , m_spanBytes(spanBytes)
    , m_budgetMs(budget.count() > 0 ? budget.count() * 1000 : 0)
{
}

VfsError fileChangedWhileBeingRead()
{
    return VfsError::make(VfsError::IoError, QStringLiteral("the file changed while it was being read"));
}

QString identityOf(const FileEntry& entry)
{
    if (entry.size == kUnknownSize && !entry.modified.isValid())
        return {};
    return QStringLiteral("%1:%2")
        .arg(entry.size)
        .arg(entry.modified.isValid() ? entry.modified.toMSecsSinceEpoch() : -1);
}

void StreamingDownload::checkBeforeEverySpan(StillTheSameFile check)
{
    m_stillTheSameFile = std::move(check);
}

void StreamingDownload::pauseBeforeRetrying(int attempt)
{
    // A quarter of a second, doubling, capped at two. Capped low on purpose:
    // the budget is what ends the read, so time spent waiting between attempts
    // is time the link might have come back in and nobody was looking.
    constexpr int kFirstPauseMs = 250;
    constexpr int kLongestPauseMs = 2000;
    const int pause = std::min(kLongestPauseMs, kFirstPauseMs << std::min(attempt - 1, 8));

    // On the condition rather than a sleep: a reader that lets go must not wait
    // out a pause it has no interest in.
    std::unique_lock<std::mutex> lock(m_mutex);
    m_drained.wait_for(
        lock, std::chrono::milliseconds(pause), [this] { return !m_running || m_cancel.isCancelled(); });
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
    // A stream that is opened again is a read that begins again, so the next
    // span is the first one and there is nothing yet to check it against.
    m_spanHasFinished = false;
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
        // The one clock that decides anything: how long since a byte arrived.
        // Not how patient a connection is, which is the connection's business
        // and is set far shorter -- see the socket option in CurlTransport.
        QElapsedTimer sinceProgress;
        sinceProgress.start();
        int attempt = 0;

        while (!m_cancel.isCancelled() && at < m_size) {
            // Before a byte of this span is asked for, and never for the first
            // one -- that is the span the validator was taken from. A file that
            // has been replaced must not contribute anything at all to what the
            // reader sees, so this is asked here rather than worked out
            // afterwards from what arrived. See MOLE-348.
            if (m_spanHasFinished && m_stillTheSameFile) {
                const VfsError changed = m_stillTheSameFile();
                if (changed.isError()) {
                    if (m_cancel.isCancelled())
                        break;
                    // Not retried, whatever the budget has left. Fetching the
                    // span again would fetch it from the file that replaced the
                    // one being read, which is not the file anybody asked for.
                    const std::lock_guard<std::mutex> guard(m_mutex);
                    m_error = changed;
                    break;
                }
            }

            const qint64 span = std::min(m_spanBytes, m_size - at);
            Sink sink(*this);
            const VfsError failed = m_fetch(sink, at, span, m_cancel);
            const qint64 delivered = sink.written();
            at += delivered;

            if (delivered > 0) {
                sinceProgress.restart();
                attempt = 0;
            }

            if (failed.isError()) {
                // A cancelled fetch is the reader letting go, not a fault.
                if (m_cancel.isCancelled())
                    break;

                // A span that stopped is fetched again from where it got to,
                // for as long as the transfer's budget is unspent.
                //
                // ADR-0013 sized spans so that no connection would carry enough
                // for the server's re-key to arrive, and rejected resuming
                // because it costs a stall-guard wait per stall. Its first
                // amendment resumed once, when a span had carried bytes. That
                // covered a server that re-keys and could not cover a link that
                // goes away and comes back: the single retry met the dead link,
                // delivered nothing, and failed a read the link was about to be
                // able to finish.
                //
                // So the bound is the budget rather than the number of tries.
                // While bytes are arriving the transfer is alive however many
                // connections it has been through; when none has arrived for
                // long enough, it is over whatever the connections say.
                //
                // **And only for a failure that could go the other way next
                // time.** The kind used to be unread, so a 403 whose credentials
                // had expired mid-copy, a 404 for an object deleted between
                // spans and a NotSupported each sat out the whole budget --
                // two minutes per file to say what the first attempt already
                // knew. See net::isWorthRetrying and MOLE-373.
                if (!net::isWorthRetrying(failed)) {
                    const std::lock_guard<std::mutex> guard(m_mutex);
                    m_error = failed;
                    break;
                }

                if (m_budgetMs > 0 && sinceProgress.elapsed() >= m_budgetMs) {
                    const std::lock_guard<std::mutex> guard(m_mutex);
                    m_error = failed;
                    break;
                }

                pauseBeforeRetrying(++attempt);
                continue;
            }

            m_spanHasFinished = true;

            // Short of what was asked for, with no error, is the end of the
            // file -- a server clamps a range that runs past it.
            if (delivered < span)
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
    m_drained.wait(lock, [this] { return !m_running || m_buffered < kStreamBufferBytes; });
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
        const bool worthSkipping
            = position > m_bufferedFrom && position - m_bufferedFrom <= kStreamBufferBytes;
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
