#include "plugins/network/TransferStreams.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
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
                        ++m_parks;
                        m_parked.notify_all();
                        // Polled rather than notified, the way a transfer polls
                        // the cancel token it was handed: a stream is closed by
                        // cancelling and joining, and a fake that could only be
                        // woken by the test would hang that instead of modelling
                        // it. Nothing asserted anywhere waits on this interval.
                        while (m_credits == 0 && !m_open && !cancel.isCancelled())
                            m_go.wait_for(lock, std::chrono::milliseconds(2));
                        m_waiting = false;
                    }
                    if (!m_open && m_credits > 0)
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
    ///
    /// Counted rather than flagged: `waiting` goes false again the moment the
    /// transfer is let go, so a test pacing block after block would otherwise
    /// see a stale one and hand over two at once.
    void waitUntilItIsWaitingForMore(int parkNumber = 1)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_parked.wait(lock, [this, parkNumber] { return m_parks >= parkNumber; });
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
    int m_parks = 0;
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

/// A server that can be rewritten between spans, and that has an ETag.
///
/// The fault this stands for is ordinary: a log rotated, a backup re-run, a
/// photo re-exported over itself, while a hundred-gigabyte read is walking
/// through it a span at a time. Each span is an independent request by byte
/// offset and nothing in the protocol says which version answered it, so the
/// reader is handed the first half of one file and the second half of another,
/// of exactly the right total length.
class RewritableServer
{
public:
    explicit RewritableServer(QByteArray contents)
        : m_contents(std::move(contents))
        , m_etag(QStringLiteral("one"))
    {
    }

    net::StreamingDownload::Fetch fetch()
    {
        return [this](QIODevice& sink, qint64 offset, qint64 span, const CancelToken&) {
            ++m_spans;
            const std::lock_guard<std::mutex> guard(m_mutex);
            const qint64 available = std::min(span, static_cast<qint64>(m_contents.size()) - offset);
            if (available > 0 && sink.write(m_contents.constData() + offset, available) != available)
                return VfsError::make(VfsError::IoError, QStringLiteral("the reader went away"));
            return VfsError::ok();
        };
    }

    /// What a backend with an ETag does: hold the value the read began on and
    /// compare it before every later span.
    ///
    /// `holdTheFirstCheck` makes the first one wait until release(), which is
    /// how a test gets to rewrite the file at a known point rather than racing
    /// the fetch thread: the stream reads ahead into an eight-megabyte buffer,
    /// so a small file is fetched whole before a test can do anything to it.
    net::StreamingDownload::StillTheSameFile guard(bool holdTheFirstCheck = false)
    {
        return [this, holdTheFirstCheck, opened = etag()]() -> VfsError {
            if (holdTheFirstCheck && m_checks == 0) {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_released.wait(lock, [this] { return m_release; });
            }
            ++m_checks;
            return etag() == opened ? VfsError::ok() : net::fileChangedWhileBeingRead();
        };
    }

    /// Lets a held check go.
    void release()
    {
        {
            const std::lock_guard<std::mutex> guard(m_mutex);
            m_release = true;
        }
        m_released.notify_all();
    }

