#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/sync/SyncTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/PartialWrite.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

using namespace mole;
using namespace mole::test;

namespace {

/// The whole payload, so a scenario can say "at 30%" and mean a byte.
constexpr qint64 kPayload = 4000;
constexpr qint64 kThirty = 1200;

} // namespace

/// A sync that goes wrong, on purpose.
///
/// The hostile slice through TransferTask found the same fault twice: a copy
/// loop that reads with the QByteArray overload cannot tell "the file ended"
/// from "the read failed", because QIODevice answers both with an empty result.
/// Sync had it too, and sync is the worse place for it: the destination is
/// committed, the file is counted as copied, and the *next* run sees matching
/// sizes on both sides and copies nothing. Nothing ever says a byte was lost.
///
/// So the three claims here are the three that matter when a sync fails: **a
/// half-copied file is never presented as a finished one**, **the source is
/// still there**, and **the failure says which file and why**. Nothing sleeps --
/// a fault fires at a byte offset.
class TestSyncTaskUnderFault : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aReadThatDiesMidFileIsAFailureAndNotACopiedFile();
    void aSourceThatClaimsMoreThanItGivesIsAFailure();
    void aSourceThatReallyShrankIsCopiedAsItNowIs();
    void aDestinationThatFillsUpSaysWhy();
    void aFileThatFailedIsNotCountedAsAppliedAndIsRetriedNextRun();
    void theByteCountIsTheWorkersOwnAndNotTheWindowsCopyOfIt();

private:
    SyncTask* run();
    /// Submits without waiting, for the one case that has to do something to the
    /// task while it is still running.
    SyncTask* start();
    QStringList destinationEntries() const;

    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::shared_ptr<FaultyFileSystem> m_source;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::shared_ptr<FaultyFileSystem> m_target;
    std::unique_ptr<TempTree> m_tree;
};

void TestSyncTaskUnderFault::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_memory->addFile(QStringLiteral("/src/payload.bin"), QByteArray(kPayload, 'a'));
    m_source = std::make_shared<FaultyFileSystem>(m_memory);
    m_disk = std::make_shared<LocalFileSystem>();
    m_target = std::make_shared<FaultyFileSystem>(m_disk);
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->makeDirs(QStringLiteral("dest")));
}

void TestSyncTaskUnderFault::cleanup()
{
    m_tasks.reset();
    m_source.reset();
    m_target.reset();
    m_memory.reset();
    m_disk.reset();
    m_tree.reset();
}

SyncTask* TestSyncTaskUnderFault::start()
{
    SyncOptions options;
    // Not a dry run, which is the default: what is being tested is what happens
    // to the bytes.
    options.dryRun = false;
    auto* task = new SyncTask(m_source, VfsUri::fromString(QStringLiteral("mem:///src")), m_target,
        m_tree->rootUri().child(QStringLiteral("dest")), options);
    m_tasks->submit(task);
    return task;
}

SyncTask* TestSyncTaskUnderFault::run()
{
    SyncTask* task = start();
    if (!waitForTask(task))
        return nullptr;
    return task;
}

QStringList TestSyncTaskUnderFault::destinationEntries() const
{
    return QDir(m_tree->absolute(QStringLiteral("dest"))).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}

