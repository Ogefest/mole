#include "plugins/network/TransferStreams.h"
#include "support/MoleTestMain.h"

#include "core/vfs/backends/MemoryFileSystem.h"

#include <QBuffer>
#include <QDir>
#include <QTemporaryDir>
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
            if (m_refuse || m_spans == m_refuseSpan)
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
    /// Refuses one particular span and accepts the rest, so a failure can be
    /// put at the start of a file or at its end.
    void refuseSpan(int number) { m_refuseSpan = number; }
    QByteArray received() const { return m_received; }
    int spans() const { return m_spans; }

private:
    QByteArray m_received;
    std::atomic<int> m_spans { 0 };
    bool m_refuse = false;
    int m_refuseSpan = 0;
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

    void closingAStreamWithNothingWrittenStillCreatesTheFile();
    void closingABufferedUploadWithNothingWrittenStillCreatesTheFile();
    void exactlyOneSpanAndOneSpanAndAByte_data();
    void exactlyOneSpanAndOneSpanAndAByte();
    void aSendThatFailsIsReportedWhicheverSpanItWas_data();
    void aSendThatFailsIsReportedWhicheverSpanItWas();
    void aBufferedUploadDestroyedWithoutBeingClosedSendsNothing();
    void aStagingFileThatCannotBeOpenedIsRefusedRatherThanLost();

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

void TestStreamingUpload::closingAStreamWithNothingWrittenStillCreatesTheFile()
{
    // Nothing to send is not nothing to do. A file somebody asked for that never
    // appears because it happened to be empty is a missing file, and every
    // backend here can hold one.
    FakeServer server;
    FakeCommit commit;

    net::StreamingUpload stream(server.send(), 128 * 1024, commit.hook());
    QVERIFY(stream.open(QIODevice::WriteOnly));
    stream.close();

    QVERIFY(!stream.commitError().isError());
    QCOMPARE(server.spans(), 1);
    QCOMPARE(server.received(), QByteArray());
    QCOMPARE(commit.calls(), 1);
}

void TestStreamingUpload::closingABufferedUploadWithNothingWrittenStillCreatesTheFile()
{
    FakeServer server;
    FakeCommit commit;

    net::BufferedUpload stream(server.sink(), [&commit] { return commit.hook()(); });
    QVERIFY(stream.open(QIODevice::WriteOnly));
    stream.close();

    QVERIFY(!stream.commitError().isError());
    QCOMPARE(server.spans(), 1);
    QCOMPARE(server.received(), QByteArray());
    QCOMPARE(commit.calls(), 1);
}

void TestStreamingUpload::exactlyOneSpanAndOneSpanAndAByte_data()
{
    QTest::addColumn<int>("size");

    // The boundary that decides whether a second transfer happens at all.
    const int span = 128 * 1024;
    QTest::newRow("a span less one") << span - 1;
    QTest::newRow("exactly one span") << span;
    QTest::newRow("one span and a byte") << span + 1;
}

void TestStreamingUpload::exactlyOneSpanAndOneSpanAndAByte()
{
    QFETCH(int, size);

    const QByteArray payload = payloadOf(size);
    FakeServer server;
    FakeCommit commit;

    net::StreamingUpload stream(server.send(), 128 * 1024, commit.hook());
    QVERIFY(stream.open(QIODevice::WriteOnly));
    QCOMPARE(stream.write(payload), static_cast<qint64>(payload.size()));
    stream.close();

    QVERIFY(!stream.commitError().isError());
    QCOMPARE(server.received(), payload);
    QCOMPARE(commit.calls(), 1);
    // A file that fills a span exactly still costs a second transfer, because
    // the only way to learn that it ended there is to offer the next span and
    // be given nothing. That is a cost, not a fault -- what would be a fault is
    // a byte landing in neither transfer.
    QVERIFY2(server.spans() >= 1, "the payload has to be sent by somebody");
}

