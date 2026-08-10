#include "plugins/network/TransferStreams.h"
#include "support/MoleTestMain.h"

#include <QTest>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace mole;

namespace {

/// A file that lives in memory, handed out a span at a time the way a server
/// hands out byte ranges.
class FakeServer
{
public:
    explicit FakeServer(QByteArray contents)
        : m_contents(std::move(contents))
    {
    }

    /// Written in blocks, because that is how libcurl delivers a transfer and
    /// the whole point of the stream is what happens between the blocks.
    net::StreamingDownload::Fetch fetch(int blockSize = 8192)
    {
        return [this, blockSize](QIODevice& sink, qint64 offset, qint64 span, const CancelToken& cancel) {
            ++m_spans;
            const qint64 available = std::min(span, static_cast<qint64>(m_contents.size()) - offset);
            for (qint64 sent = 0; sent < available; sent += blockSize) {
                if (cancel.isCancelled())
                    return VfsError::make(VfsError::Cancelled, QStringLiteral("cancelled"));
                if (m_failAfter >= 0 && m_delivered >= m_failAfter)
                    return VfsError::make(VfsError::NetworkError, QStringLiteral("the server hung up"));

                const qint64 size = std::min<qint64>(blockSize, available - sent);
                if (sink.write(m_contents.constData() + offset + sent, size) != size)
                    return VfsError::make(VfsError::IoError, QStringLiteral("the reader went away"));
                m_delivered += size;
            }
            return VfsError::ok();
        };
    }

    /// Makes the transfer fail once this many bytes have been handed over.
    void failAfter(qint64 bytes) { m_failAfter = bytes; }
    int spans() const { return m_spans; }

private:
    QByteArray m_contents;
    std::atomic<qint64> m_delivered { 0 };
    std::atomic<int> m_spans { 0 };
    qint64 m_failAfter = -1;
};

/// A server whose connection stops carrying bytes once it has carried enough of
/// them.
///
/// This is the SFTP re-key fault in miniature. A long transfer runs at full
/// speed and then simply ceases -- the connection open, the server there, and
/// nothing arriving until the stall guard gives up two minutes later -- because
/// the SSH session re-keys and that pairing of client and server does not
/// survive it. The limit is per connection rather than per file, which is the
/// whole reason the span loop works: a fresh connection carries just as much
/// again, and a file of any size is a series of connections none of which lives
/// long enough to meet the fault.
class ServerThatDiesPastALimit
{
public:
    ServerThatDiesPastALimit(QByteArray contents, qint64 carriesAtMost)
        : m_contents(std::move(contents))
        , m_limit(carriesAtMost)
    {
    }

    net::StreamingDownload::Fetch fetch(int blockSize = 8192)
    {
        return [this, blockSize](QIODevice& sink, qint64 offset, qint64 span, const CancelToken& cancel) {
            {
                const std::lock_guard<std::mutex> guard(m_mutex);
                m_asked.append(Span { offset, span });
            }

            const qint64 available = std::min(span, static_cast<qint64>(m_contents.size()) - offset);
            for (qint64 sent = 0; sent < available; sent += blockSize) {
                if (cancel.isCancelled())
                    return VfsError::make(VfsError::Cancelled, QStringLiteral("cancelled"));
                if (sent >= m_limit) {
                    // What the stall guard reports, two minutes after the bytes
                    // stopped: nothing came, and nobody said why.
                    return VfsError::make(VfsError::NetworkError,
                        QStringLiteral("the transfer stopped after %1 bytes and nothing more arrived")
                            .arg(sent));
                }

                const qint64 size = std::min<qint64>(blockSize, available - sent);
                if (sink.write(m_contents.constData() + offset + sent, size) != size)
                    return VfsError::make(VfsError::IoError, QStringLiteral("the reader went away"));
            }
            return VfsError::ok();
        };
    }

    /// What each connection was asked for, in the order the spans were asked.
    struct Span
    {
        qint64 offset = 0;
        qint64 length = 0;
    };
    QList<Span> asked() const
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        return m_asked;
    }

private:
    QByteArray m_contents;
    qint64 m_limit;
    mutable std::mutex m_mutex;
    QList<Span> m_asked;
};

