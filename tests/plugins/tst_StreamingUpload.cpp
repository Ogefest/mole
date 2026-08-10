#include "plugins/network/TransferStreams.h"
#include "support/MoleTestMain.h"

#include "core/vfs/backends/MemoryFileSystem.h"

#include <QBuffer>
#include <QTest>

#include <atomic>

using namespace mole;

namespace {

QByteArray payloadOf(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 11 + (i >> 7)) & 0xff);
    return data;
}

/// A server that collects what it is sent, and can be told to refuse.
class FakeServer
{
public:
    /// Accepts spans the way a real one does: each appends to what the last
    /// left, and a short one is the end of the file.
    net::StreamingUpload::Send send()
    {
        return [this](QIODevice& source, qint64 span, bool append, const CancelToken& cancel) {
            ++m_spans;
            if (!append)
                m_received.clear();
            if (m_refuse)
                return VfsError::make(VfsError::NetworkError, QStringLiteral("the server hung up"));

            QByteArray buffer(16 * 1024, Qt::Uninitialized);
            qint64 taken = 0;
            while (taken < span) {
                if (cancel.isCancelled())
                    return VfsError::make(VfsError::Cancelled, QStringLiteral("cancelled"));
                const qint64 got = source.read(buffer.data(), std::min<qint64>(buffer.size(), span - taken));
                if (got < 0)
                    return VfsError::make(VfsError::IoError, QStringLiteral("the writer went away"));
                if (got == 0)
                    break;
                m_received.append(buffer.constData(), static_cast<int>(got));
                taken += got;
            }
            return VfsError::ok();
        };
    }

    net::BufferedUpload::Sink sink()
    {
        return [this](QIODevice& payload, qint64 size) -> Result<void> {
            ++m_spans;
            if (m_refuse)
                return Result<void>::failure(VfsError::NetworkError, QStringLiteral("the server hung up"));
            m_received = payload.read(size);
            return {};
        };
    }

    void refuse() { m_refuse = true; }
    QByteArray received() const { return m_received; }
    int spans() const { return m_spans; }

private:
    QByteArray m_received;
    std::atomic<int> m_spans { 0 };
    bool m_refuse = false;
};

/// Stands in for the step that puts a finished upload under its real name.
class FakeCommit
{
public:
    net::StreamingUpload::Commit hook()
    {
        return [this] {
            ++m_calls;
            return m_fails ? VfsError::make(VfsError::AlreadyExists, QStringLiteral("something is there"))
                           : VfsError::ok();
        };
    }

    void fail() { m_fails = true; }
    int calls() const { return m_calls; }

private:
    std::atomic<int> m_calls { 0 };
    bool m_fails = false;
};

} // namespace

/// The write streams, and the step that gives an upload its real name only once
/// it has all arrived.
class TestStreamingUpload : public QObject
{
    Q_OBJECT

private slots:
    void aWorkingNameIsRecognisableAndReversible();

    void streamingCommitsOnceEverythingIsSent();
    void streamingDoesNotCommitAFailedSend();
    void streamingDoesNotCommitAnAbandonedStream();
    void streamingReportsACommitThatFailed();

    void bufferedCommitsOnceThePayloadIsSent();
    void bufferedDoesNotCommitAFailedSend();
    void bufferedReportsACommitThatFailed();

    void commitRenamesTheWorkingNameIntoPlace();
    void commitRefusesToReplaceSomethingThatAppeared();
    void commitLeavesNothingBehindWhenTheRenameFails();
};

void TestStreamingUpload::aWorkingNameIsRecognisableAndReversible()
{
    const VfsUri target = VfsUri::fromString(QStringLiteral("sftp://nas/photos/report.pdf"));
    const VfsUri staging = partialWriteOf(target);

    QCOMPARE(staging.path(), QStringLiteral("/photos/report.pdf.mole-partial"));
    QCOMPARE(staging.scheme(), target.scheme());
    QCOMPARE(staging.authority(), target.authority());
    QCOMPARE(staging.parent(), target.parent());

    // The whole mechanism is that anybody looking at a listing can tell. That
    // includes a sweep for what a killed process left behind, which is a filter
    // over names and nothing else.
    QVERIFY(isPartialWrite(staging.fileName()));
    QVERIFY(!isPartialWrite(target.fileName()));
    QVERIFY(!isPartialWrite(QStringLiteral("mole-partial.txt")));
}

void TestStreamingUpload::streamingCommitsOnceEverythingIsSent()
{
    const QByteArray payload = payloadOf(300 * 1024);
    FakeServer server;
    FakeCommit commit;

    net::StreamingUpload stream(server.send(), 128 * 1024, commit.hook());
    QVERIFY(stream.open(QIODevice::WriteOnly));

    for (int at = 0; at < payload.size(); at += 40 * 1024) {
        const QByteArray chunk = payload.mid(at, 40 * 1024);
        QCOMPARE(stream.write(chunk), static_cast<qint64>(chunk.size()));
        // Nothing is put in place while bytes are still arriving, however many
        // spans have already gone up.
        QCOMPARE(commit.calls(), 0);
    }

    stream.close();

    QVERIFY(!stream.commitError().isError());
    QCOMPARE(server.received(), payload);
    // Once, at the end -- not once per span.
    QCOMPARE(commit.calls(), 1);
}

void TestStreamingUpload::streamingDoesNotCommitAFailedSend()
{
    FakeServer server;
    server.refuse();
    FakeCommit commit;

    net::StreamingUpload stream(server.send(), 128 * 1024, commit.hook());
    QVERIFY(stream.open(QIODevice::WriteOnly));
    // The write may be accepted or refused depending on how far the sending
    // thread has got; what matters is what close() reports.
    stream.write(payloadOf(64 * 1024));
    stream.close();

    QVERIFY(stream.commitError().isError());
    QCOMPARE(stream.commitError().code, VfsError::NetworkError);
    // The name stays free: a failed transfer must not end up looking finished.
    QCOMPARE(commit.calls(), 0);
}

