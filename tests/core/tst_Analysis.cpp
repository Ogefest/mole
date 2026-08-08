#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/analysis/AnalyseDirectoryTask.h"
#include "core/analysis/AnalysisStore.h"
#include "core/analysis/ReportDiff.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

class TestAnalysis : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // --- buckets ---
    void sizeBuckets_data();
    void sizeBuckets();
    void ageBuckets();

    // --- the walk ---
    void countsFilesFoldersAndBytes();
    void groupsByExtension();
    void attributesBytesToTopLevelFolders();
    void keepsTheLargestFiles();
    void fillsSizeAndAgeHistograms();
    void reportsUnreadableFolders();
    void recordsDepth();
    void emptyDirectoryProducesAValidReport();
    void cancellationProducesNoReport();

    // --- storage ---
    void savesAndReloadsAReport();
    void historyIsNewestFirst();
    void twoRunsInTheSameSecondBothSurvive();
    void keepsHistoriesApartPerDirectory();
    void survivesACorruptReportFile();
    void pruneKeepsTheNewest();
    void listsEveryAnalysedRoot();

    // --- diffing ---
    void diffReportsGrowth();
    void diffSpotsNewAndRemovedExtensions();
    void diffSortsByBiggestChange();
    void refusesToDiffDifferentDirectories();
    void refusesToDiffAnInvalidReport();

private:
    AnalysisReport analyse(const QString& path, const QString& label = {});

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_fs;
    std::unique_ptr<AnalysisStore> m_store;
};

void TestAnalysis::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<MemoryFileSystem>();
    m_store = std::make_unique<AnalysisStore>(QDir(m_dir->path()).filePath(QStringLiteral("analysis")));
}

void TestAnalysis::cleanup()
{
    m_tasks.reset();
    m_fs.reset();
    m_store.reset();
    m_dir.reset();
}

AnalysisReport TestAnalysis::analyse(const QString& path, const QString& label)
{
    auto* task = new AnalyseDirectoryTask(m_fs, VfsUri(QStringLiteral("mem"), QString(), path), label);
    m_tasks->submit(task);
    if (!waitForTask(task, 15000))
        return {};
    return task->report();
}

// ---------------------------------------------------------------- buckets

void TestAnalysis::sizeBuckets_data()
{
    QTest::addColumn<qint64>("bytes");
    QTest::addColumn<int>("bucket");

    QTest::newRow("empty file") << qint64(0) << 0;
    QTest::newRow("just under 1 kB") << qint64(1023) << 0;
    QTest::newRow("1 kB") << qint64(1024) << 1;
    QTest::newRow("50 kB") << qint64(50 * 1024) << 2;
    QTest::newRow("500 kB") << qint64(500 * 1024) << 3;
    QTest::newRow("5 MB") << qint64(5LL * 1024 * 1024) << 4;
    QTest::newRow("50 MB") << qint64(50LL * 1024 * 1024) << 5;
    QTest::newRow("500 MB") << qint64(500LL * 1024 * 1024) << 6;
    QTest::newRow("4 GB") << qint64(4LL * 1024 * 1024 * 1024) << 7;
}

void TestAnalysis::sizeBuckets()
{
    QFETCH(qint64, bytes);
    QFETCH(int, bucket);
    QCOMPARE(AnalysisReport::sizeBucketFor(bytes), bucket);
    // Every bucket must have a label, or the chart draws a nameless bar.
    QVERIFY(bucket < AnalysisReport::sizeBucketLabels().size());
}

void TestAnalysis::ageBuckets()
{
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1750000000);

    QCOMPARE(AnalysisReport::ageBucketFor(now.addSecs(-3600), now), 0); // today
    QCOMPARE(AnalysisReport::ageBucketFor(now.addDays(-3), now), 1); // this week
    QCOMPARE(AnalysisReport::ageBucketFor(now.addDays(-20), now), 2); // this month
    QCOMPARE(AnalysisReport::ageBucketFor(now.addDays(-200), now), 3); // this year
    QCOMPARE(AnalysisReport::ageBucketFor(now.addDays(-800), now), 4); // 1-5 years
    QCOMPARE(AnalysisReport::ageBucketFor(now.addDays(-4000), now), 5); // older
    // A backend that cannot report a timestamp gets its own bucket rather
    // than being silently counted as ancient.
    QCOMPARE(AnalysisReport::ageBucketFor(QDateTime(), now), 6);
}