/// A server that hands over the file a block at a time and only when it is let
/// go, so a test can say exactly how much has arrived before it does anything
/// else.
///
/// Everything here is a condition rather than a delay: the test waits until the
/// transfer is demonstrably waiting for the next block, which is a fact about
/// the fake and not about how fast the machine is.
class PacedServer
{
public:
    PacedServer(QByteArray contents, int blockSize)
        : m_contents(std::move(contents))
        , m_blockSize(blockSize)
    {
    }

    net::StreamingDownload::Fetch fetch()
    {
        return [this](QIODevice& sink, qint64 offset, qint64 span, const CancelToken& cancel) {
            {
                const std::lock_guard<std::mutex> guard(m_mutex);
                ++m_spans;
                m_started.notify_all();
            }

            const qint64 available = std::min(span, static_cast<qint64>(m_contents.size()) - offset);
            for (qint64 sent = 0; sent < available; sent += m_blockSize) {
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    if (m_credits == 0 && !m_open) {
                        m_waiting = true;
                        m_parked.notify_all();
                        m_go.wait(lock, [this] { return m_credits > 0 || m_open; });
                        m_waiting = false;
                    }
                    if (!m_open)
                        --m_credits;
                }
                if (cancel.isCancelled())
                    return VfsError::make(VfsError::Cancelled, QStringLiteral("cancelled"));

                const qint64 size = std::min<qint64>(m_blockSize, available - sent);
                if (sink.write(m_contents.constData() + offset + sent, size) != size)
                    return VfsError::make(VfsError::IoError, QStringLiteral("the reader went away"));
            }
            return VfsError::ok();
        };
    }

    /// Lets `blocks` more blocks through, and no more.
    void allow(int blocks)
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_credits += blocks;
        m_go.notify_all();
    }

    /// Returns once every block allowed so far has been handed over and the
    /// transfer is waiting for the next one.
    void waitUntilItIsWaitingForMore()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_parked.wait(lock, [this] { return m_waiting; });
    }

    /// Leaves the transfer where it is for a moment, so that a reader which was
    /// going to give up on it has the chance to.
    ///
    /// The one thing in this file that watches a clock, and what hangs on it is
    /// only how long the test waits -- never what it concludes. A second
    /// transfer is counted the moment it starts and the count is never
    /// forgotten, so a stream that starts one is caught by the assertion either
    /// way; the wait is what stops the bytes from arriving first and making the
    /// question moot.
    void waitForASecondTransfer(std::chrono::milliseconds patience)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_started.wait_for(lock, patience, [this] { return m_spans > 1; });
    }

    /// Lets the rest of the file through. Also what stops a parked transfer from
    /// hanging the test: a stream is closed by cancelling the fetch and joining
    /// its thread, and a thread waiting here would never see the cancellation.
    void release()
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_open = true;
        m_go.notify_all();
    }

    int spans() const
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        return m_spans;
    }

private:
    QByteArray m_contents;
    int m_blockSize;

    mutable std::mutex m_mutex;
    std::condition_variable m_parked;
    std::condition_variable m_go;
    std::condition_variable m_started;
    int m_credits = 0;
    int m_spans = 0;
    bool m_waiting = false;
    bool m_open = false;
};

/// Lets a paced server go however the test ends, including on a failed
/// assertion. Declared after the stream so it runs before the stream is closed.
struct LetGo
{
    PacedServer& server;
    ~LetGo() { server.release(); }
};

QByteArray payloadOf(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 7 + (i >> 8)) & 0xff);
    return data;
}

/// Reads to the end, or until the stream says it failed.
QByteArray readAll(net::StreamingDownload& stream, qint64* lastResult)
{
    QByteArray collected;
    QByteArray buffer(16 * 1024, Qt::Uninitialized);
    qint64 got = 0;
    while ((got = stream.read(buffer.data(), buffer.size())) > 0)
        collected.append(buffer.constData(), static_cast<int>(got));
    *lastResult = got;
    return collected;
}

} // namespace