void TestStreamingUpload::aSendThatFailsIsReportedWhicheverSpanItWas_data()
{
    QTest::addColumn<int>("failingSpan");
    QTest::newRow("the first span") << 1;
    QTest::newRow("the last span") << 3;
}

void TestStreamingUpload::aSendThatFailsIsReportedWhicheverSpanItWas()
{
    QFETCH(int, failingSpan);

    // A connection that dies at the start of a large upload and one that dies at
    // the end are the same outcome from up here: the file is not there, and
    // nothing may be put in place under the name somebody asked for.
    const QByteArray payload = payloadOf(300 * 1024);
    FakeServer server;
    FakeCommit commit;
    server.refuseSpan(failingSpan);

    net::StreamingUpload stream(server.send(), 128 * 1024, commit.hook());
    QVERIFY(stream.open(QIODevice::WriteOnly));
    for (int at = 0; at < payload.size(); at += 32 * 1024)
        stream.write(payload.mid(at, 32 * 1024));
    stream.close();

    QVERIFY(stream.commitError().isError());
    QVERIFY2(stream.errorString().contains(QStringLiteral("hung up")), qPrintable(stream.errorString()));
    QCOMPARE(commit.calls(), 0);
}

void TestStreamingUpload::aBufferedUploadDestroyedWithoutBeingClosedSendsNothing()
{
    // The staged path's version of an abandoned copy. Nothing has left the
    // machine yet, so nothing should: sending a truncated payload from a
    // destructor is silent corruption, and worse than sending nothing at all.
    FakeServer server;
    FakeCommit commit;

    {
        net::BufferedUpload stream(server.sink(), [&commit] { return commit.hook()(); });
        QVERIFY(stream.open(QIODevice::WriteOnly));
        stream.write(payloadOf(64 * 1024));
    }

    QCOMPARE(server.spans(), 0);
    QCOMPARE(server.received(), QByteArray());
    QCOMPARE(commit.calls(), 0);
}

void TestStreamingUpload::aStagingFileThatCannotBeOpenedIsRefusedRatherThanLost()
{
    // Everything below the streaming threshold is staged in a temporary file,
    // and a machine whose temporary directory is missing or full is a machine
    // where that fails. Opening has to say so: a stream that reports itself open
    // and then swallows every write is how an upload disappears without an
    // error anywhere.
    //
    // **What refuses is our own check, not Qt's.** QTemporaryFile is happy to put
    // its file wherever a missing temporary directory leaves it -- the filesystem
    // root, on the Qt build this was found on -- which succeeds for any account
    // that can write there. So opening goes through staging::openFile(), which
    // asks about the directory first, and this case fails the moment that is
    // removed. See MOLE-297 and MOLE-304.
    // A directory this case owns and takes away, rather than a TMPDIR pointed at
    // something that does not exist: what refuses then is Mole, on any account,
    // and not the platform declining to write to somewhere it cannot reach.
    QTemporaryDir own;
    QVERIFY(own.isValid());
    const QString gone = QDir(own.path()).filePath(QStringLiteral("taken-away"));
    const QByteArray previous = qgetenv("MOLE_STAGING_DIR");
    qputenv("MOLE_STAGING_DIR", gone.toUtf8());

    FakeServer server;
    FakeCommit commit;
    net::BufferedUpload stream(server.sink(), [&commit] { return commit.hook()(); });
    const bool opened = stream.open(QIODevice::WriteOnly);

    if (previous.isEmpty())
        qunsetenv("MOLE_STAGING_DIR");
    else
        qputenv("MOLE_STAGING_DIR", previous);

    QVERIFY2(!opened, "an upload with nowhere to stage itself has to refuse to open");
    QVERIFY2(stream.errorString().contains(QStringLiteral("temporary")), qPrintable(stream.errorString()));
    QCOMPARE(server.spans(), 0);
    QCOMPARE(commit.calls(), 0);
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
