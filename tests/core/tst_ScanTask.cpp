#include "sdk/ScanReaders.h"
#include "support/FakePlugin.h"
#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/index/ScanTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QSemaphore>

using namespace mole;
using namespace mole::test;

namespace {
/// The readers a test made, answering the one question factReaderFor() asks of
/// the host. The real one is MetadataRegistry, which lives a layer above this
/// suite; the point here is what the scan hands a reader, not who found it.
class FakeLookup final : public IMetadataLookup
{
public:
    void add(IMetadataReader* reader) { m_readers.append(reader); }
    QList<IMetadataReader*> readersFor(const FileEntry& entry) const override
    {
        QList<IMetadataReader*> claiming;
        for (IMetadataReader* reader : m_readers) {
            if (reader->canRead(entry))
                claiming.append(reader);
        }
        return claiming;
    }

private:
    QList<IMetadataReader*> m_readers;
};

/// A scan asked to keep what has not changed, and nothing else. The only
/// option most of these cases vary.
ScanOptions incrementally()
{
    ScanOptions options;
    options.incremental = true;
    return options;
}
}

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
    void theRealFactReaderHandsAReaderTheServicesItNeeds();
    void aReaderThatThrowsCostsItsOwnRowsAndNotTheScan();
    void aReaderSeesTheScanBeingCancelledUnderIt();
    void aSecondScanOfAnUnchangedTreeWalksNothing();
    void whatChangedIsReflectedAndWhatWentIsGone();
    void aDriveThatDoesNotDateItsFoldersIsWalkedInFullAndSaysSo();
    void aFullRescanDoesWhatItSays();
    void whatIsInsideAContainerIsIndexedBesideIt();
    void aContainerNothingCanOpenCostsItsOwnRowsAndNoMore();
    void anIncrementalScanKeepsWhatBothHalvesOfTheTreeWereAskedFor();

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
    task->setFactReader([](const FileEntry& entry, const CancelToken&) -> QList<SearchFact> {
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
    task->setFactReader([](const FileEntry& entry, const CancelToken&) -> QList<SearchFact> {
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

void TestScanTask::theRealFactReaderHandsAReaderTheServicesItNeeds()
{
    // Through factReaderFor() rather than a stub, because the argument the scan
    // passes is the whole of this: every shipped reader that needs bytes past
    // the sniff page checks services.vfs and gives up without it, so an empty
    // struct here means the index knows less about a file than the panel does
    // and "camera is X" over the index misses files the drawer names.
    m_fs->addFile(QStringLiteral("/photos/a.jpg"));

    auto log = std::make_shared<FakeMetadataReader::Log>();
    FakeMetadataReader reader(QStringLiteral("test.exif"),
        QList<FileFact> {
            { QStringLiteral("Camera"), QStringLiteral("Canon EOS 5D"), QStringLiteral("image.camera") } },
        10, log);
    FakeLookup lookup;
    lookup.add(&reader);

    VfsManager vfs;
    PluginServices services;
    services.vfs = &vfs;
    services.metadata = &lookup;

    auto* task = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    task->setFactReader(factReaderFor(services, m_fs));
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(log->reads.load(), 1);
    QVERIFY(log->sawServices.load());

    SearchQuery byCamera;
    byCamera.add(SearchPredicate::metadataIs(QStringLiteral("image.camera"), QStringLiteral("canon")));
    QCOMPARE(m_index->search(byCamera).value().size(), 1);
}

void TestScanTask::aReaderThatThrowsCostsItsOwnRowsAndNotTheScan()
{
    // In the panel a throwing reader loses its own rows and is named in the log.
    // Unguarded here, the throw travelled out of the walker's callback and
    // ScanTask::run() into Task::run()'s generic net: the scan ended Failed with
    // "stopped unexpectedly", the volume was left half-written, and nothing said
    // which reader did it. One third-party plugin with a bug on one odd file
    // turned a nightly re-index of a whole NAS into a failed job.
    m_fs->addFile(QStringLiteral("/photos/a.jpg"));

    FakeMetadataReader broken(QStringLiteral("test.broken"), QList<FileFact> {}, 20);
    broken.failInstead();
    FakeMetadataReader sound(QStringLiteral("test.sound"),
        QList<FileFact> {
            { QStringLiteral("Camera"), QStringLiteral("Nikon Z6"), QStringLiteral("image.camera") } },
        10);
    FakeLookup lookup;
    lookup.add(&broken);
    lookup.add(&sound);

    VfsManager vfs;
    PluginServices services;
    services.vfs = &vfs;
    services.metadata = &lookup;

    auto* task = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    task->setFactReader(factReaderFor(services, m_fs));
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(m_index->fileCount().value(), 2); // the folder and the file

    SearchQuery byCamera;
    byCamera.add(SearchPredicate::metadataIs(QStringLiteral("image.camera"), QStringLiteral("nikon")));
    QCOMPARE(m_index->search(byCamera).value().size(), 1);
}

void TestScanTask::aReaderSeesTheScanBeingCancelledUnderIt()
{
    // The token used to be a default-constructed one, so a reader that opens a
    // large file to find the tags at the end of it could not notice the scan had
    // been stopped: it finished its file first, and on a slow drive that is the
    // difference between a cancel and a wait.
    m_fs->addFile(QStringLiteral("/photos/a.jpg"));

    auto log = std::make_shared<FakeMetadataReader::Log>();
    auto gate = std::make_shared<QSemaphore>();
    FakeMetadataReader reader(QStringLiteral("test.slow"), QList<FileFact> {}, 10, log);
    reader.holdUntilReleased(gate);
    FakeLookup lookup;
    lookup.add(&reader);

    VfsManager vfs;
    PluginServices services;
    services.vfs = &vfs;
    services.metadata = &lookup;

    auto* task = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    task->setFactReader(factReaderFor(services, m_fs));
    m_tasks->submit(task);

    // Waited for, not slept through: the reader is inside read() and holding.
    QVERIFY(waitFor([&log] { return log->reads.load() == 1; }, 10000));
    task->requestCancel();
    gate->release();

    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Cancelled);
    QCOMPARE(log->cancelled.load(), 1);
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

/// Hours to learn that nothing much has moved is what this exists to stop.
void TestScanTask::aSecondScanOfAnUnchangedTreeWalksNothing()
{
    for (int i = 0; i < 5; ++i)
        m_fs->addFile(QStringLiteral("/deep/branch%1/leaf.txt").arg(i));

    // Dated before the scan, because a folder changed in the same second the
    // scan read it is one the scan is right to distrust for ever after.
    const QDateTime settled = QDateTime::currentDateTime().addSecs(-3600);
    m_fs->setModified(QStringLiteral("/"), settled);
    m_fs->setModified(QStringLiteral("/deep"), settled);
    for (int i = 0; i < 5; ++i)
        m_fs->setModified(QStringLiteral("/deep/branch%1").arg(i), settled);

    QVERIFY(waitForTask(startScan()));
    const int listedInFull = m_fs->listCallCount();
    const qint64 rows = m_index->fileCount().value();
    QVERIFY(rows > 5);

    auto* again = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    again->setOptions(incrementally());
    m_tasks->submit(again);
    QVERIFY(waitForTask(again));

    QCOMPARE(again->state(), Task::State::Succeeded);
    QVERIFY2(again->datesFolders(), "the memory drive dates its folders, so there was something to skip");
    QVERIFY2(again->carriedForward() > 0, "an unchanged tree has to be kept rather than rewritten");

    // The same index afterwards, at a fraction of the listings.
    QCOMPARE(m_index->fileCount().value(), rows);
    const int listedIncrementally = m_fs->listCallCount() - listedInFull;
    QVERIFY2(listedIncrementally < listedInFull,
        qPrintable(QStringLiteral("the second scan listed %1 directories and the first listed %2")
                       .arg(listedIncrementally)
                       .arg(listedInFull)));
    QVERIFY2(
        again->statusText().contains(QStringLiteral("unchanged and kept")), qPrintable(again->statusText()));
}

void TestScanTask::whatChangedIsReflectedAndWhatWentIsGone()
{
    m_fs->addFile(QStringLiteral("/keep/steady.txt"));
    m_fs->addFile(QStringLiteral("/churn/old.txt"));
    m_fs->addFile(QStringLiteral("/doomed/inside.txt"));
    const QDateTime settled = QDateTime::currentDateTime().addSecs(-3600);
    for (const QString& folder : { QStringLiteral("/"), QStringLiteral("/keep"), QStringLiteral("/churn"),
             QStringLiteral("/doomed") }) {
        m_fs->setModified(folder, settled);
    }
    QVERIFY(waitForTask(startScan()));
    QCOMPARE(m_index->fileCount().value(), 6); // three folders, three files

    // Added, changed, deleted, and a whole subtree removed.
    m_fs->addFile(QStringLiteral("/churn/new.txt"));
    QVERIFY(m_fs->remove(VfsUri::fromString(QStringLiteral("mem:///churn/old.txt")), false).ok());
    QVERIFY(m_fs->remove(VfsUri::fromString(QStringLiteral("mem:///doomed")), true).ok());

    auto* again = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    again->setOptions(incrementally());
    m_tasks->submit(again);
    QVERIFY(waitForTask(again));

    const auto found = [this](const QString& text) {
        SearchQuery query;
        query.add(SearchPredicate::name(text));
        const Result<QList<IndexSearchHit>> hits = m_index->search(query);
        return hits.ok() ? hits.value().size() : -1;
    };

    QCOMPARE(found(QStringLiteral("new.txt")), 1);
    QCOMPARE(found(QStringLiteral("old.txt")), 0);
    QCOMPARE(found(QStringLiteral("steady.txt")), 1);
    // The case that makes this correct rather than fast: a subtree that is gone
    // is not in its parent's listing, so nothing carried it forward.
    QCOMPARE(found(QStringLiteral("inside.txt")), 0);
    QCOMPARE(found(QStringLiteral("doomed")), 0);
}

void TestScanTask::aDriveThatDoesNotDateItsFoldersIsWalkedInFullAndSaysSo()
{
    // A backend that reports no times at all: there is nothing to compare, so
    // the scan walks the lot rather than being quietly wrong about it.
    m_fs->addDirectory(QStringLiteral("/undated"));
    m_fs->addFile(QStringLiteral("/undated/a.txt"));
    QVERIFY(waitForTask(startScan()));

    auto* again = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    again->setOptions(incrementally());
    m_tasks->submit(again);
    QVERIFY(waitForTask(again));

    // The memory drive does date its folders, so this holds the other half:
    // when it can be trusted, nothing is said about it.
    if (again->datesFolders()) {
        QVERIFY(!again->statusText().contains(QStringLiteral("does not date")));
    } else {
        QVERIFY2(again->statusText().contains(QStringLiteral("does not date")),
            "a drive that gives the scan nothing to go on has to say so");
        QCOMPARE(again->carriedForward(), 0);
    }
}

void TestScanTask::aFullRescanDoesWhatItSays()
{
    m_fs->addFile(QStringLiteral("/tree/a.txt"));
    QVERIFY(waitForTask(startScan()));
    const int listedInFull = m_fs->listCallCount();

    // The default, which is what "full rescan" is: nothing kept, everything
    // walked, for when somebody suspects the index.
    ScanTask* again = startScan();
    QVERIFY(waitForTask(again));

    QCOMPARE(again->carriedForward(), 0);
    QCOMPARE(m_fs->listCallCount() - listedInFull, listedInFull);
    QCOMPARE(m_index->fileCount().value(), 2);
}

/// The half a scheduled re-index used to lose. An incremental scan carries the
/// unchanged subtrees across and re-walks the rest, and the rows it writes for
/// the part it re-walked have to say as much about those files as the rows it
/// carried say about theirs -- otherwise the index answers for the parts of the
/// tree nobody has touched and stops answering for the parts they have.
void TestScanTask::anIncrementalScanKeepsWhatBothHalvesOfTheTreeWereAskedFor()
{
    m_fs->addFile(QStringLiteral("/steady/kept.jpg"));
    m_fs->addFile(QStringLiteral("/steady/kept.bag"));
    m_fs->addFile(QStringLiteral("/churn/moved.jpg"));
    m_fs->addFile(QStringLiteral("/churn/moved.bag"));

    // Dated before the scan, so the second one has something it can trust.
    const QDateTime settled = QDateTime::currentDateTime().addSecs(-3600);
    for (const QString& folder :
        { QStringLiteral("/"), QStringLiteral("/steady"), QStringLiteral("/churn") }) {
        m_fs->setModified(folder, settled);
    }

    const auto camera = [](const FileEntry& entry, const CancelToken&) -> QList<SearchFact> {
        if (entry.uri.suffix() != QLatin1String("jpg"))
            return {};
        return { SearchFact { QStringLiteral("image.camera"), QStringLiteral("X100V"), 0, false } };
    };
    const auto inside = [](const FileEntry& entry, bool*) -> QList<IndexedFile> {
        if (entry.uri.suffix() != QLatin1String("bag"))
            return {};
        // Named after its container, because the index answers about names and
        // not about where a row sits.
        const QString name = entry.name + QStringLiteral("-member.txt");
        IndexedFile member;
        member.name = name;
        member.path = entry.uri.path() + QLatin1Char('!') + name;
        member.parentPath = entry.uri.path();
        member.extension = QStringLiteral("txt");
        member.uri = entry.uri.toString() + QLatin1Char('!') + name;
        return { member };
    };

    auto* first = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    first->setFactReader(camera);
    first->setContainerReader(inside);
    m_tasks->submit(first);
    QVERIFY(waitForTask(first));
    QCOMPARE(first->state(), Task::State::Succeeded);

    // One subtree moves and the other does not, which is what makes the second
    // scan carry half the tree and re-walk half of it.
    m_fs->setModified(QStringLiteral("/churn"), QDateTime::currentDateTime());

    ScanOptions options;
    options.incremental = true;
    options.metadata = true;
    options.archives = true;
    auto* again = new ScanTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")),
        QStringLiteral("scratch"), m_index.get());
    again->setOptions(options);
    again->setFactReader(camera);
    again->setContainerReader(inside);
    m_tasks->submit(again);
    QVERIFY(waitForTask(again));
    QCOMPARE(again->state(), Task::State::Succeeded);
    QVERIFY2(again->carriedForward() > 0, "the unchanged subtree has to have been carried, not re-walked");

    // Named rather than located, because each half is one file and a name is
    // what the index answers about most directly.
    const auto cameraFor = [this](const QString& name) {
        SearchQuery query;
        query.add(SearchPredicate::metadataIs(QStringLiteral("image.camera"), QStringLiteral("x100v")));
        query.add(SearchPredicate::name(name));
        const Result<QList<IndexSearchHit>> hits = m_index->search(query);
        return hits.ok() ? hits.value().size() : -1;
    };
    QCOMPARE(cameraFor(QStringLiteral("kept.jpg")), 1); // carried across
    QCOMPARE(cameraFor(QStringLiteral("moved.jpg")), 1); // re-walked, and the half that used to go

    const auto memberOf = [this](const QString& name) {
        SearchQuery query;
        query.add(SearchPredicate::name(name));
        const Result<QList<IndexSearchHit>> hits = m_index->search(query);
        return hits.ok() ? hits.value().size() : -1;
    };
    QCOMPARE(memberOf(QStringLiteral("kept.bag-member.txt")), 1);
    QCOMPARE(memberOf(QStringLiteral("moved.bag-member.txt")), 1);
}

MOLE_TEST_MAIN(TestScanTask)
#include "tst_ScanTask.moc"
