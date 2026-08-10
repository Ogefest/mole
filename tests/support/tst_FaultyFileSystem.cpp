#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/backends/MemoryFileSystem.h"

#include <atomic>
#include <thread>

using namespace mole;
using namespace mole::test;

namespace {

/// What came out of a stream, and whether it ended or broke.
struct ReadOutcome
{
    QByteArray data;
    bool failed = false;
    QString error;
};

/// Reads until the end or the failure, in chunks of `chunkSize` -- the size
/// matters, because a fault that fires on a chunk boundary rather than on
/// its own byte would pass every test that reads the whole file at once.
ReadOutcome drain(QIODevice& device, qint64 chunkSize)
{
    ReadOutcome outcome;
    QByteArray buffer(chunkSize, Qt::Uninitialized);
    for (;;) {
        const qint64 got = device.read(buffer.data(), chunkSize);
        if (got < 0) {
            outcome.failed = true;
            outcome.error = device.errorString();
            break;
        }
        if (got == 0)
            break;
        outcome.data.append(buffer.constData(), got);
    }
    return outcome;
}

VfsUri uri(const char* path)
{
    return VfsUri::fromString(QLatin1String("mem://") + QLatin1String(path));
}

} // namespace

/// The fault injector is itself a test tool, and one that miscounts bytes would
/// make every suite built on top of it green for the wrong reason. Each fault is
/// checked here against the one thing it promises: that it happens at the byte
/// it was given.
class TestFaultyFileSystem : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void readFailsAfterExactlyTheBytesItPromised();
    void theOffsetHoldsWhateverChunkSizeIsUsed();
    void readGoesShortOnceAndThenRecovers();
    void readStallsUntilItIsReleased();

    void writeKeepsOnlyEveryNthByte();
    void aWriteThatFailsOnCloseStoresNothing();
    void theDestinationFillsAtTheOffset();

    void theFileChangesSizeUnderTheReader();
    void theFileVanishesUnderTheReader();
    void theFileIsRenamedUnderTheReader();
    void accessIsRevokedPartWayThrough();

    void aListingThatLiesAboutASize();
    void everyStreamGetsTheWholeFault();
    void aFaultCanBeLimitedToOnePath();

private:
    std::shared_ptr<MemoryFileSystem> m_mem;
    std::shared_ptr<FaultyFileSystem> m_faulty;
};

void TestFaultyFileSystem::init()
{
    m_mem = std::make_shared<MemoryFileSystem>();
    m_mem->addFile(QStringLiteral("/data/file.bin"), QByteArray(1000, 'a'));
    m_mem->addDirectory(QStringLiteral("/out"));
    m_faulty = std::make_shared<FaultyFileSystem>(m_mem);
}

void TestFaultyFileSystem::readFailsAfterExactlyTheBytesItPromised()
{
    m_faulty->readFailsAt(120);

    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
    QVERIFY2(stream.ok(), qPrintable(stream.error().message));

    const ReadOutcome outcome = drain(*stream.value(), 256);
    QCOMPARE(outcome.data.size(), 120);
    QVERIFY(outcome.failed);
    QVERIFY2(outcome.error.contains(QStringLiteral("the connection went away")), qPrintable(outcome.error));
}

void TestFaultyFileSystem::theOffsetHoldsWhateverChunkSizeIsUsed()
{
    m_faulty->readFailsAt(300);

    // 7 bytes at a time and the whole file at once must give the same answer:
    // the fault belongs to the file, not to the caller's buffer size.
    for (const qint64 chunk : { qint64(7), qint64(256), qint64(4096) }) {
        Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
        QVERIFY(stream.ok());
        const ReadOutcome outcome = drain(*stream.value(), chunk);
        QVERIFY(outcome.failed);
        QCOMPARE(outcome.data.size(), 300);
    }
}

