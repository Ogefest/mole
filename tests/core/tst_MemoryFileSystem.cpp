#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/backends/MemoryFileSystem.h"

#include <QElapsedTimer>

#include <thread>

using namespace mole;
using namespace mole::test;

class TestMemoryFileSystem : public QObject
{
    Q_OBJECT

private slots:
    /// The in-memory backend is held to exactly the same contract as the disk.
    void conformance();

    void injectedFaultsSurface();
    void clearingFaultsRestoresAccess();
    void conformanceOnACaseInsensitiveVolume();
    void renameMovesWholeSubtree();
    void listDelayIsInterruptibleByCancel();
};

void TestMemoryFileSystem::conformance()
{
    auto fs = std::make_shared<MemoryFileSystem>();

    ConformanceContext context;
    context.fileSystem = fs;
    context.root = VfsUri::fromString(QStringLiteral("mem:///"));
    context.seedFile = [fs](const QString& path, const QByteArray& data) {
        fs->addFile(QLatin1Char('/') + path, data);
        return true;
    };
    context.seedDir = [fs](const QString& path) {
        fs->addDirectory(QLatin1Char('/') + path);
        return true;
    };
    // A scratch drive has no permissions, so the refusal is injected. It is the
    // same claim either way: the backend that cannot read a directory says so
    // rather than answering with an empty one.
    context.whileUnlistable = [fs](const QString& path, const std::function<void()>& check) {
        fs->setFault(QLatin1Char('/') + path, VfsError::AccessDenied);
        check();
        fs->clearFaults();
        return true;
    };

    // The in-memory drive writes straight into its map: there is no working
    // name and so no moment at which it could tell an overwrite from a file that
    // arrived. See ConformanceContext::stagesWrites and TODO.md.
    context.stagesWrites = false;

    // **Held part way, which is what makes the mid-operation cancel real.** The
    // suite could only ever offer a token that was already cancelled, and every
    // backend answers that before it starts -- so nothing held one to noticing a
    // token cancelled *while it is working*, which is the only cancel a user can
    // produce. The drive's list gate is a condition rather than a duration: the
    // listing is definitely inside the call when the count says so. See
    // ADR-0096 and MOLE-401.
    context.whileHeldPartWay = [fs](const char* what,
                                   const std::function<void(const CancelToken&)>& operation,
                                   const std::function<void()>& whenHeld) {
        // Two of the calls can be held, because two of them have a gate: the
        // listing and the read. remove(), rename() and openWrite() finish in
        // memory with nothing to wait on, so there is no moment to be "inside"
        // them -- the suite is told so rather than passed.
        const QLatin1String call(what);
        const bool listing = call == QLatin1String("list");
        const bool reading = call == QLatin1String("openRead");
        if (!listing && !reading)
            return false;

        auto gate = std::make_shared<QSemaphore>(0);
        if (listing)
            fs->setListGate(gate);
        else
            fs->setReadGate(gate);

        CancelToken token;
        std::thread worker([&operation, &token] { operation(token); });

        // Wait for the call to be *in* the drive, then cancel and let it go.
        const bool held = mole::test::waitFor(
            [fs, listing] { return (listing ? fs->listsInProgress() : fs->readsInProgress()) > 0; }, 5000);
        whenHeld();
        token.cancel();
        gate->release(4);
        worker.join();
        if (listing)
            fs->setListGate(nullptr);
        else
            fs->setReadGate(nullptr);
        return held;
    };

    runFileSystemConformance(context);
}

void TestMemoryFileSystem::injectedFaultsSurface()
{
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->addFile(QStringLiteral("/data/report.txt"), QByteArray("x"));
    fs->setFault(QStringLiteral("/data"), VfsError::NetworkError);

    Result<FileEntryList> listing
        = fs->list(VfsUri::fromString(QStringLiteral("mem:///data")), CancelToken());
    QVERIFY(!listing.ok());
    QCOMPARE(listing.error().code, VfsError::NetworkError);

    // Siblings stay reachable -- a fault is scoped to one path.
    QVERIFY(fs->list(VfsUri::fromString(QStringLiteral("mem:///")), CancelToken()).ok());
}

void TestMemoryFileSystem::clearingFaultsRestoresAccess()
{
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->addDirectory(QStringLiteral("/a"));
    fs->setFault(QStringLiteral("/a"), VfsError::AccessDenied);
    QVERIFY(!fs->list(VfsUri::fromString(QStringLiteral("mem:///a")), CancelToken()).ok());

    fs->clearFaults();
    QVERIFY(fs->list(VfsUri::fromString(QStringLiteral("mem:///a")), CancelToken()).ok());
}

void TestMemoryFileSystem::conformanceOnACaseInsensitiveVolume()
{
    // The same contract, held against a volume that does not distinguish case --
    // an NTFS one, or an APFS one with the default settings. Nobody here has one,
    // and every rule that only bites on such a volume would otherwise be checked
    // on the days somebody does.
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->setCaseSensitivity(Qt::CaseInsensitive);

    ConformanceContext context;
    context.fileSystem = fs;
    context.root = VfsUri::fromString(QStringLiteral("mem:///"));
    context.seedFile = [fs](const QString& path, const QByteArray& data) {
        fs->addFile(QLatin1Char('/') + path, data);
        return true;
    };
    context.seedDir = [fs](const QString& path) {
        fs->addDirectory(QLatin1Char('/') + path);
        return true;
    };
    // A scratch drive has no permissions, so the refusal is injected. It is the
    // same claim either way: the backend that cannot read a directory says so
    // rather than answering with an empty one.
    context.whileUnlistable = [fs](const QString& path, const std::function<void()>& check) {
        fs->setFault(QLatin1Char('/') + path, VfsError::AccessDenied);
        check();
        fs->clearFaults();
        return true;
    };

    // The in-memory drive writes straight into its map: there is no working
    // name and so no moment at which it could tell an overwrite from a file that
    // arrived. See ConformanceContext::stagesWrites and TODO.md.
    context.stagesWrites = false;

    runFileSystemConformance(context);
}

void TestMemoryFileSystem::renameMovesWholeSubtree()
{
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->addFile(QStringLiteral("/old/deep/file.txt"), QByteArray("payload"));

    Result<void> renamed = fs->rename(
        VfsUri::fromString(QStringLiteral("mem:///old")), VfsUri::fromString(QStringLiteral("mem:///new")));
    QVERIFY2(renamed.ok(), qPrintable(renamed.error().message));

    QVERIFY(!fs->stat(VfsUri::fromString(QStringLiteral("mem:///old"))).ok());

    Result<FileEntry> moved = fs->stat(VfsUri::fromString(QStringLiteral("mem:///new/deep/file.txt")));
    QVERIFY2(moved.ok(), "children must move with their parent");
    QCOMPARE(moved.value().size, 7);
}

void TestMemoryFileSystem::listDelayIsInterruptibleByCancel()
{
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->addDirectory(QStringLiteral("/slow"));
    fs->setListDelayMs(5000);

    CancelToken token;
    token.cancel();

    QElapsedTimer timer;
    timer.start();
    Result<FileEntryList> listing = fs->list(VfsUri::fromString(QStringLiteral("mem:///slow")), token);

    QVERIFY(!listing.ok());
    QCOMPARE(listing.error().code, VfsError::Cancelled);
    QVERIFY2(timer.elapsed() < 1000, "a cancelled listing must not wait out the full delay");
}

MOLE_TEST_MAIN(TestMemoryFileSystem)
#include "tst_MemoryFileSystem.moc"
