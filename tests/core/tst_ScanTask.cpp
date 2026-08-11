#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/index/ScanTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>

using namespace mole;
using namespace mole::test;

class TestScanTask : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void indexesWholeTree();
    void indexedEntriesAreSearchable();
    void rescanReplacesPreviousContents();
    void aSearchDuringARescanSeesThePreviousContents();
    void aCancelledRescanLeavesTheIndexAsItWas();
    void aRescanThatFailsHalfWayLeavesTheIndexAsItWas();
    void unreadableSubtreeIsReportedButScanSucceeds();
    void failingIndexWriteFailsTheTask();
    void cancellationLeavesTaskCancelled();
    void reportsProgressText();
    void aScanCanRecordWhatEachFileSaysAboutItself();
    void aScanWithoutItWritesWhatItAlwaysWrote();
    void aFileWhoseReaderFindsNothingIsStillIndexed();
    void whatIsInsideAContainerIsIndexedBesideIt();
    void aContainerNothingCanOpenCostsItsOwnRowsAndNoMore();

private:
    ScanTask* startScan(const QString& rootPath = QStringLiteral("/"));
    ScanTask* startScanOn(FileSystemPtr fileSystem);
    /// The three files a first scan settles into the index, and the count they
    /// come to. A rescan running over them must keep answering with these.
    void addSettledFiles();
    /// A tree with more entries in it than one insert batch holds, so a scan of
    /// it has really written rows by the time it is held still at /bulk/held --
    /// and more again below that point, so releasing it goes back to writing.
    void addTreeThatOutgrowsOneBatch();
    /// Starts a scan over `m_fs` wrapped in a drive that stalls at /bulk/held
    /// and returns once it is stopped there, with the drive in `m_held`. Null
    /// means it never got that far, which is a failed test rather than a
    /// reason to carry on.
    ScanTask* startScanHeldMidWalk();

    static constexpr int kSettledFiles = 3;
    /// ScanTask writes every 2000 rows, so each half of the tree is a little
    /// over that: enough for one flush before the hold and one after it.
    static constexpr int kBulkFiles = 2100;
    /// Every entry the second scan finds: the settled files, /bulk and its
    /// files, /bulk/held and its files.
    static constexpr int kRescanEntries = kSettledFiles + 1 + kBulkFiles + 1 + kBulkFiles;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<IndexDatabase> m_index;
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_fs;
    std::shared_ptr<FaultyFileSystem> m_held;
};

void TestScanTask::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_index = std::make_unique<IndexDatabase>(QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite")));
    QVERIFY(m_index->open().ok());

    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<MemoryFileSystem>();
}

void TestScanTask::cleanup()
{
    // A scan left stalled holds a pool thread for ever, and the task manager
    // waits for it on the way out. A test that fails before its own release()
    // has to fail, not hang.
    if (m_held)
        m_held->release();
    m_held.reset();
    m_tasks.reset();
    m_index.reset();
    m_fs.reset();
    m_dir.reset();
}

ScanTask* TestScanTask::startScan(const QString& rootPath)
{
    auto* task = new ScanTask(
        m_fs, VfsUri(QStringLiteral("mem"), QString(), rootPath), QStringLiteral("scratch"), m_index.get());
    m_tasks->submit(task);
    return task;
}

ScanTask* TestScanTask::startScanOn(FileSystemPtr fileSystem)
{
    // The same root as startScan(), so the second scan is a rescan of the same
    // volume rather than a scan of a new one.
    auto* task
        = new ScanTask(std::move(fileSystem), VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
            QStringLiteral("scratch"), m_index.get());
    m_tasks->submit(task);
    return task;
}

void TestScanTask::addSettledFiles()
{
    for (int i = 0; i < kSettledFiles; ++i)
        m_fs->addFile(QStringLiteral("/settled-%1.txt").arg(i));
}

void TestScanTask::addTreeThatOutgrowsOneBatch()
{
    // Nested rather than side by side: a walk lists /bulk in full before it
    // descends, so the hold below it lands after the first batch has gone in
    // whatever order the backend returns entries.
    for (int i = 0; i < kBulkFiles; ++i) {
        m_fs->addFile(QStringLiteral("/bulk/f%1.txt").arg(i));
        m_fs->addFile(QStringLiteral("/bulk/held/g%1.txt").arg(i));
    }
}