    /// The whole file is replaced, and says so.
    void rewriteWith(const QByteArray& contents, const QString& etag)
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_contents = contents;
        m_etag = etag;
    }

    QString etag() const
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        return m_etag;
    }

    int spans() const { return m_spans; }
    int checks() const { return m_checks; }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_released;
    bool m_release = false;
    QByteArray m_contents;
    QString m_etag;
    std::atomic<int> m_spans { 0 };
    std::atomic<int> m_checks { 0 };
};

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
    void thatSameFileOverOneSpanResumesUntilItIsWhole();
    void aSpanThatKeepsFailingEndsTheReadWhenTheBudgetIsSpent();
    void aLinkThatComesBackInsideTheBudgetCostsNothing();
    void aRefusalThatWillNotChangeIsNotWaitedOut();

    void seekingToTheStartToTheEndAndPastIt();
    void aSpanBoundaryThatFallsOnAReadBoundaryIsInvisible();
    void aFileOfNoBytesAndAFileOfOne_data();
    void aFileOfNoBytesAndAFileOfOne();
    void aFileTheSizeOfTheBufferArrivesWhole_data();
    void aFileTheSizeOfTheBufferArrivesWhole();
    void aFetchFasterThanTheReaderFillsUpAndWaits();
    void aReaderFasterThanTheFetchWaitsAndGetsEverything();
    void aFileRewrittenBetweenTwoSpansFailsRatherThanStitchingThem();
    void aFileThatIsStillItselfIsCheckedAndNotDisturbed();
    void aStreamWithNoCheckReadsExactlyAsItDidBefore();
    void readingAgainAfterAnErrorSaysTheSameThing();
    void closingDuringABlockedReadDoesNotHang();
    void aStreamKeepsAliveTheDriveItWasOpenedOn();
    void everyDeviceABackendHandsOutHoldsItsDrive();
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

    // A budget of a second rather than the two minutes a real read gets: what
    // is being asserted is that a read which cannot finish comes back as a
    // failure, and waiting out the real figure would make this the slowest test
    // in the suite for no extra claim.
    net::StreamingDownload stream(server.fetch(), payload.size(), 512 * 1024, std::chrono::seconds(1));
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

    auto stream = std::make_unique<net::StreamingDownload>(server.fetch(), payload.size(), 64 * 1024 * 1024);
    QVERIFY(stream->open(QIODevice::ReadOnly));
    QCOMPARE(stream->read(4096).size(), 4096);

    // **The close happens on a thread of its own, and that is the assertion.**
    // This was `stream.close(); QVERIFY(true); // reaching here at all is the
    // assertion` -- and the failure mode is a *hang*, so the only thing that
    // reported it was the suite's 180-second timeout, which kills the whole
    // binary and names no case. A closer that never returns is now a five-second
    // wait and a sentence. See MOLE-399.
    std::atomic_bool closed { false };
    std::thread closer([&stream, &closed] {
        stream->close();
        stream.reset();
        closed.store(true);
    });
    const bool released = mole::test::waitFor([&closed] { return closed.load(); }, 5000);
    if (released)
        closer.join();
    else
        closer.detach(); // leaked deliberately: joining a stuck thread hangs the suite

    QVERIFY2(released,
        "closing a blocked stream did not release the fetch thread -- every cancelled copy "
        "leaks one");
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

void TestStreamingDownload::thatSameFileOverOneSpanResumesUntilItIsWhole()
{
    // MOLE-99 in miniature, and the reversal of what this test used to assert.
    //
    // The same server, asked for the whole file in one span, meets its limit
    // part way -- which is what a server whose RekeyLimit falls *inside* the
    // span does, and no span size avoids it because the client cannot know what
    // the server's limit is. It used to be a failure, and then the file could
    // not be read at all: every attempt stopped in the same place.
    //
    // Now the span resumes from the byte it reached. It costs one stall-guard
    // wait per limit met, which is what ADR-0013 rejected resuming for, against
    // a file that otherwise cannot be read at all.
    const QByteArray payload = payloadOf(512 * 1024);
    ServerThatDiesPastALimit server(payload, 100 * 1024);

    net::StreamingDownload stream(server.fetch(), payload.size(), payload.size());
    QVERIFY(stream.open(QIODevice::ReadOnly));

    qint64 last = 0;
    const QByteArray collected = readAll(stream, &last);
    QCOMPARE(last, 0);
    QVERIFY2(!stream.error().isError(), qPrintable(stream.errorString()));
    QVERIFY2(collected == payload,
        "every byte, and the right bytes: a resume that restarted at the "
        "wrong offset would still be the right length");

    // And it really did take several goes, or the server was not provoking the
    // fault this is about.
    QVERIFY2(
        server.asked().size() > 1, "the whole file arrived in one connection, so the limit was never met");
}