/// The read stream that makes a file larger than the disk copyable.
class TestStreamingDownload : public QObject
{
    Q_OBJECT

private slots:
    void readsTheWholeFileThroughSeveralSpans();
    void readsNothingUntilItIsAskedTo();
    void aTransferThatFailsPartWayIsAnError();
    void aTransferThatEndsShortIsAnError();
    void seekingForwardsInsideTheBufferDoesNotRefetch();
    void aForwardSeekIsDecidedByDistanceNotByWhatHasArrived();
    void aForwardSeekWaitsForBytesThatHaveNotArrivedYet();
    void seekingBackwardsStartsAgain();
    void abandoningTheStreamDoesNotHang();

    void aFileLongerThanOneConnectionCanCarryStillArrivesWhole();
    void thatSameFileOverOneConnectionDiesPartWay();
};

void TestStreamingDownload::readsTheWholeFileThroughSeveralSpans()
{
    const QByteArray payload = payloadOf(300 * 1024);
    FakeServer server(payload);

    // A span size that does not divide the file, so the last one is short --
    // which is how the end of a file announces itself.
    net::StreamingDownload stream(server.fetch(), payload.size(), 128 * 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));
    QCOMPARE(stream.size(), static_cast<qint64>(payload.size()));

    QByteArray collected;
    while (collected.size() < payload.size()) {
        const QByteArray chunk = stream.read(64 * 1024);
        QVERIFY2(!chunk.isEmpty(), "the stream stopped before the end of the file");
        collected.append(chunk);
    }

    QCOMPARE(collected.size(), payload.size());
    QCOMPARE(collected, payload);
    QCOMPARE(server.spans(), 3);
    // And then it ends, rather than blocking or repeating itself.
    QCOMPARE(stream.read(1024).size(), 0);
    QVERIFY(stream.atEnd());
}

void TestStreamingDownload::readsNothingUntilItIsAskedTo()
{
    const QByteArray payload = payloadOf(64 * 1024);
    FakeServer server(payload);

    {
        net::StreamingDownload stream(server.fetch(), payload.size(), 32 * 1024);
        QVERIFY(stream.open(QIODevice::ReadOnly));
        // Opening a file is not reading it. A preview that opens ten files and
        // reads one must not fetch ten.
        QCOMPARE(server.spans(), 0);
    }
    QCOMPARE(server.spans(), 0);
}

void TestStreamingDownload::aTransferThatFailsPartWayIsAnError()
{
    const QByteArray payload = payloadOf(256 * 1024);
    FakeServer server(payload);
    server.failAfter(100 * 1024);

    net::StreamingDownload stream(server.fetch(), payload.size(), 512 * 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));

    // What the caller must see is a failure, not a short file: read() answers
    // -1 rather than 0, and says why.
    QByteArray buffer(16 * 1024, Qt::Uninitialized);
    qint64 total = 0;
    qint64 got = 0;
    while ((got = stream.read(buffer.data(), buffer.size())) > 0)
        total += got;

    QCOMPARE(got, -1);
    QVERIFY(total < payload.size());
    QVERIFY(stream.error().isError());
    QVERIFY2(stream.errorString().contains(QStringLiteral("hung up")), qPrintable(stream.errorString()));
}

void TestStreamingDownload::aTransferThatEndsShortIsAnError()
{
    // The server has less than it said it had: every span succeeds, and the
    // file still ends early. Reporting that as the end of the file is how a
    // truncated copy would be called a whole one.
    const QByteArray payload = payloadOf(100 * 1024);
    FakeServer server(payload);

    net::StreamingDownload stream(server.fetch(), payload.size() + 50 * 1024, 512 * 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));

    QByteArray buffer(16 * 1024, Qt::Uninitialized);
    qint64 total = 0;
    qint64 got = 0;
    while ((got = stream.read(buffer.data(), buffer.size())) > 0)
        total += got;

    QCOMPARE(got, -1);
    QCOMPARE(total, static_cast<qint64>(payload.size()));
    QVERIFY2(
        stream.errorString().contains(QStringLiteral("stopped after")), qPrintable(stream.errorString()));
}

