#include "plugins/network/TransferStreams.h"
#include "support/MoleTestMain.h"

#include <QTest>

#include <atomic>

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

QByteArray payloadOf(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 7 + (i >> 8)) & 0xff);
    return data;
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
    void seekingBackwardsStartsAgain();
    void abandoningTheStreamDoesNotHang();
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

MOLE_TEST_MAIN(TestStreamingDownload)

#include "tst_StreamingDownload.moc"