void TestStreamingDownload::aSpanThatKeepsFailingEndsTheReadWhenTheBudgetIsSpent()
{
    // What bounds a read now, and the case that used to be bounded by counting
    // attempts instead.
    //
    // This used to assert that a span delivering nothing ends the read there and
    // then -- one stall, one attempt that gets nowhere, done. That covered a link
    // that had gone for good and could not cover one that comes back, because
    // the single retry met the dead link and failed a read the link was about to
    // be able to finish. The bound is now the budget: however many connections
    // it takes, what ends the read is nothing having arrived for long enough.
    const QByteArray payload = payloadOf(256 * 1024);
    FakeServer server(payload);
    server.failAfter(100 * 1024);

    // A short budget so the test costs a moment. The mechanism is the same at
    // half a second as at two minutes.
    net::StreamingDownload stream(server.fetch(), payload.size(), 512 * 1024, std::chrono::seconds(1));
    QVERIFY(stream.open(QIODevice::ReadOnly));

    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    const QByteArray collected = readAll(stream, &last);
    const qint64 took = clock.elapsed();

    QCOMPARE(last, -1);
    QVERIFY(collected.size() < payload.size());
    QVERIFY2(stream.errorString().contains(QStringLiteral("hung up")), qPrintable(stream.errorString()));

    // It kept trying rather than giving up on the first refusal -- that is the
    // whole change -- and it stopped once the budget was spent rather than
    // going on for ever.
    QVERIFY2(server.spans() > 2,
        qPrintable(QStringLiteral("only %1 attempts: a budget was not being spent").arg(server.spans())));
    QVERIFY2(
        took >= 1000, qPrintable(QStringLiteral("gave up after %1 ms, inside its own budget").arg(took)));
    QVERIFY2(took < 20000, qPrintable(QStringLiteral("still going after %1 ms").arg(took)));
}

void TestStreamingDownload::aLinkThatComesBackInsideTheBudgetCostsNothing()
{
    // The case the old bound could not meet, and the reason for this ticket: a
    // link that goes away and returns must not cost a transfer. Every attempt
    // fails, delivering nothing, until the link comes back -- and then the file
    // arrives whole.
    //
    // Counted in attempts rather than timed, so it is the same on every machine.
    const QByteArray payload = payloadOf(300 * 1024);
    FakeServer server(payload);

    int refusals = 6;
    auto flaky
        = [&server, &refusals](QIODevice& sink, qint64 offset, qint64 span, const CancelToken& cancel) {
              if (refusals > 0) {
                  --refusals;
                  return VfsError::make(VfsError::NetworkError, QStringLiteral("the link is down"));
              }
              return server.fetch()(sink, offset, span, cancel);
          };

    net::StreamingDownload stream(std::move(flaky), payload.size(), 128 * 1024, std::chrono::seconds(30));
    QVERIFY(stream.open(QIODevice::ReadOnly));

    qint64 last = 0;
    const QByteArray collected = readAll(stream, &last);

    QCOMPARE(last, 0);
    QVERIFY2(!stream.error().isError(), qPrintable(stream.errorString()));
    QVERIFY2(collected == payload,
        "the file did not come back whole after the link returned, which is the whole point");
    QCOMPARE(refusals, 0);
}

/// A failure that cannot go the other way, waited out for two minutes.
///
/// The retry loop bounded itself by the budget and read the *kind* not at all,
/// so a 403 whose credentials had expired mid-copy, a 404 for an object deleted
/// between spans and a NotSupported each sat out the whole 120 seconds -- per
/// file -- before reporting what the first attempt already knew. Only a
/// connection, a short body and an unclassified failure can go the other way
/// next time. See net::isWorthRetrying and MOLE-373.
void TestStreamingDownload::aRefusalThatWillNotChangeIsNotWaitedOut()
{
    const QByteArray payload = payloadOf(256 * 1024);

    struct Refuser
    {
        VfsError::Code code;
        int spans = 0;
    };

    // A generous budget, so the assertion is about the kind and not about the
    // clock running out.
    for (const VfsError::Code code : { VfsError::AccessDenied, VfsError::NotFound, VfsError::NotSupported }) {
        auto refuser = std::make_shared<Refuser>(Refuser { code, 0 });
        auto fetch = [refuser](QIODevice&, qint64, qint64, const CancelToken&) {
            ++refuser->spans;
            return VfsError::make(refuser->code, QStringLiteral("the server said no"));
        };

        QElapsedTimer clock;
        clock.start();
        net::StreamingDownload stream(fetch, payload.size(), 64 * 1024, std::chrono::seconds(30));
        QVERIFY(stream.open(QIODevice::ReadOnly));

        QByteArray buffer(16 * 1024, Qt::Uninitialized);
        QCOMPARE(stream.read(buffer.data(), buffer.size()), qint64(-1));
        QVERIFY(stream.error().isError());
        QCOMPARE(stream.error().code, code);

        // Well under the budget, and one attempt rather than a stream of them.
        QVERIFY2(clock.elapsed() < 5000,
            qPrintable(QStringLiteral("waited %1 ms for a refusal").arg(clock.elapsed())));
        QCOMPARE(refuser->spans, 1);
    }

    // And a connection failure is still retried, because that is what retrying
    // is for: this one comes good on the third attempt.
    auto flaky = std::make_shared<int>(0);
    auto comesBack = [flaky, payload](QIODevice& sink, qint64 offset, qint64 span, const CancelToken&) {
        if (++*flaky < 3)
            return VfsError::make(VfsError::NetworkError, QStringLiteral("the link went away"));
        const qint64 available = std::min(span, qint64(payload.size()) - offset);
        if (available > 0)
            sink.write(payload.constData() + offset, available);
        return VfsError::ok();
    };
    net::StreamingDownload survives(comesBack, payload.size(), 64 * 1024, std::chrono::seconds(30));
    QVERIFY(survives.open(QIODevice::ReadOnly));
    QCOMPARE(survives.readAll().size(), payload.size());
    QVERIFY(!survives.error().isError());
    QVERIFY(*flaky >= 3);
}