void TestStreamingDownload::seekingForwardsInsideTheBufferDoesNotRefetch()
{
    const QByteArray payload = payloadOf(200 * 1024);
    FakeServer server(payload);

    net::StreamingDownload stream(server.fetch(), payload.size(), 512 * 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));

    QCOMPARE(stream.read(1024).size(), 1024);
    const int spansSoFar = server.spans();

    QVERIFY(stream.seek(50 * 1024));
    QCOMPARE(stream.read(1024), payload.mid(50 * 1024, 1024));
    // A short hop forwards is answered from what has already arrived.
    QCOMPARE(server.spans(), spansSoFar);
}

void TestStreamingDownload::aForwardSeekIsDecidedByDistanceNotByWhatHasArrived()
{
    // The rule is a distance: a hop of up to a bufferful is answered from the
    // transfer that is running, and anything further starts another. What has
    // actually arrived by then does not come into it, and must not -- a stream
    // that starts a second transfer only when the network happened to be slow
    // reads differently on every run, and nothing can be tested against it.
    const int size = static_cast<int>(net::kStreamBufferBytes) + 64 * 1024;
    const QByteArray payload = payloadOf(size);

    {
        FakeServer server(payload);
        net::StreamingDownload stream(server.fetch(), payload.size(), payload.size());
        QVERIFY(stream.open(QIODevice::ReadOnly));
        QCOMPARE(stream.read(1024).size(), 1024);

        // Exactly a bufferful ahead: the far edge of what one transfer answers.
        QVERIFY(stream.seek(1024 + net::kStreamBufferBytes));
        QCOMPARE(stream.read(1024), payload.mid(1024 + static_cast<int>(net::kStreamBufferBytes), 1024));
        QCOMPARE(server.spans(), 1);
    }

    {
        FakeServer server(payload);
        net::StreamingDownload stream(server.fetch(), payload.size(), payload.size());
        QVERIFY(stream.open(QIODevice::ReadOnly));
        QCOMPARE(stream.read(1024).size(), 1024);

        // One byte further, and it is cheaper to ask again than to pour a
        // bufferful down the drain.
        QVERIFY(stream.seek(1025 + net::kStreamBufferBytes));
        QCOMPARE(stream.read(1024), payload.mid(1025 + static_cast<int>(net::kStreamBufferBytes), 1024));
        QCOMPARE(server.spans(), 2);
    }
}

void TestStreamingDownload::aForwardSeekWaitsForBytesThatHaveNotArrivedYet()
{
    // The same hop, with the bytes demonstrably not there yet: one block has
    // been handed over and the server is waiting to be let go of the next. A
    // stream that looked at what had arrived would give up on the transfer here
    // and start another -- an SSH handshake and a discarded buffer, for a hop of
    // nineteen kilobytes -- and would do it only on the runs where the network
    // was slower than the reader.
    const QByteArray payload = payloadOf(200 * 1024);
    PacedServer server(payload, 8 * 1024);

    net::StreamingDownload stream(server.fetch(), payload.size(), payload.size());
    LetGo letGo { server };
    QVERIFY(stream.open(QIODevice::ReadOnly));

    server.allow(1);
    QCOMPARE(stream.read(1024).size(), 1024);
    server.waitUntilItIsWaitingForMore();

    // The seek runs on a thread of its own because it is meant to block: the
    // bytes it wants have not been handed over, and this thread is the only
    // thing that can hand them over. It waits before doing so, because a stream
    // that gave up on the transfer would do it here -- with the bytes still
    // missing -- and letting them through first would answer a different
    // question, the one where they had already arrived.
    bool seeked = false;
    std::thread reader([&] { seeked = stream.seek(20 * 1024); });
    server.waitForASecondTransfer(std::chrono::milliseconds(100));
    server.release();
    reader.join();

    QVERIFY(seeked);
    QCOMPARE(stream.read(1024), payload.mid(20 * 1024, 1024));
    QCOMPARE(server.spans(), 1);
}