// ------------------------------------------------------------------ walk

void TestAnalysis::countsFilesFoldersAndBytes()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(100, 'x'));
    m_fs->addFile(QStringLiteral("/root/b.txt"), QByteArray(200, 'x'));
    m_fs->addFile(QStringLiteral("/root/sub/c.bin"), QByteArray(300, 'x'));

    const AnalysisReport report = analyse(QStringLiteral("/root"));

    QVERIFY(report.isValid());
    QCOMPARE(report.fileCount, 3);
    QCOMPARE(report.folderCount, 1);
    QCOMPARE(report.totalBytes, 600);
    QCOMPARE(report.averageFileBytes(), 200);
    QVERIFY(!report.partial);
}

void TestAnalysis::groupsByExtension()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(100, 'x'));
    m_fs->addFile(QStringLiteral("/root/b.txt"), QByteArray(200, 'x'));
    m_fs->addFile(QStringLiteral("/root/big.mkv"), QByteArray(5000, 'x'));
    m_fs->addFile(QStringLiteral("/root/README"), QByteArray(50, 'x'));

    const AnalysisReport report = analyse(QStringLiteral("/root"));

    // Sorted by bytes: the thing filling the disk comes first.
    QCOMPARE(report.extensions.first().extension, QStringLiteral("mkv"));
    QCOMPARE(report.extensions.first().bytes, 5000);

    const auto txt = std::find_if(report.extensions.begin(), report.extensions.end(),
        [](const ExtensionStat& s) { return s.extension == QLatin1String("txt"); });
    QVERIFY(txt != report.extensions.end());
    QCOMPARE(txt->count, 2);
    QCOMPARE(txt->bytes, 300);

    // Files without an extension are their own group rather than vanishing.
    const auto none = std::find_if(report.extensions.begin(), report.extensions.end(),
        [](const ExtensionStat& s) { return s.extension.isEmpty(); });
    QVERIFY(none != report.extensions.end());
    QCOMPARE(none->count, 1);
}

void TestAnalysis::attributesBytesToTopLevelFolders()
{
    m_fs->addFile(QStringLiteral("/root/media/film.mkv"), QByteArray(9000, 'x'));
    m_fs->addFile(QStringLiteral("/root/media/deep/clip.mkv"), QByteArray(1000, 'x'));
    m_fs->addFile(QStringLiteral("/root/docs/notes.txt"), QByteArray(10, 'x'));
    m_fs->addFile(QStringLiteral("/root/loose.txt"), QByteArray(5, 'x'));

    const AnalysisReport report = analyse(QStringLiteral("/root"));

    // "Which subfolder is responsible" is the first question anyone asks, and
    // nested files must count towards their top-level ancestor.
    QCOMPARE(report.topFolders.first().name, QStringLiteral("media"));
    QCOMPARE(report.topFolders.first().bytes, 10000);
    QCOMPARE(report.topFolders.first().count, 2);

    QCOMPARE(report.topFolders.at(1).name, QStringLiteral("docs"));
    // A file sitting directly in the root belongs to no subfolder.
    for (const FolderStat& folder : report.topFolders)
        QVERIFY(folder.name != QStringLiteral("loose.txt"));
}

void TestAnalysis::keepsTheLargestFiles()
{
    for (int i = 1; i <= 40; ++i)
        m_fs->addFile(QStringLiteral("/root/f%1.bin").arg(i), QByteArray(i * 10, 'x'));

    const AnalysisReport report = analyse(QStringLiteral("/root"));

    // Capped and sorted: the point is not holding every file in memory.
    QCOMPARE(report.largestFiles.size(), 25);
    QCOMPARE(report.largestFiles.first().bytes, 400);
    QCOMPARE(report.largestFiles.first().name, QStringLiteral("f40.bin"));
    for (int i = 1; i < report.largestFiles.size(); ++i)
        QVERIFY(report.largestFiles.at(i - 1).bytes >= report.largestFiles.at(i).bytes);
}