void TestStreamingDownload::seekingToTheStartToTheEndAndPastIt()
{
    // The ends of the device, which is where anything that previews a file goes
    // first: to the front for a header and to the back for a footer.
    const QByteArray payload = payloadOf(64 * 1024);
    FakeServer server(payload);

    net::StreamingDownload stream(server.fetch(), payload.size(), 16 * 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));

    QVERIFY(stream.seek(0));
    QCOMPARE(stream.read(16), payload.left(16));

    // Exactly the end is a legal position holding nothing, not a failure.
    QVERIFY(stream.seek(payload.size()));
    QVERIFY(stream.atEnd());
    QCOMPARE(stream.read(16).size(), 0);

    // Past the end is refused rather than answered with rubbish, and the device
    // is still usable afterwards.
    QVERIFY(!stream.seek(payload.size() + 1));
    QVERIFY(!stream.seek(-1));
    QVERIFY(stream.seek(1024));
    QCOMPARE(stream.read(16), payload.mid(1024, 16));
}

void TestStreamingDownload::aSpanBoundaryThatFallsOnAReadBoundaryIsInvisible()
{
    // The seam between two transfers, landing exactly where a read ends. Every
    // span is a separate connection, so this is the arrangement where an
    // off-by-one at either end shows up as a missing or a repeated block -- and
    // the reader is supposed to see one continuous file.
    const qint64 span = 32 * 1024;
    const QByteArray payload = payloadOf(4 * static_cast<int>(span));
    FakeServer server(payload);

    net::StreamingDownload stream(server.fetch(4096), payload.size(), span);
    QVERIFY(stream.open(QIODevice::ReadOnly));

    QByteArray collected;
    for (int i = 0; i < 4; ++i) {
        const QByteArray chunk = stream.read(span);
        QCOMPARE(chunk.size(), static_cast<int>(span));
        collected.append(chunk);
    }
    QCOMPARE(collected, payload);
    QCOMPARE(server.spans(), 4);
    QCOMPARE(stream.read(1).size(), 0);
}

void TestStreamingDownload::aFileOfNoBytesAndAFileOfOne_data()
{
    QTest::addColumn<int>("size");
    QTest::newRow("no bytes at all") << 0;
    QTest::newRow("one byte") << 1;
}

void TestStreamingDownload::aFileOfNoBytesAndAFileOfOne()
{
    QFETCH(int, size);

    // The degenerate ends of every loop in the class, and both are real files.
    const QByteArray payload = payloadOf(size);
    FakeServer server(payload);

    net::StreamingDownload stream(server.fetch(), size, 64 * 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));
    QCOMPARE(stream.size(), static_cast<qint64>(size));

    QCOMPARE(stream.readAll(), payload);
    QVERIFY(stream.atEnd());
    QCOMPARE(stream.read(16).size(), 0);
    // An empty file is not a transfer: there is nothing to ask a server for.
    QCOMPARE(server.spans(), size == 0 ? 0 : 1);
}