void TestFaultyFileSystem::readGoesShortOnceAndThenRecovers()
{
    m_faulty->readGoesShortAt(100, 3);

    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
    QVERIFY(stream.ok());
    QIODevice& device = *stream.value();

    QByteArray buffer(256, Qt::Uninitialized);
    QCOMPARE(device.read(buffer.data(), 256), 100); // stops on the offset
    QCOMPARE(device.read(buffer.data(), 256), 3); // the short one
    QCOMPARE(device.read(buffer.data(), 256), 256); // and then it is a stream again

    // A short read is not the end of the file: everything still arrives.
    const ReadOutcome rest = drain(device, 256);
    QVERIFY(!rest.failed);
    QCOMPARE(rest.data.size() + 100 + 3 + 256, 1000);
}

void TestFaultyFileSystem::readStallsUntilItIsReleased()
{
    m_faulty->readStallsAt(500);

    std::atomic_bool finished { false };
    ReadOutcome outcome;
    // Opened and read on another thread, the way a task reads: the point of a
    // stall is that the thread doing the work is the one that is stopped.
    std::thread reader([&] {
        Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
        if (stream.ok())
            outcome = drain(*stream.value(), 256);
        finished.store(true);
    });

    QVERIFY(waitFor([this] { return m_faulty->isStalled(); }));
    QVERIFY2(!finished.load(), "the read went past the offset it was told to stop at");

    m_faulty->release();
    reader.join();

    QVERIFY(!outcome.failed);
    QCOMPARE(outcome.data.size(), 1000);
    QVERIFY(!m_faulty->isStalled());
}

void TestFaultyFileSystem::writeKeepsOnlyEveryNthByte()
{
    m_faulty->writeKeepsEveryNth(4);

    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openWrite(uri("/out/copy.bin"));
    QVERIFY2(stream.ok(), qPrintable(stream.error().message));

    const QByteArray payload(4000, 'x');
    QCOMPARE(stream.value()->write(payload), 4000); // the lie: everything was accepted
    QVERIFY(closeAndReport(*stream.value()).ok()); // and nothing complained

    const Result<FileEntry> landed = m_mem->stat(uri("/out/copy.bin"));
    QVERIFY(landed.ok());
    QCOMPARE(landed.value().size, 1000);
}

void TestFaultyFileSystem::aWriteThatFailsOnCloseStoresNothing()
{
    m_faulty->writeFailsOnClose();

    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openWrite(uri("/out/copy.bin"));
    QVERIFY(stream.ok());
    QCOMPARE(stream.value()->write(QByteArray(500, 'x')), 500);

    const Result<void> committed = closeAndReport(*stream.value());
    QVERIFY(!committed.ok());
    QCOMPARE(committed.error().code, VfsError::NetworkError);

    // Nothing landed, which is what a commit that failed means -- a half file
    // left behind would be a different fault with a different name.
    QVERIFY(!m_mem->stat(uri("/out/copy.bin")).ok());
}

void TestFaultyFileSystem::theDestinationFillsAtTheOffset()
{
    m_faulty->destinationFillsAt(600);

    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openWrite(uri("/out/copy.bin"));
    QVERIFY(stream.ok());

    // The bytes before the offset really are written: a disk that fills up keeps
    // what it already took, and says it took less than it was given.
    QCOMPARE(stream.value()->write(QByteArray(1000, 'x')), 600);
    QVERIFY2(stream.value()->errorString().contains(QStringLiteral("no space left")),
        qPrintable(stream.value()->errorString()));
    QVERIFY(closeAndReport(*stream.value()).ok());

    const Result<FileEntry> landed = m_mem->stat(uri("/out/copy.bin"));
    QVERIFY(landed.ok());
    QCOMPARE(landed.value().size, 600);
}

void TestFaultyFileSystem::theFileChangesSizeUnderTheReader()
{
    m_faulty->fileChangesSizeAt(300, 400);

    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
    QVERIFY(stream.ok());

    // The read ends early and says nothing about it, which is the fault: an
    // early end and a smaller file are the same event seen from here.
    const ReadOutcome outcome = drain(*stream.value(), 256);
    QVERIFY(!outcome.failed);
    QCOMPARE(outcome.data.size(), 400);

    const Result<FileEntry> now = m_mem->stat(uri("/data/file.bin"));
    QVERIFY(now.ok());
    QCOMPARE(now.value().size, 400);
}