void TestAnalysis::fillsSizeAndAgeHistograms()
{
    m_fs->addFile(QStringLiteral("/root/tiny.txt"), QByteArray(10, 'x'));
    m_fs->addFile(QStringLiteral("/root/small.txt"), QByteArray(5000, 'x'));
    m_fs->addFile(QStringLiteral("/root/medium.bin"), QByteArray(50000, 'x'));

    const AnalysisReport report = analyse(QStringLiteral("/root"));

    QCOMPARE(report.sizeBuckets.size(), AnalysisReport::sizeBucketLabels().size());
    QCOMPARE(report.ageBuckets.size(), AnalysisReport::ageBucketLabels().size());
    QCOMPARE(report.sizeBuckets.at(0).count, 1); // under 1 kB
    QCOMPARE(report.sizeBuckets.at(1).count, 1); // 1-10 kB
    QCOMPARE(report.sizeBuckets.at(2).count, 1); // 10-100 kB

    qint64 histogramTotal = 0;
    for (const BucketStat& bucket : report.sizeBuckets)
        histogramTotal += bucket.count;
    QCOMPARE(histogramTotal, report.fileCount);
}

void TestAnalysis::reportsUnreadableFolders()
{
    m_fs->addFile(QStringLiteral("/root/open/a.txt"), QByteArray(10, 'x'));
    m_fs->addFile(QStringLiteral("/root/locked/secret.txt"), QByteArray(999, 'x'));
    m_fs->setFault(QStringLiteral("/root/locked"), VfsError::AccessDenied);

    const AnalysisReport report = analyse(QStringLiteral("/root"));

    // A total that quietly excludes a subtree is worse than no total, so the
    // report says so out loud.
    QCOMPARE(report.unreadableFolders, 1);
    QVERIFY2(report.partial, "a report missing a subtree must admit it");
    QCOMPARE(report.fileCount, 1);
}

void TestAnalysis::recordsDepth()
{
    m_fs->addFile(QStringLiteral("/root/a/b/c/deep.txt"), QByteArray(1, 'x'));
    const AnalysisReport report = analyse(QStringLiteral("/root"));
    QCOMPARE(report.maxDepth, 4);
}

void TestAnalysis::emptyDirectoryProducesAValidReport()
{
    m_fs->addDirectory(QStringLiteral("/empty"));
    const AnalysisReport report = analyse(QStringLiteral("/empty"));

    // Zero is an answer. An invalid report would look like a failure.
    QVERIFY(report.isValid());
    QCOMPARE(report.fileCount, 0);
    QCOMPARE(report.totalBytes, 0);
    QCOMPARE(report.averageFileBytes(), 0);
    QVERIFY(report.extensions.isEmpty());
}

void TestAnalysis::cancellationProducesNoReport()
{
    for (int i = 0; i < 40; ++i)
        m_fs->addFile(QStringLiteral("/root/d%1/f.txt").arg(i), QByteArray(10, 'x'));
    m_fs->setListDelayMs(50);

    auto* task = new AnalyseDirectoryTask(
        m_fs, VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/root")), QString());
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->state() == Task::State::Running; }));
    task->requestCancel();

    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Cancelled);
    // Half a walk is not a report, and saving one would poison the history.
    QVERIFY(!task->report().isValid());
}

// --------------------------------------------------------------- storage

void TestAnalysis::savesAndReloadsAReport()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(123, 'x'));
    const AnalysisReport report = analyse(QStringLiteral("/root"), QStringLiteral("My folder"));

    QVERIFY(m_store->save(report));

    const AnalysisReport loaded = m_store->load(report.rootUri, report.id);
    QCOMPARE(loaded.rootUri, report.rootUri);
    QCOMPARE(loaded.label, QStringLiteral("My folder"));
    QCOMPARE(loaded.fileCount, report.fileCount);
    QCOMPARE(loaded.totalBytes, report.totalBytes);
    QCOMPARE(loaded.extensions.size(), report.extensions.size());
    QCOMPARE(loaded.extensions.first().extension, report.extensions.first().extension);
    QCOMPARE(loaded.sizeBuckets.size(), report.sizeBuckets.size());
    QCOMPARE(loaded.createdAt.toSecsSinceEpoch(), report.createdAt.toSecsSinceEpoch());
}

