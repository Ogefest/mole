#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/search/LiveSearchTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QElapsedTimer>

using namespace mole;
using namespace mole::test;

class TestLiveSearchTask : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void findsMatchesByName();
    void streamsResultsWhileRunning();
    void aHandfulOfMatchesArrivesBeforeTheWalkEnds();
    void filtersByExtension();
    void filtersByKind();
    void filtersBySize();
    void caseInsensitiveByDefault();
    void stopsAtResultLimit();
    void cancellationStopsSearch();
    void missingBackendFailsTask();

private:
    LiveSearchTask* start(SearchQuery query, FileSystemPtr fs = nullptr);

    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_fs;
    FileEntryList m_hits;
};

void TestLiveSearchTask::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<MemoryFileSystem>();
    m_hits.clear();
}

void TestLiveSearchTask::cleanup()
{
    m_tasks.reset();
    m_fs.reset();
}

LiveSearchTask* TestLiveSearchTask::start(SearchQuery query, FileSystemPtr fs)
{
    auto* task
        = new LiveSearchTask(fs ? fs : m_fs, VfsUri::fromString(QStringLiteral("mem:///")), std::move(query));
    connect(
        task, &LiveSearchTask::hitsFound, this, [this](const FileEntryList& batch) { m_hits.append(batch); });
    m_tasks->submit(task);
    return task;
}

void TestLiveSearchTask::findsMatchesByName()
{
    m_fs->addFile(QStringLiteral("/a/report-q1.pdf"));
    m_fs->addFile(QStringLiteral("/b/report-q2.pdf"));
    m_fs->addFile(QStringLiteral("/b/unrelated.txt"));

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("report")));

    LiveSearchTask* task = start(query);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->hitCount(), 2);
    QCOMPARE(m_hits.size(), 2);
}

void TestLiveSearchTask::streamsResultsWhileRunning()
{
    // More than one emit batch, so results must arrive in pieces rather than
    // all at the very end.
    for (int i = 0; i < 450; ++i)
        m_fs->addFile(QStringLiteral("/bulk/match%1.txt").arg(i));

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("match")));

    LiveSearchTask* task = start(query);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->hitCount(), 450);
    QCOMPARE(m_hits.size(), 450);
}

void TestLiveSearchTask::aHandfulOfMatchesArrivesBeforeTheWalkEnds()
{
    // Six matches -- nowhere near the two hundred that fill a batch -- spread over
    // directories on a drive that takes a quarter of a second to list each one, so
    // the walk lasts well over a second. Batching on count alone meant a search
    // like this showed nothing until it had finished, which is the case anyone
    // searching a large disk actually meets.
    m_fs->addFile(QStringLiteral("/a/hit-1.txt"));
    m_fs->addFile(QStringLiteral("/a/hit-2.txt"));
    m_fs->addFile(QStringLiteral("/b/hit-3.txt"));
    m_fs->addFile(QStringLiteral("/c/hit-4.txt"));
    m_fs->addFile(QStringLiteral("/d/hit-5.txt"));
    m_fs->addFile(QStringLiteral("/e/hit-6.txt"));
    m_fs->setListDelayMs(250);

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("hit")));

    // Timed rather than checked against isFinished(): the last flush happens inside
    // run(), before the task is marked finished, so "arrived while not finished" is
    // true even when everything arrives in one lump at the very end. The question is
    // *when*, and only a clock can answer it.
    QElapsedTimer clock;
    qint64 firstBatchAt = -1;
    qint64 finishedAt = -1;

    auto* task = new LiveSearchTask(m_fs, VfsUri::fromString(QStringLiteral("mem:///")), query);
    connect(
        task, &LiveSearchTask::hitsFound, this, [this, &clock, &firstBatchAt](const FileEntryList& batch) {
            m_hits.append(batch);
            if (firstBatchAt < 0)
                firstBatchAt = clock.elapsed();
        });
    connect(task, &Task::finished, this, [&clock, &finishedAt] { finishedAt = clock.elapsed(); });

    clock.start();
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    QCOMPARE(task->hitCount(), 6);
    // Six is far below the two hundred that fill a batch, which is the whole point:
    // if this passed by filling one, it would be testing nothing.
    QVERIFY(task->hitCount() < 200);

    QVERIFY(firstBatchAt >= 0);
    QVERIFY(finishedAt > 0);
    QVERIFY2(firstBatchAt < finishedAt / 2,
        qPrintable(QStringLiteral("first matches arrived at %1 ms of a %2 ms walk; they have to reach the "
                                  "view early, not at the end")
                       .arg(firstBatchAt)
                       .arg(finishedAt)));
}

