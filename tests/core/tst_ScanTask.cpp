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
    void unreadableSubtreeIsReportedButScanSucceeds();
    void failingIndexWriteFailsTheTask();
    void cancellationLeavesTaskCancelled();
    void reportsProgressText();

private:
    ScanTask* startScan(const QString& rootPath = QStringLiteral("/"));

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<IndexDatabase> m_index;
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_fs;
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

    IndexSearchQuery query;
    query.text = QStringLiteral("budget");
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

    IndexSearchQuery query;
    query.text = QStringLiteral("gone");
    QVERIFY(m_index->search(query).value().isEmpty());
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

MOLE_TEST_MAIN(TestScanTask)
#include "tst_ScanTask.moc"