void TestAnalysis::historyIsNewestFirst()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(10, 'x'));

    AnalysisReport older = analyse(QStringLiteral("/root"));
    older.id = QStringLiteral("20260101-000000");
    older.createdAt = QDateTime::fromSecsSinceEpoch(1700000000);
    QVERIFY(m_store->save(older));

    AnalysisReport newer = analyse(QStringLiteral("/root"));
    newer.id = QStringLiteral("20260201-000000");
    newer.createdAt = QDateTime::fromSecsSinceEpoch(1800000000);
    QVERIFY(m_store->save(newer));

    const QList<ReportSummary> history = m_store->history(older.rootUri);
    QCOMPARE(history.size(), 2);
    QCOMPARE(history.first().id, QStringLiteral("20260201-000000"));
    QCOMPARE(m_store->latest(older.rootUri).id, QStringLiteral("20260201-000000"));
}

void TestAnalysis::twoRunsInTheSameSecondBothSurvive()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(10, 'x'));

    // Ids used to be second-resolution, so a quick second run overwrote the
    // first and the history lost exactly the entry it existed to compare with.
    const AnalysisReport first = analyse(QStringLiteral("/root"));
    const AnalysisReport second = analyse(QStringLiteral("/root"));

    QVERIFY2(first.id != second.id, "two runs must never share an id");
    QVERIFY(m_store->save(first));
    QVERIFY(m_store->save(second));
    QCOMPARE(m_store->history(first.rootUri).size(), 2);
}

void TestAnalysis::keepsHistoriesApartPerDirectory()
{
    m_fs->addFile(QStringLiteral("/one/a.txt"), QByteArray(10, 'x'));
    m_fs->addFile(QStringLiteral("/two/b.txt"), QByteArray(20, 'x'));

    QVERIFY(m_store->save(analyse(QStringLiteral("/one"))));
    QVERIFY(m_store->save(analyse(QStringLiteral("/two"))));

    // Analysing two folders at once has to build two histories, not one mixed.
    QCOMPARE(m_store->history(QStringLiteral("mem:///one")).size(), 1);
    QCOMPARE(m_store->history(QStringLiteral("mem:///two")).size(), 1);
    QCOMPARE(m_store->history(QStringLiteral("mem:///one")).first().totalBytes, 10);
    QCOMPARE(m_store->history(QStringLiteral("mem:///two")).first().totalBytes, 20);
}

void TestAnalysis::survivesACorruptReportFile()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(10, 'x'));
    const AnalysisReport good = analyse(QStringLiteral("/root"));
    QVERIFY(m_store->save(good));

    // Drop a broken file next to the good one.
    QDir folder(m_store->directory());
    const QStringList subfolders = folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(subfolders.size(), 1);
    QFile broken(QDir(folder.filePath(subfolders.first())).filePath(QStringLiteral("bad.json")));
    QVERIFY(broken.open(QIODevice::WriteOnly));
    broken.write("{ not json");
    broken.close();

    // One bad file must not hide the rest of the history.
    const QList<ReportSummary> history = m_store->history(good.rootUri);
    QCOMPARE(history.size(), 1);
    QCOMPARE(history.first().id, good.id);
}

void TestAnalysis::pruneKeepsTheNewest()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(10, 'x'));
    const AnalysisReport base = analyse(QStringLiteral("/root"));

    for (int i = 0; i < 5; ++i) {
        AnalysisReport report = base;
        report.id = QStringLiteral("2026010%1-000000").arg(i);
        report.createdAt = QDateTime::fromSecsSinceEpoch(1700000000 + i * 86400);
        QVERIFY(m_store->save(report));
    }
    QCOMPARE(m_store->history(base.rootUri).size(), 5);

    // History is useful; unbounded history is a slow leak.
    QCOMPARE(m_store->prune(base.rootUri, 2), 3);
    const QList<ReportSummary> left = m_store->history(base.rootUri);
    QCOMPARE(left.size(), 2);
    QCOMPARE(left.first().id, QStringLiteral("20260104-000000"));
}

void TestAnalysis::listsEveryAnalysedRoot()
{
    m_fs->addFile(QStringLiteral("/one/a.txt"), QByteArray(10, 'x'));
    m_fs->addFile(QStringLiteral("/two/b.txt"), QByteArray(20, 'x'));
    QVERIFY(m_store->save(analyse(QStringLiteral("/one"))));
    QVERIFY(m_store->save(analyse(QStringLiteral("/two"))));

    const QStringList roots = m_store->analysedRoots();
    QCOMPARE(roots.size(), 2);
    QVERIFY(roots.contains(QStringLiteral("mem:///one")));
    QVERIFY(roots.contains(QStringLiteral("mem:///two")));
}