void TestLiveSearchTask::filtersByExtension()
{
    m_fs->addFile(QStringLiteral("/x/data.csv"));
    m_fs->addFile(QStringLiteral("/x/data.parquet"));

    SearchQuery query;
    query.add(SearchPredicate::extension(QStringLiteral("PARQUET")));

    LiveSearchTask* task = start(query);
    QVERIFY(waitForTask(task));

    QCOMPARE(m_hits.size(), 1);
    QCOMPARE(m_hits.first().name, QStringLiteral("data.parquet"));
}

void TestLiveSearchTask::filtersByKind()
{
    m_fs->addFile(QStringLiteral("/folder/file.txt"));

    SearchQuery dirsOnly;
    dirsOnly.add(SearchPredicate::kind(true));
    LiveSearchTask* task = start(dirsOnly);
    QVERIFY(waitForTask(task));

    QCOMPARE(m_hits.size(), 1);
    QCOMPARE(m_hits.first().name, QStringLiteral("folder"));
    QVERIFY(m_hits.first().isDir);
}

void TestLiveSearchTask::filtersBySize()
{
    m_fs->addFile(QStringLiteral("/small.bin"), QByteArray(10, 'x'));
    m_fs->addFile(QStringLiteral("/big.bin"), QByteArray(5000, 'x'));

    SearchQuery query;
    query.add(SearchPredicate::minSize(1000));
    query.add(SearchPredicate::kind(false));

    LiveSearchTask* task = start(query);
    QVERIFY(waitForTask(task));

    QCOMPARE(m_hits.size(), 1);
    QCOMPARE(m_hits.first().name, QStringLiteral("big.bin"));
}

void TestLiveSearchTask::caseInsensitiveByDefault()
{
    m_fs->addFile(QStringLiteral("/ŁÓDŹ.txt"));

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("łódź")));

    LiveSearchTask* task = start(query);
    QVERIFY(waitForTask(task));
    QCOMPARE(m_hits.size(), 1);

    SearchQuery sensitive;
    sensitive.add(SearchPredicate::name(QStringLiteral("łódź"), true));
    m_hits.clear();

    LiveSearchTask* strict = start(sensitive);
    QVERIFY(waitForTask(strict));
    QVERIFY(m_hits.isEmpty());
}

void TestLiveSearchTask::stopsAtResultLimit()
{
    for (int i = 0; i < 100; ++i)
        m_fs->addFile(QStringLiteral("/many/file%1.txt").arg(i));

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("file")));
    query.limit = 10;

    LiveSearchTask* task = start(query);
    QVERIFY(waitForTask(task));

    // Truncation is a normal outcome and must be visible to the user, not an
    // error and not silently presented as a complete result set.
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->truncated());
    QCOMPARE(task->hitCount(), 10);
    QCOMPARE(m_hits.size(), 10);
    QVERIFY(task->statusText().contains(QStringLiteral("limit")));
}

void TestLiveSearchTask::cancellationStopsSearch()
{
    for (int i = 0; i < 40; ++i)
        m_fs->addFile(QStringLiteral("/dir%1/file.txt").arg(i));
    m_fs->setListDelayMs(50);

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("file")));

    LiveSearchTask* task = start(query);
    QVERIFY(waitFor([task] { return task->state() == Task::State::Running; }));
    task->requestCancel();

    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Cancelled);
}

void TestLiveSearchTask::missingBackendFailsTask()
{
    LiveSearchTask* task = start(SearchQuery {}, FileSystemPtr {});
    // start() substitutes m_fs when given null, so build this one directly.
    QVERIFY(waitForTask(task));

    auto* orphan = new LiveSearchTask(nullptr, VfsUri::fromString(QStringLiteral("mem:///")), SearchQuery {});
    m_tasks->submit(orphan);
    QVERIFY(waitForTask(orphan));

    QCOMPARE(orphan->state(), Task::State::Failed);
    QCOMPARE(orphan->error().code, VfsError::NotFound);
}

MOLE_TEST_MAIN(TestLiveSearchTask)
#include "tst_LiveSearchTask.moc"