ScanTask* TestScanTask::startScanHeldMidWalk()
{
    m_held = std::make_shared<FaultyFileSystem>(m_fs);
    m_held->listStalls(QStringLiteral("/bulk/held"));

    ScanTask* task = startScanOn(m_held);
    // On the condition, never on a clock: the scan really is stopped part way
    // through, with rows written and nothing committed.
    return waitFor([this] { return m_held->isStalled(); }) ? task : nullptr;
}

void TestScanTask::indexesWholeTree()
{
    m_fs->addFile(QStringLiteral("/a.txt"));
    m_fs->addFile(QStringLiteral("/sub/b.txt"));
    m_fs->addFile(QStringLiteral("/sub/deeper/c.txt"));

    ScanTask* task = startScan();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    // 3 files + 2 directories.
    QCOMPARE(task->filesIndexed(), 5);
    QCOMPARE(m_index->fileCount().value(), 5);
}

void TestScanTask::indexedEntriesAreSearchable()
{
    m_fs->addFile(QStringLiteral("/projects/2026/budget.xlsx"));
    m_fs->addFile(QStringLiteral("/projects/2026/notes.md"));

    QVERIFY(waitForTask(startScan()));

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("budget")));
    Result<QList<IndexSearchHit>> hits = m_index->search(query);

    QVERIFY(hits.ok());
    QCOMPARE(hits.value().size(), 1);
    QCOMPARE(hits.value().first().uri, QStringLiteral("mem:///projects/2026/budget.xlsx"));
    QCOMPARE(hits.value().first().volumeLabel, QStringLiteral("scratch"));
}

void TestScanTask::rescanReplacesPreviousContents()
{
    m_fs->addFile(QStringLiteral("/gone.txt"));
    QVERIFY(waitForTask(startScan()));
    QCOMPARE(m_index->fileCount().value(), 1);

    // The file disappears between scans; the index must forget it rather than
    // keep serving a stale hit forever.
    QVERIFY(m_fs->remove(VfsUri::fromString(QStringLiteral("mem:///gone.txt")), false).ok());
    m_fs->addFile(QStringLiteral("/fresh.txt"));

    QVERIFY(waitForTask(startScan()));
    QCOMPARE(m_index->fileCount().value(), 1);
    QCOMPARE(m_index->volumes().value().size(), 1);

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("gone")));
    QVERIFY(m_index->search(query).value().isEmpty());
}

/// The fault this pair of scans exists for.
///
/// A rescan used to empty the index before walking, so for as long as it ran a
/// search over that volume answered from the part already re-walked -- fast,
/// confident and short. On a 4 TB tree that window is hours, and large trees
/// are the ones people index.
void TestScanTask::aSearchDuringARescanSeesThePreviousContents()
{
    addSettledFiles();
    QVERIFY(waitForTask(startScan()));
    QCOMPARE(m_index->fileCount().value(), kSettledFiles);

    addTreeThatOutgrowsOneBatch();
    ScanTask* task = startScanHeldMidWalk();
    QVERIFY2(task != nullptr, "the rescan never reached the point it was to be held at");

    // The rescan has written rows by now and not one of them may be visible,
    // and nothing it is going to replace may have gone missing yet.
    QCOMPARE(m_index->fileCount().value(), kSettledFiles);
    SearchQuery settled;
    settled.add(SearchPredicate::name(QStringLiteral("settled-")));
    QCOMPARE(m_index->search(settled).value().size(), kSettledFiles);
    SearchQuery arriving; // "f1" names only the /bulk files
    arriving.add(SearchPredicate::name(QStringLiteral("f1")));
    QVERIFY2(m_index->search(arriving).value().isEmpty(),
        "half a scan became visible, which is a search answering from a tree it has not finished reading");

    m_held->release();
    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(m_index->fileCount().value(), kRescanEntries);
}