// --------------------------------------------------------------- diffing

void TestAnalysis::diffReportsGrowth()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(100, 'x'));
    const AnalysisReport before = analyse(QStringLiteral("/root"));

    m_fs->addFile(QStringLiteral("/root/b.txt"), QByteArray(400, 'x'));
    const AnalysisReport after = analyse(QStringLiteral("/root"));

    const ReportDiff diff = ReportDiff::between(before, after);
    QVERIFY(diff.valid);
    QCOMPARE(diff.fileCountDelta(), 1);
    QCOMPARE(diff.totalBytesDelta(), 400);
}

void TestAnalysis::diffSpotsNewAndRemovedExtensions()
{
    m_fs->addFile(QStringLiteral("/root/keep.txt"), QByteArray(100, 'x'));
    m_fs->addFile(QStringLiteral("/root/old.log"), QByteArray(50, 'x'));
    const AnalysisReport before = analyse(QStringLiteral("/root"));

    QVERIFY(m_fs->remove(VfsUri::fromString(QStringLiteral("mem:///root/old.log")), false).ok());
    m_fs->addFile(QStringLiteral("/root/new.mkv"), QByteArray(9000, 'x'));
    const AnalysisReport after = analyse(QStringLiteral("/root"));

    const ReportDiff diff = ReportDiff::between(before, after);
    QVERIFY(diff.valid);

    const auto find = [&diff](const QString& extension) {
        return std::find_if(diff.extensions.begin(), diff.extensions.end(),
            [&extension](const ExtensionDelta& d) { return d.extension == extension; });
    };

    const auto mkv = find(QStringLiteral("mkv"));
    QVERIFY(mkv != diff.extensions.end());
    QVERIFY(mkv->isNew());
    QCOMPARE(mkv->bytesDelta(), 9000);

    const auto log = find(QStringLiteral("log"));
    QVERIFY(log != diff.extensions.end());
    QVERIFY(log->isGone());
    QCOMPARE(log->bytesDelta(), -50);

    // Something that did not move is still listed: "this is unchanged" is
    // sometimes the answer you came for.
    const auto txt = find(QStringLiteral("txt"));
    QVERIFY(txt != diff.extensions.end());
    QVERIFY(!txt->changed());
}

void TestAnalysis::diffSortsByBiggestChange()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(100, 'x'));
    m_fs->addFile(QStringLiteral("/root/huge.iso"), QByteArray(50000, 'x'));
    const AnalysisReport before = analyse(QStringLiteral("/root"));

    QVERIFY(m_fs->remove(VfsUri::fromString(QStringLiteral("mem:///root/huge.iso")), false).ok());
    m_fs->addFile(QStringLiteral("/root/b.txt"), QByteArray(20, 'x'));
    const AnalysisReport after = analyse(QStringLiteral("/root"));

    const ReportDiff diff = ReportDiff::between(before, after);
    // A large deletion has to be as prominent as a large addition.
    QCOMPARE(diff.extensions.first().extension, QStringLiteral("iso"));
    QCOMPARE(diff.extensions.first().bytesDelta(), -50000);
}

void TestAnalysis::refusesToDiffDifferentDirectories()
{
    m_fs->addFile(QStringLiteral("/one/a.txt"), QByteArray(10, 'x'));
    m_fs->addFile(QStringLiteral("/two/b.txt"), QByteArray(20, 'x'));

    // Numbers that look meaningful and are not would be worse than nothing.
    const ReportDiff diff
        = ReportDiff::between(analyse(QStringLiteral("/one")), analyse(QStringLiteral("/two")));
    QVERIFY(!diff.valid);
}

void TestAnalysis::refusesToDiffAnInvalidReport()
{
    m_fs->addFile(QStringLiteral("/root/a.txt"), QByteArray(10, 'x'));
    QVERIFY(!ReportDiff::between(AnalysisReport {}, analyse(QStringLiteral("/root"))).valid);
    QVERIFY(!ReportDiff::between(analyse(QStringLiteral("/root")), AnalysisReport {}).valid);
}

MOLE_TEST_MAIN(TestAnalysis)
#include "tst_Analysis.moc"