void TestStreamingDownload::aFileTheSizeOfTheBufferArrivesWhole_data()
{
    QTest::addColumn<qint64>("size");

    // The threshold in this class: what fits between the transfer and the
    // reader. Below it the fetch never waits, at it the fetch waits exactly
    // once, and above it the two take turns for the rest of the file.
    QTest::newRow("a buffer less one") << net::kStreamBufferBytes - 1;
    QTest::newRow("exactly a buffer") << net::kStreamBufferBytes;
    QTest::newRow("a buffer and one") << net::kStreamBufferBytes + 1;
}

void TestStreamingDownload::aFileTheSizeOfTheBufferArrivesWhole()
{
    QFETCH(qint64, size);

    const QByteArray payload = payloadOf(static_cast<int>(size));
    FakeServer server(payload);

    net::StreamingDownload stream(server.fetch(), size, size + 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));

    qint64 last = 0;
    const QByteArray collected = readAll(stream, &last);
    QCOMPARE(last, 0);
    QCOMPARE(collected.size(), payload.size());
    QCOMPARE(collected, payload);
}

void TestStreamingDownload::aFetchFasterThanTheReaderFillsUpAndWaits()
{
    // A server handing bytes over faster than anything is reading them. The
    // buffer is what stops that from becoming unbounded memory, and it works by
    // making the transfer wait -- so the file has to be larger than the buffer
    // for the waiting to happen at all.
    const QByteArray payload = payloadOf(static_cast<int>(net::kStreamBufferBytes) + 4 * 1024 * 1024);
    FakeServer server(payload);

    net::StreamingDownload stream(server.fetch(64 * 1024), payload.size(), payload.size());
    QVERIFY(stream.open(QIODevice::ReadOnly));

    QByteArray collected;
    QByteArray buffer(8 * 1024, Qt::Uninitialized);
    qint64 got = 0;
    while ((got = stream.read(buffer.data(), buffer.size())) > 0)
        collected.append(buffer.constData(), static_cast<int>(got));

    QCOMPARE(got, 0);
    QCOMPARE(collected.size(), payload.size());
    QCOMPARE(collected, payload);
    QCOMPARE(server.spans(), 1);
}

void TestStreamingDownload::aReaderFasterThanTheFetchWaitsAndGetsEverything()
{
    // The other way round: a reader that has asked for everything and a server
    // handing over one block at a time. The reader has to wait rather than see
    // the end of the file early, which is the difference between a slow copy and
    // a truncated one.
    const int blockSize = 8 * 1024;
    const QByteArray payload = payloadOf(10 * blockSize);
    PacedServer server(payload, blockSize);

    net::StreamingDownload stream(server.fetch(), payload.size(), payload.size());
    LetGo letGo { server };
    QVERIFY(stream.open(QIODevice::ReadOnly));

    QByteArray collected;
    qint64 last = 0;
    std::thread reader([&] { collected = readAll(stream, &last); });

    // One block per park, so the reader is genuinely waiting between them
    // rather than being handed a file that had already arrived.
    for (int block = 1; block <= 10; ++block) {
        server.waitUntilItIsWaitingForMore(block);
        server.allow(1);
    }
    reader.join();

    QCOMPARE(last, 0);
    QCOMPARE(collected, payload);
    QCOMPARE(server.spans(), 1);
}

/// The stitched file, refused.
///
/// The reader gets the first span and then an error -- never the second span's
/// bytes, because the check runs before they are asked for. Getting that order
/// wrong would leave the wrong half of another file in the buffer for a reader
/// that ignores the return code, and the whole point is that nothing downstream
/// has to.
void TestStreamingDownload::aFileRewrittenBetweenTwoSpansFailsRatherThanStitchingThem()
{
    const QByteArray first(200 * 1024, 'a');
    const QByteArray second(200 * 1024, 'b');
    RewritableServer server(first);

    net::StreamingDownload stream(server.fetch(), first.size(), 64 * 1024);
    stream.checkBeforeEverySpan(server.guard(true));
    QVERIFY(stream.open(QIODevice::ReadOnly));

    // The first span arrives, and then the file underneath becomes another one.
    // The check for the second span is held until it has, so this is a sequence
    // rather than a race.
    const QByteArray opening = stream.read(64 * 1024);
    QCOMPARE(opening, first.left(64 * 1024));
    server.rewriteWith(second, QStringLiteral("two"));
    server.release();

    qint64 last = 0;
    const QByteArray rest = readAll(stream, &last);
    QCOMPARE(last, -1);
    QCOMPARE(stream.error().code, VfsError::IoError);
    QCOMPARE(stream.error().message, QStringLiteral("the file changed while it was being read"));

    // Not one byte of the file that replaced it.
    QVERIFY2(!rest.contains('b'), "the second file's bytes were handed to the reader");
    QVERIFY2(server.spans() <= 2, "a span was fetched after the file was known to have changed");
}