void TestStreamingUpload::streamingDoesNotCommitAnAbandonedStream()
{
    FakeServer server;
    FakeCommit commit;

    {
        // Destroyed without being closed, which is what a cancelled copy leaves
        // behind. Part of a file has gone up under the working name; giving it
        // the real name would be exactly the fault this exists to prevent.
        net::StreamingUpload stream(server.send(), 128 * 1024, commit.hook());
        QVERIFY(stream.open(QIODevice::WriteOnly));
        stream.write(payloadOf(64 * 1024));
    }

    QCOMPARE(commit.calls(), 0);
}

void TestStreamingUpload::streamingReportsACommitThatFailed()
{
    const QByteArray payload = payloadOf(64 * 1024);
    FakeServer server;
    FakeCommit commit;
    commit.fail();

    net::StreamingUpload stream(server.send(), 128 * 1024, commit.hook());
    QVERIFY(stream.open(QIODevice::WriteOnly));
    QCOMPARE(stream.write(payload), static_cast<qint64>(payload.size()));
    stream.close();

    // Every byte arrived and the file is still not there. A caller told the
    // upload succeeded would be told a lie.
    QCOMPARE(server.received(), payload);
    QCOMPARE(commit.calls(), 1);
    QCOMPARE(stream.commitError().code, VfsError::AlreadyExists);
    QVERIFY(stream.errorString().contains(QStringLiteral("something is there")));
}

void TestStreamingUpload::bufferedCommitsOnceThePayloadIsSent()
{
    const QByteArray payload = payloadOf(70 * 1024);
    FakeServer server;
    FakeCommit commit;

    net::BufferedUpload stream(server.sink(), commit.hook());
    QVERIFY(stream.open(QIODevice::WriteOnly));
    QCOMPARE(stream.write(payload), static_cast<qint64>(payload.size()));
    QCOMPARE(commit.calls(), 0);

    stream.close();

    QVERIFY(!stream.commitError().isError());
    QCOMPARE(server.received(), payload);
    QCOMPARE(commit.calls(), 1);
}

void TestStreamingUpload::bufferedDoesNotCommitAFailedSend()
{
    FakeServer server;
    server.refuse();
    FakeCommit commit;

    net::BufferedUpload stream(server.sink(), commit.hook());
    QVERIFY(stream.open(QIODevice::WriteOnly));
    stream.write(payloadOf(8 * 1024));
    stream.close();

    QCOMPARE(stream.commitError().code, VfsError::NetworkError);
    QCOMPARE(commit.calls(), 0);
}

void TestStreamingUpload::bufferedReportsACommitThatFailed()
{
    FakeServer server;
    FakeCommit commit;
    commit.fail();

    net::BufferedUpload stream(server.sink(), commit.hook());
    QVERIFY(stream.open(QIODevice::WriteOnly));
    stream.write(payloadOf(8 * 1024));
    stream.close();

    QCOMPARE(commit.calls(), 1);
    QCOMPARE(stream.commitError().code, VfsError::AlreadyExists);
}

void TestStreamingUpload::commitRenamesTheWorkingNameIntoPlace()
{
    auto fs = std::make_shared<MemoryFileSystem>();
    const VfsUri target = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));
    const VfsUri staging = partialWriteOf(target);
    fs->addFile(staging.path(), QByteArrayLiteral("the whole file"));

    const VfsError failed = commitPartialWrite(*fs, staging, target);

    QVERIFY(!failed.isError());
    QVERIFY(fs->stat(target).ok());
    QVERIFY(!fs->stat(staging).ok());
}

void TestStreamingUpload::commitRefusesToReplaceSomethingThatAppeared()
{
    auto fs = std::make_shared<MemoryFileSystem>();
    const VfsUri target = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));
    const VfsUri staging = partialWriteOf(target);
    fs->addFile(staging.path(), QByteArrayLiteral("what was uploaded"));
    // Somebody else got there during the minutes the upload took.
    fs->addFile(target.path(), QByteArrayLiteral("what somebody else put there"));

    const VfsError failed = commitPartialWrite(*fs, staging, target);

    QCOMPARE(failed.code, VfsError::AlreadyExists);
    // Theirs, untouched -- the whole reason for checking rather than assuming.
    const Result<std::unique_ptr<QIODevice>> file = fs->openRead(target);
    QVERIFY(file.ok());
    QCOMPARE(file.value()->readAll(), QByteArrayLiteral("what somebody else put there"));
    // And nothing of ours left lying about.
    QVERIFY(!fs->stat(staging).ok());
}

void TestStreamingUpload::commitLeavesNothingBehindWhenTheRenameFails()
{
    auto fs = std::make_shared<MemoryFileSystem>();
    const VfsUri target = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));
    const VfsUri staging = partialWriteOf(target);
    fs->addFile(staging.path(), QByteArrayLiteral("the whole file"));
    fs->setFault(target.path(), VfsError::AccessDenied);

    const VfsError failed = commitPartialWrite(*fs, staging, target);

    QCOMPARE(failed.code, VfsError::AccessDenied);
    // The transfer failed, so it leaves nothing: bytes under a name nothing
    // will ever open are litter rather than a result.
    fs->clearFaults();
    QVERIFY(!fs->stat(staging).ok());
    QVERIFY(!fs->stat(target).ok());
}

MOLE_TEST_MAIN(TestStreamingUpload)
#include "tst_StreamingUpload.moc"