void TestStreamingDownload::seekingBackwardsStartsAgain()
{
    const QByteArray payload = payloadOf(200 * 1024);
    FakeServer server(payload);

    net::StreamingDownload stream(server.fetch(), payload.size(), 512 * 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));

    QCOMPARE(stream.read(80 * 1024).size(), 80 * 1024);
    QVERIFY(stream.seek(1024));
    QCOMPARE(stream.read(512), payload.mid(1024, 512));
    QVERIFY2(server.spans() > 1, "going backwards has to fetch again");
}

void TestStreamingDownload::abandoningTheStreamDoesNotHang()
{
    // A transfer far larger than the buffer, read once and then dropped. The
    // fetch is blocked on a full buffer at that moment, and closing has to
    // release it -- otherwise every cancelled copy leaks a stuck thread.
    const QByteArray payload = payloadOf(40 * 1024 * 1024);
    FakeServer server(payload);

    net::StreamingDownload stream(server.fetch(), payload.size(), 64 * 1024 * 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));
    QCOMPARE(stream.read(4096).size(), 4096);
    stream.close();

    QVERIFY(true); // reaching here at all is the assertion
}

void TestStreamingDownload::aFileLongerThanOneConnectionCanCarryStillArrivesWhole()
{
    // The re-key fault, and the reason the span loop exists. An SFTP transfer
    // that runs past the point where the session re-keys stops dead: the file
    // is half there, the connection is open, and nothing more arrives. Every
    // span being a connection of its own is what keeps any one of them clear of
    // it, and a file of any size is then just more spans.
    const qint64 diesPast = 100 * 1024;
    const qint64 span = 64 * 1024;
    const QByteArray payload = payloadOf(512 * 1024);
    ServerThatDiesPastALimit server(payload, diesPast);

    net::StreamingDownload stream(server.fetch(), payload.size(), span);
    QVERIFY(stream.open(QIODevice::ReadOnly));

    qint64 last = 0;
    const QByteArray collected = readAll(stream, &last);
    QCOMPARE(last, 0); // the end of the file, not a failure
    QCOMPARE(collected.size(), payload.size());
    // Content rather than size: a span fetched from the wrong offset, or fetched
    // twice, weighs exactly the same as the file it was supposed to be.
    QCOMPARE(collected, payload);

    const QList<ServerThatDiesPastALimit::Span> asked = server.asked();
    QVERIFY2(asked.size() > 1,
        "a file this size has to be more than one connection for the test to mean "
        "anything");
    qint64 expectedOffset = 0;
    for (const ServerThatDiesPastALimit::Span& one : asked) {
        QCOMPARE(one.offset, expectedOffset);
        QVERIFY2(one.length <= span,
            "a span longer than it was told to be is a connection that will meet "
            "the fault");
        QVERIFY2(one.length <= diesPast, "the spans have to stay clear of the limit, not merely near it");
        expectedOffset += one.length;
    }
    QCOMPARE(expectedOffset, static_cast<qint64>(payload.size()));
}

void TestStreamingDownload::thatSameFileOverOneConnectionDiesPartWay()
{
    // The control, and the reason the test above is not green for the wrong
    // reason: the same server, asked for the whole file in one go, does exactly
    // what the real one does -- stops part way and says nothing more arrived.
    const QByteArray payload = payloadOf(512 * 1024);
    ServerThatDiesPastALimit server(payload, 100 * 1024);

    net::StreamingDownload stream(server.fetch(), payload.size(), payload.size());
    QVERIFY(stream.open(QIODevice::ReadOnly));

    qint64 last = 0;
    const QByteArray collected = readAll(stream, &last);
    QCOMPARE(last, -1);
    QVERIFY(collected.size() < payload.size());
    QVERIFY2(stream.errorString().contains(QStringLiteral("nothing more arrived")),
        qPrintable(stream.errorString()));
}

MOLE_TEST_MAIN(TestStreamingDownload)

#include "tst_StreamingDownload.moc"