/// And a file that is still itself is read exactly as before, having been asked.
void TestStreamingDownload::aFileThatIsStillItselfIsCheckedAndNotDisturbed()
{
    const QByteArray payload = payloadOf(300 * 1024);
    RewritableServer server(payload);

    net::StreamingDownload stream(server.fetch(), payload.size(), 128 * 1024);
    stream.checkBeforeEverySpan(server.guard());
    QVERIFY(stream.open(QIODevice::ReadOnly));

    qint64 last = 0;
    QCOMPARE(readAll(stream, &last), payload);
    QCOMPARE(last, 0);
    QCOMPARE(server.spans(), 3);
    // Before every span but the first: three spans, two checks. The first is the
    // one the validator was taken from and asking about it would be asking
    // whether the file is the file.
    QCOMPARE(server.checks(), 2);
}

/// A drive that folds the question into its own request sets no check, and
/// nothing changes for it.
void TestStreamingDownload::aStreamWithNoCheckReadsExactlyAsItDidBefore()
{
    const QByteArray payload = payloadOf(300 * 1024);
    RewritableServer server(payload);

    net::StreamingDownload stream(server.fetch(), payload.size(), 128 * 1024);
    QVERIFY(stream.open(QIODevice::ReadOnly));

    qint64 last = 0;
    QCOMPARE(readAll(stream, &last), payload);
    QCOMPARE(last, 0);
    QCOMPARE(server.checks(), 0);
}

void TestStreamingDownload::readingAgainAfterAnErrorSaysTheSameThing()
{
    // Whoever is copying the file asks again -- a loop does not stop on the
    // first -1 unless somebody wrote it to. A second read must not turn the
    // failure into an end of file, which would leave the copy calling a
    // truncated result complete.
    const QByteArray payload = payloadOf(256 * 1024);
    FakeServer server(payload);
    server.failAfter(64 * 1024);

    // A short budget, for the same reason as above: the claim is about the
    // second read, not about how long the first one is willing to wait.
    net::StreamingDownload stream(server.fetch(), payload.size(), payload.size(), std::chrono::seconds(1));
    QVERIFY(stream.open(QIODevice::ReadOnly));

    qint64 first = 0;
    readAll(stream, &first);
    QCOMPARE(first, -1);
    const QString said = stream.errorString();

    QByteArray buffer(1024, Qt::Uninitialized);
    QCOMPARE(stream.read(buffer.data(), buffer.size()), -1);
    QCOMPARE(stream.read(buffer.data(), buffer.size()), -1);
    QCOMPARE(stream.errorString(), said);
    QVERIFY(stream.error().isError());
}

void TestStreamingDownload::closingDuringABlockedReadDoesNotHang()
{
    // A drive ejected, or a preview closed, while a read is waiting on a server
    // that has stopped sending. The read has to come back -- as a failure, since
    // the file did not arrive -- and the close has to return rather than wait for
    // a transfer that is waiting for it.
    const QByteArray payload = payloadOf(200 * 1024);
    PacedServer server(payload, 8 * 1024);

    net::StreamingDownload stream(server.fetch(), payload.size(), payload.size());
    LetGo letGo { server };
    QVERIFY(stream.open(QIODevice::ReadOnly));

    server.allow(1);
    QCOMPARE(stream.read(1024).size(), 1024);
    server.waitUntilItIsWaitingForMore();

    qint64 result = 0;
    std::thread reader([&] {
        QByteArray buffer(128 * 1024, Qt::Uninitialized);
        result = stream.read(buffer.data(), buffer.size());
    });
    stream.close();
    reader.join();

    // One block was handed over and 1024 bytes of it read, so 7168 is all there
    // ever was. Whether the read comes back with those or with a failure depends
    // on which side of the close it was, and both are correct; inventing bytes
    // that never arrived is not, and neither is never coming back at all.
    QVERIFY2(result <= 7 * 1024, "a stream closed under a reader must not hand over bytes it never had");
}