void TestFaultyFileSystem::theFileVanishesUnderTheReader()
{
    m_faulty->fileVanishesAt(250);

    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
    QVERIFY(stream.ok());

    const ReadOutcome outcome = drain(*stream.value(), 64);
    QCOMPARE(outcome.data.size(), 250);
    QVERIFY(outcome.failed);
    QVERIFY2(outcome.error.contains(QStringLiteral("went away")), qPrintable(outcome.error));
    QVERIFY(!m_mem->stat(uri("/data/file.bin")).ok());
}

void TestFaultyFileSystem::theFileIsRenamedUnderTheReader()
{
    m_faulty->fileIsRenamedAt(250, QStringLiteral("elsewhere.bin"));

    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
    QVERIFY(stream.ok());

    const ReadOutcome outcome = drain(*stream.value(), 64);
    QCOMPARE(outcome.data.size(), 250);
    QVERIFY(outcome.failed);
    QVERIFY(!m_mem->stat(uri("/data/file.bin")).ok());
    QVERIFY(m_mem->stat(uri("/data/elsewhere.bin")).ok());
}

void TestFaultyFileSystem::accessIsRevokedPartWayThrough()
{
    m_faulty->accessRevokedAt(250);

    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
    QVERIFY(stream.ok());

    const ReadOutcome outcome = drain(*stream.value(), 64);
    QCOMPARE(outcome.data.size(), 250);
    QVERIFY(outcome.failed);

    // The drive itself is closed from then on, not just that one stream.
    const Result<FileEntryList> listing = m_faulty->list(uri("/data"), CancelToken());
    QVERIFY(!listing.ok());
    QCOMPARE(listing.error().code, VfsError::AccessDenied);
    QVERIFY(!m_faulty->stat(uri("/data/file.bin")).ok());
    QVERIFY(!m_faulty->openWrite(uri("/out/copy.bin")).ok());

    // ... while the drive underneath is untouched, so a test can still look.
    QVERIFY(m_mem->stat(uri("/data/file.bin")).ok());
}

void TestFaultyFileSystem::aListingThatLiesAboutASize()
{
    m_faulty->listingOverstatesSizeBy(500);

    const Result<FileEntryList> listing = m_faulty->list(uri("/data"), CancelToken());
    QVERIFY(listing.ok());
    QCOMPARE(listing.value().size(), 1);
    QCOMPARE(listing.value().first().size, 1500);
    QCOMPARE(m_faulty->stat(uri("/data/file.bin")).value().size, 1500);

    // The claim is all that changes. The bytes are still the bytes.
    Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
    QVERIFY(stream.ok());
    QCOMPARE(drain(*stream.value(), 256).data.size(), 1000);
}

void TestFaultyFileSystem::everyStreamGetsTheWholeFault()
{
    m_faulty->readFailsAt(120);

    // Ten concurrent copies of one file should each drop at 120 bytes. A fault
    // consumed by whichever stream got there first would quietly turn nine of
    // them into successes.
    for (int i = 0; i < 3; ++i) {
        Result<std::unique_ptr<QIODevice>> stream = m_faulty->openRead(uri("/data/file.bin"));
        QVERIFY(stream.ok());
        const ReadOutcome outcome = drain(*stream.value(), 256);
        QVERIFY(outcome.failed);
        QCOMPARE(outcome.data.size(), 120);
    }
}

void TestFaultyFileSystem::aFaultCanBeLimitedToOnePath()
{
    m_mem->addFile(QStringLiteral("/data/other.bin"), QByteArray(1000, 'b'));
    m_faulty->readFailsAt(120, QStringLiteral("/data/file.bin"));

    Result<std::unique_ptr<QIODevice>> broken = m_faulty->openRead(uri("/data/file.bin"));
    QVERIFY(broken.ok());
    QVERIFY(drain(*broken.value(), 256).failed);

    Result<std::unique_ptr<QIODevice>> fine = m_faulty->openRead(uri("/data/other.bin"));
    QVERIFY(fine.ok());
    const ReadOutcome outcome = drain(*fine.value(), 256);
    QVERIFY(!outcome.failed);
    QCOMPARE(outcome.data.size(), 1000);
}

MOLE_TEST_MAIN(TestFaultyFileSystem)
#include "tst_FaultyFileSystem.moc"