void TestScanTask::aCancelledRescanLeavesTheIndexAsItWas()
{
    addSettledFiles();
    QVERIFY(waitForTask(startScan()));
    const IndexVolume before = m_index->volumes().value().first();

    addTreeThatOutgrowsOneBatch();
    ScanTask* task = startScanHeldMidWalk();
    QVERIFY2(task != nullptr, "the rescan never reached the point it was to be held at");

    task->requestCancel();
    m_held->release();
    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Cancelled);

    // A cancelled rescan losing the index is the same fault wearing a hat.
    QCOMPARE(m_index->fileCount().value(), kSettledFiles);
    const IndexVolume after = m_index->volumes().value().first();
    QCOMPARE(after.fileCount, before.fileCount);
    QCOMPARE(after.lastScan, before.lastScan);
}

void TestScanTask::aRescanThatFailsHalfWayLeavesTheIndexAsItWas()
{
    addSettledFiles();
    QVERIFY(waitForTask(startScan()));
    const IndexVolume before = m_index->volumes().value().first();

    addTreeThatOutgrowsOneBatch();
    ScanTask* task = startScanHeldMidWalk();
    QVERIFY2(task != nullptr, "the rescan never reached the point it was to be held at");

    // The index goes away under the scan while it is stopped, so the next batch
    // it tries to write is refused. Nothing it wrote before that can tidy
    // itself up afterwards -- which is the point: what is visible has to be
    // decided by what was committed, not by a cleanup path having run.
    m_index->close();
    m_held->release();
    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Failed);

    QVERIFY(m_index->open().ok());
    QCOMPARE(m_index->fileCount().value(), kSettledFiles);
    const IndexVolume after = m_index->volumes().value().first();
    QCOMPARE(after.fileCount, before.fileCount);
    QCOMPARE(after.lastScan, before.lastScan);
}

void TestScanTask::unreadableSubtreeIsReportedButScanSucceeds()
{
    m_fs->addFile(QStringLiteral("/open/a.txt"));
    m_fs->addFile(QStringLiteral("/locked/b.txt"));
    m_fs->setFault(QStringLiteral("/locked"), VfsError::AccessDenied);

    ScanTask* task = startScan();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->skippedDirectories(), 1);
    QVERIFY2(task->statusText().contains(QStringLiteral("unreadable")),
        "the user has to be told part of the tree was skipped");

    // /open, /open/a.txt and /locked itself were indexed; /locked/b.txt was not.
    QCOMPARE(m_index->fileCount().value(), 3);
}

void TestScanTask::failingIndexWriteFailsTheTask()
{
    m_fs->addFile(QStringLiteral("/a.txt"));
    m_index->close(); // simulate the index becoming unavailable

    ScanTask* task = startScan();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Failed);
    QVERIFY(!errorTextOf(*task).isEmpty());
}

void TestScanTask::cancellationLeavesTaskCancelled()
{
    for (int i = 0; i < 40; ++i)
        m_fs->addFile(QStringLiteral("/dir%1/file.txt").arg(i));
    m_fs->setListDelayMs(50);

    ScanTask* task = startScan();
    QVERIFY(waitFor([task] { return task->state() == Task::State::Running; }));
    task->requestCancel();

    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Cancelled);
}

void TestScanTask::reportsProgressText()
{
    for (int i = 0; i < 5; ++i)
        m_fs->addFile(QStringLiteral("/f%1.txt").arg(i));

    ScanTask* task = startScan();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->progress(), 100);
    QVERIFY(task->statusText().contains(QStringLiteral("5 entries indexed")));
}

void TestScanTask::aScanCanRecordWhatEachFileSaysAboutItself()
{
    m_fs->addFile(QStringLiteral("/photos/a.jpg"));
    m_fs->addFile(QStringLiteral("/photos/b.jpg"));

    auto* task = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    task->setFactReader([](const FileEntry& entry) -> QList<SearchFact> {
        if (entry.uri.suffix() != QLatin1String("jpg"))
            return {};
        return { SearchFact { QStringLiteral("image.camera"), QStringLiteral("Canon EOS 5D"), 0, false },
            SearchFact { QStringLiteral("image.iso"), QStringLiteral("ISO 400"), 400, true } };
    });
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Succeeded);

    SearchQuery byCamera;
    byCamera.add(SearchPredicate::metadataIs(QStringLiteral("image.camera"), QStringLiteral("canon")));
    QCOMPARE(m_index->search(byCamera).value().size(), 2);

    SearchQuery fast;
    fast.add(SearchPredicate::metadataAtLeast(QStringLiteral("image.iso"), 1000));
    QVERIFY(m_index->search(fast).value().isEmpty());

    // Where the time goes is where the progress has to look.
    QCOMPARE(task->filesRead(), 2);
    QVERIFY2(task->statusText().contains(QStringLiteral("read")),
        qPrintable(QStringLiteral("the line said: %1").arg(task->statusText())));
}