void TestStreamingDownload::aStreamKeepsAliveTheDriveItWasOpenedOn()
{
    // The reproduction. Every remote read captures its backend -- it has to, the
    // bytes come from the server on demand -- and the only thing that used to
    // stop the stream outliving the drive was a comment saying "whoever opened it
    // is holding the drive it came from". That is a convention about callers: a
    // preview or a viewer keeping a device open across an unmount left the stream
    // as the only thing that had ever kept the backend alive, and the
    // use-after-free landed inside libcurl, a long way from the cause.
    //
    // Shaped exactly like the shipped code: the fetch holds the drive by raw
    // pointer, and keepAlive() is the only thing between it and freed memory.
    // Read under `make asan`, which is what turns this from a pass into a proof.
    // See MOLE-364.
    const QByteArray contents = payloadOf(24 * 1024);
    auto drive = std::make_shared<MemoryFileSystem>();
    drive->addFile(QStringLiteral("/big.bin"), contents);
    std::weak_ptr<IFileSystem> watch = drive;

    MemoryFileSystem* raw = drive.get();
    auto stream = std::make_unique<net::StreamingDownload>(
        [raw](QIODevice& sink, qint64 offset, qint64 span, const CancelToken&) {
            Result<std::unique_ptr<QIODevice>> source
                = raw->openRead(VfsUri::fromString(QStringLiteral("mem:///big.bin")));
            if (!source.ok())
                return source.error();
            if (!source.value()->seek(offset))
                return VfsError::make(VfsError::IoError, QStringLiteral("could not seek"));
            const QByteArray chunk = source.value()->read(span);
            if (sink.write(chunk) != chunk.size())
                return VfsError::make(VfsError::IoError, QStringLiteral("the reader went away"));
            return VfsError::ok();
        },
        contents.size(), 8 * 1024);
    stream->keepAlive(drive);
    QVERIFY(stream->open(QIODevice::ReadOnly));

    // Everything else lets go of it, which is what unmounting does.
    drive.reset();
    QVERIFY2(!watch.expired(), "the stream is not holding the drive up");

    QCOMPARE(stream->readAll(), contents);
    stream->close();
    stream.reset();
    QVERIFY2(watch.expired(), "the drive outlived the stream that was holding it");
}

void TestStreamingDownload::everyDeviceABackendHandsOutHoldsItsDrive()
{
    // The mechanism is only as good as its use, and a fifth backend written next
    // year that forgets keepAlive() is a use-after-free nothing would notice --
    // the shape of fault MOLE-364 was. So the rule is held here: a file under
    // plugins/network that builds a device has to name sharedSelf() at least as
    // many times as it builds one.
    static const QRegularExpression builds(QStringLiteral(
        "make_unique<(?:net::)?(?:StreamingDownload|StreamingUpload|BufferedUpload|SmbFile)>"));

    const QDir directory(QStringLiteral(MOLE_SHELL_SOURCE_DIR) + QStringLiteral("/plugins/network"));
    const QStringList sources = directory.entryList({ QStringLiteral("*.cpp") }, QDir::Files, QDir::Name);
    QVERIFY(!sources.isEmpty());

    QStringList offenders;
    int handedOut = 0;
    for (const QString& name : sources) {
        if (name == QLatin1String("TransferStreams.cpp"))
            continue; // where the mechanism lives, not a backend that uses it

        QFile source(directory.filePath(name));
        QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(source.fileName()));
        const QString text = QString::fromUtf8(source.readAll());

        int built = 0;
        QRegularExpressionMatchIterator each = builds.globalMatch(text);
        while (each.hasNext()) {
            each.next();
            ++built;
        }
        if (built == 0)
            continue;
        handedOut += built;
        const int held = static_cast<int>(text.count(QStringLiteral("sharedSelf()")));
        if (held < built) {
            offenders.append(QStringLiteral("%1 hands out %2 device(s) and holds its drive %3 time(s)")
                                 .arg(name)
                                 .arg(built)
                                 .arg(held));
        }
    }

    QVERIFY2(handedOut >= 8,
        qPrintable(
            QStringLiteral("only %1 device sites were found -- has the shape changed?").arg(handedOut)));
    QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QStringLiteral("; "))));
}

MOLE_TEST_MAIN(TestStreamingDownload)

#include "tst_StreamingDownload.moc"