void TestSyncTaskUnderFault::aReadThatDiesMidFileIsAFailureAndNotACopiedFile()
{
    // The fault this file exists for. The connection dies 1200 bytes into a
    // 4000-byte file; QIODevice reports that the same way it reports the end of
    // a file, and a loop that stops on an empty result takes the second for the
    // first. Sync then commits the destination and counts the file as copied --
    // and the run after this one sees matching sizes and copies nothing, so the
    // 2800 missing bytes are never noticed by anything.
    m_source->readFailsAt(kThirty);

    SyncTask* task = run();
    QVERIFY(task);

    QCOMPARE(task->appliedCount(), 0);
    QCOMPARE(task->failures().size(), 1);
    const QString failure = task->failures().first();
    QVERIFY2(failure.contains(QStringLiteral("payload.bin")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("stopped after 1200 bytes")), qPrintable(failure));

    // And nothing is left behind claiming to be the file -- not under its own
    // name and not under a working one.
    QVERIFY2(destinationEntries().isEmpty(), qPrintable(destinationEntries().join(QLatin1Char(' '))));
    // The source still holds all of it.
    QCOMPARE(
        m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///src/payload.bin"))).value().size, kPayload);
}

void TestSyncTaskUnderFault::aSourceThatClaimsMoreThanItGivesIsAFailure()
{
    // The drive says 4500 bytes, hands over 4000 and still says 4500 when asked
    // again. Nothing shrank: the read ended early and called it the end of the
    // file. A read that ends early and a file that shrank look identical from
    // the copy loop, so the source is asked -- once, and only when there is a
    // discrepancy to explain. See ADR-0027.
    m_source->listingOverstatesSizeBy(500);

    SyncTask* task = run();
    QVERIFY(task);

    QCOMPARE(task->appliedCount(), 0);
    QCOMPARE(task->failures().size(), 1);
    const QString failure = task->failures().first();
    QVERIFY2(failure.contains(QStringLiteral("payload.bin")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("4500")) && failure.contains(QStringLiteral("4000")),
        qPrintable(failure));
    QVERIFY(destinationEntries().isEmpty());
}

void TestSyncTaskUnderFault::aSourceThatReallyShrankIsCopiedAsItNowIs()
{
    // The other half of the guard above, and the reason it asks the source
    // rather than trusting the plan: this file really did shrink between the
    // listing and the read, so what arrives is the file as it now is and there
    // is nothing to report.
    m_source->fileChangesSizeAt(kThirty, kThirty);

    SyncTask* task = run();
    QVERIFY(task);

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
    QCOMPARE(task->appliedCount(), 1);
    QCOMPARE(destinationEntries(), QStringList { QStringLiteral("payload.bin") });
    QCOMPARE(QFileInfo(m_tree->absolute(QStringLiteral("dest/payload.bin"))).size(), kThirty);
}

void TestSyncTaskUnderFault::aDestinationThatFillsUpSaysWhy()
{
    // A destination that filled up, one whose connection went away and one whose
    // file was pulled out from under it were all "short write". Which of them it
    // was is the only part anybody can act on: one is "buy a disk", one is "try
    // again", and one is "something else is writing here".
    m_target->destinationFillsAt(kThirty);

    SyncTask* task = run();
    QVERIFY(task);

    QCOMPARE(task->appliedCount(), 0);
    QCOMPARE(task->failures().size(), 1);
    const QString failure = task->failures().first();
    QVERIFY2(failure.contains(QStringLiteral("payload.bin")), qPrintable(failure));
    QVERIFY2(!failure.endsWith(QStringLiteral("short write")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("1200")), qPrintable(failure));
    QVERIFY(destinationEntries().isEmpty());
}

void TestSyncTaskUnderFault::aFileThatFailedIsNotCountedAsAppliedAndIsRetriedNextRun()
{
    // What the whole fault was really about: the *next* run. A file wrongly
    // counted as copied leaves a destination whose size matches the source's, so
    // the following sync plans nothing and the loss becomes permanent and
    // silent. With the failure reported, the file is still missing and the
    // second run copies it.
    m_source->readFailsAt(kThirty);
    SyncTask* first = run();
    QVERIFY(first);
    QCOMPARE(first->appliedCount(), 0);

    // The drive behaves this time -- a fresh wrapper over the same contents,
    // which is what "the connection came back" looks like from here.
    m_source = std::make_shared<FaultyFileSystem>(m_memory);
    SyncTask* second = run();
    QVERIFY(second);
    QVERIFY2(second->failures().isEmpty(), qPrintable(second->failures().join(QLatin1Char(' '))));
    QCOMPARE(second->appliedCount(), 1);
    QCOMPARE(QFileInfo(m_tree->absolute(QStringLiteral("dest/payload.bin"))).size(), kPayload);
}

MOLE_TEST_MAIN(TestSyncTaskUnderFault)
/// The running total a sync reports, when the window has already read one.
///
/// `Task::bytesDone()` used to answer with the figure the *window* holds, which
/// is only refreshed when the task's report box is drained -- at most once every
/// `kDrainIntervalMs`. Sync built its running total by reading that figure back
/// and adding the chunk it had just written, so between two drains every
/// iteration added its chunk to the same stale number. The total stopped
/// advancing and the file was reported as a fraction of its size, while the copy
/// itself was perfectly correct. `TransferTask` never had it: it keeps its own
/// counter and reports that.
///
/// It was also a data race, which is how it was found. The map the figure came
/// from is written on the main thread when the box is drained and was being read
/// on the worker, with nothing between them -- ThreadSanitizer's only complaint
/// against this codebase once Qt could answer it (ADR-0055).
///
/// **Nothing here waits for a clock.** The stall holds the worker at a byte
/// offset; the test then waits for the drain to *land*, which is a condition it
/// can see. What makes the fault certain rather than likely is the drain
/// throttle itself: having just drained, the next one cannot come for
/// `kDrainIntervalMs`, and the few hundred bytes left of an in-memory file are
/// gone long before that.
void TestSyncTaskUnderFault::theByteCountIsTheWorkersOwnAndNotTheWindowsCopyOfIt()
{
    // Held at a thousand bytes, then let go in small pieces so that several
    // iterations run between one drain and the next.
    m_source->readStallsAt(1000);
    for (const qint64 at : { 1000, 1200, 1400, 1600, 1800, 2000, 2200, 2400 })
        m_source->readGoesShortAt(at, 100);

    SyncTask* task = start();
    QVERIFY(waitFor([this] { return m_source->isStalled(); }));

    // The window's copy catches up while the worker is held still. This is the
    // condition, not a duration: the figure appearing is the drain landing.
    QVERIFY(waitFor([task] { return task->bytesDone() == 1000; }));

    m_source->release();
    QVERIFY(waitForTask(task));

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
    QCOMPARE(task->bytesDone(), kPayload);

    // And the copy itself was never in doubt -- which is what made this one
    // worth catching. A count that lies about work that was done correctly is
    // read as a stall by whoever is watching it.
    QFile landed(m_tree->absolute(QStringLiteral("dest/payload.bin")));
    QVERIFY(landed.open(QIODevice::ReadOnly));
    QCOMPARE(landed.readAll(), QByteArray(kPayload, 'a'));
}

#include "tst_SyncTaskUnderFault.moc"