void TestScanTask::aScanWithoutItWritesWhatItAlwaysWrote()
{
    m_fs->addFile(QStringLiteral("/photos/a.jpg"));

    ScanTask* task = startScan();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->filesRead(), 0);
    QCOMPARE(m_index->fileCount().value(), 2); // the folder and the file
    QCOMPARE(task->statusText(), QStringLiteral("2 entries indexed"));

    // Nothing to ask about, and asking is not an error.
    SearchQuery byCamera;
    byCamera.add(SearchPredicate::metadataIs(QStringLiteral("image.camera"), QStringLiteral("canon")));
    QVERIFY(m_index->search(byCamera).ok());
    QVERIFY(m_index->search(byCamera).value().isEmpty());
}

void TestScanTask::aFileWhoseReaderFindsNothingIsStillIndexed()
{
    m_fs->addFile(QStringLiteral("/mixed/readable.jpg"));
    m_fs->addFile(QStringLiteral("/mixed/awkward.bin"));

    auto* task = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    task->setFactReader([](const FileEntry& entry) -> QList<SearchFact> {
        // What a reader that gave up looks like from here: nothing at all.
        if (entry.uri.suffix() == QLatin1String("bin"))
            return {};
        return { SearchFact { QStringLiteral("image.camera"), QStringLiteral("Canon"), 0, false } };
    });
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    // Both files and both folders are in the index; only one said anything.
    QCOMPARE(m_index->fileCount().value(), 3);
    SearchQuery awkward;
    awkward.add(SearchPredicate::name(QStringLiteral("awkward")));
    QCOMPARE(m_index->search(awkward).value().size(), 1);
}

/// A zip of years-old projects holds a great deal of what somebody is looking
/// for, and none of it could be found by any means at all.
void TestScanTask::whatIsInsideAContainerIsIndexedBesideIt()
{
    m_fs->addFile(QStringLiteral("/backup.zip"));
    m_fs->addFile(QStringLiteral("/loose.txt"));

    auto* task = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    task->setContainerReader([](const FileEntry& entry, bool* truncated) -> QList<IndexedFile> {
        if (entry.uri.suffix() != QLatin1String("zip"))
            return {};
        // More than this reader is willing to take, said on the container's row.
        *truncated = true;
        IndexedFile member;
        member.name = QStringLiteral("report.pdf");
        member.path = QStringLiteral("/report.pdf");
        member.parentPath = QStringLiteral("/");
        member.extension = QStringLiteral("pdf");
        member.size = 4096;
        return { member };
    });
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->containedEntries(), 1);

    // Found by name, like anything else.
    SearchQuery byName;
    byName.add(SearchPredicate::name(QStringLiteral("report")));
    QCOMPARE(m_index->search(byName).value().size(), 1);
    QCOMPARE(m_index->search(byName).value().first().name, QStringLiteral("report.pdf"));

    // The container itself is still a row, and the count includes both.
    QCOMPARE(m_index->fileCount().value(), 3);
    QVERIFY2(
        task->statusText().contains(QStringLiteral("inside containers")), qPrintable(task->statusText()));
}

void TestScanTask::aContainerNothingCanOpenCostsItsOwnRowsAndNoMore()
{
    m_fs->addFile(QStringLiteral("/broken.zip"));
    m_fs->addFile(QStringLiteral("/fine.txt"));

    auto* task = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    // What a corrupt, encrypted or unsupported container looks like from here:
    // nothing at all, and no reason for the scan to stop.
    task->setContainerReader([](const FileEntry&, bool*) -> QList<IndexedFile> { return {}; });
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->containedEntries(), 0);
    QCOMPARE(m_index->fileCount().value(), 2);
}

MOLE_TEST_MAIN(TestScanTask)
#include "tst_ScanTask.moc"
