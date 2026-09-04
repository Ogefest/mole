#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/alerts/AlertEvaluator.h"
#include "core/alerts/AlertStore.h"
#include "core/alerts/CheckAlertsTask.h"
#include "core/analysis/AnalyseDirectoryTask.h"
#include "core/analysis/AnalysisStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// Alerts: measuring a metric, judging it, and remembering what happened.
class TestAlerts : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ---- measuring ----
    void measuresTheSizeOfATree();
    void measuresFileAndFolderCounts();
    void measuresFreeSpaceOnTheDrive();
    void measuresPermissions();
    void measuresWhetherSomethingExists();
    void measuresHowLongSinceAnythingChanged();
    void refusesAMetricItCannotRead();
    void readingFromAReportSaysSoWhenThereIsNone();
    void readsFromASavedReport();

    // ---- a failure to read is not a reading -----------------------------
    void anUnreachableTargetIsNotAMissingOne();
    void aFolderItCouldNotEnterIsNotAnEmptyOne();
    void aRootItCouldNotEnterIsNotAnEmptyTree();
    void theAgeOfTheNewestFileIsRefusedFromAReport();
    void aModifiedTimeIsSaidInUtc();

    // ---- judging ----
    void triggersWhenAboveTheThreshold();
    void staysQuietWhenWithinBounds();
    void aFirstReadingEstablishesTheBaselineForChanged();
    void changedTripsOnTheSecondDifferentReading();
    void anUnreadableMetricIsNotOk();

    // ---- storing ----
    void survivesARestart();
    void aRuleNamingSomethingUnknownIsDroppedAndCounted();
    void announcesOnlyTransitions();
    void countsWhatIsTriggered();

    // ---- the task ----
    void checksEveryRuleOnAPoolThread();

private:
    AlertEvaluator::Reading measure(const AlertRule& rule) const;
    AlertRule ruleFor(
        AlertMetric metric, AlertComparison comparison, double threshold, const QString& target = {}) const;

    std::unique_ptr<QTemporaryDir> m_profile;
    std::unique_ptr<TempTree> m_tree;
    /// The mounted drive, wrapped so any case can make it misbehave. Every case
    /// that declares no fault sees plain local disk.
    std::shared_ptr<FaultyFileSystem> m_disk;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<AnalysisStore> m_analysis;
    std::unique_ptr<TaskManager> m_tasks;
};

void TestAlerts::init()
{
    m_profile = std::make_unique<QTemporaryDir>();
    QVERIFY(m_profile->isValid());
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());

    QVERIFY(m_tree->writeFile(QStringLiteral("media/film.mkv"), QByteArray(9000, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("media/clip.mkv"), QByteArray(1000, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("docs/a.txt"), QByteArray(100, 'x')));

    m_vfs = std::make_unique<VfsManager>();
    m_disk = std::make_shared<FaultyFileSystem>(std::make_shared<LocalFileSystem>());
    Mount mount;
    mount.id = QStringLiteral("local");
    mount.displayName = QStringLiteral("Local");
    mount.root = VfsUri::fromLocalPath(QStringLiteral("/"));
    mount.fileSystem = m_disk;
    m_vfs->addMount(mount);

    m_analysis
        = std::make_unique<AnalysisStore>(QDir(m_profile->path()).filePath(QStringLiteral("analysis")));
    m_tasks = std::make_unique<TaskManager>();
}

void TestAlerts::cleanup()
{
    m_tasks.reset();
    m_analysis.reset();
    m_vfs.reset();
    m_disk.reset();
    m_tree.reset();
    m_profile.reset();
}

AlertRule TestAlerts::ruleFor(
    AlertMetric metric, AlertComparison comparison, double threshold, const QString& target) const
{
    AlertRule rule;
    rule.id = QStringLiteral("r1");
    rule.label = QStringLiteral("Watch");
    rule.targetUri = target.isEmpty() ? m_tree->rootUri().toString() : target;
    rule.metric = metric;
    rule.comparison = comparison;
    rule.threshold = threshold;
    return rule;
}

AlertEvaluator::Reading TestAlerts::measure(const AlertRule& rule) const
{
    const AlertEvaluator evaluator(m_vfs.get(), m_analysis.get());
    return evaluator.measure(rule, CancelToken());
}

// ------------------------------------------------------------- measuring

void TestAlerts::measuresTheSizeOfATree()
{
    const AlertEvaluator::Reading reading
        = measure(ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 0));
    QVERIFY2(reading.measured, qPrintable(reading.error));
    QCOMPARE(reading.number, 10100.0);
}

void TestAlerts::measuresFileAndFolderCounts()
{
    QCOMPARE(measure(ruleFor(AlertMetric::FileCount, AlertComparison::Above, 0)).number, 3.0);
    QCOMPARE(measure(ruleFor(AlertMetric::FolderCount, AlertComparison::Above, 0)).number, 2.0);
    QCOMPARE(measure(ruleFor(AlertMetric::LargestFile, AlertComparison::Above, 0)).number, 9000.0);
}

void TestAlerts::measuresFreeSpaceOnTheDrive()
{
    const AlertEvaluator::Reading bytes = measure(ruleFor(AlertMetric::FreeSpace, AlertComparison::Below, 0));
    QVERIFY2(bytes.measured, qPrintable(bytes.error));
    QVERIFY(bytes.number > 0);

    const AlertEvaluator::Reading percent
        = measure(ruleFor(AlertMetric::FreeSpacePercent, AlertComparison::Below, 0));
    QVERIFY(percent.measured);
    QVERIFY(percent.number >= 0.0 && percent.number <= 100.0);
}

void TestAlerts::measuresPermissions()
{
    AlertRule rule = ruleFor(AlertMetric::Permissions, AlertComparison::Changed, 0,
        m_tree->rootUri().child(QStringLiteral("docs/a.txt")).toString());

    const AlertEvaluator::Reading reading = measure(rule);
    QVERIFY2(reading.measured, qPrintable(reading.error));
    // "rw-r--r--" and friends: legible without decoding an octal number.
    QCOMPARE(reading.text.size(), 9);
    QVERIFY(reading.text.startsWith(QLatin1Char('r')));
}

void TestAlerts::measuresWhetherSomethingExists()
{
    AlertRule present = ruleFor(AlertMetric::Exists, AlertComparison::Below, 1,
        m_tree->rootUri().child(QStringLiteral("docs/a.txt")).toString());
    QCOMPARE(measure(present).number, 1.0);

    AlertRule missing = ruleFor(AlertMetric::Exists, AlertComparison::Below, 1,
        m_tree->rootUri().child(QStringLiteral("docs/gone.txt")).toString());
    const AlertEvaluator::Reading reading = measure(missing);
    // A file that is not there is a successful measurement of zero, not a
    // failure to measure -- otherwise the alert could never fire.
    QVERIFY(reading.measured);
    QCOMPARE(reading.number, 0.0);
}

void TestAlerts::measuresHowLongSinceAnythingChanged()
{
    const AlertEvaluator::Reading reading
        = measure(ruleFor(AlertMetric::NewestFileAgeHours, AlertComparison::Above, 48));
    QVERIFY2(reading.measured, qPrintable(reading.error));
    // The fixture was written moments ago, so a backup watch on it is quiet.
    QVERIFY(reading.number < 1.0);
}

void TestAlerts::refusesAMetricItCannotRead()
{
    AlertRule rule = ruleFor(
        AlertMetric::TotalSize, AlertComparison::Above, 0, QStringLiteral("nosuchscheme://host/data"));
    const AlertEvaluator::Reading reading = measure(rule);
    QVERIFY(!reading.measured);
    QVERIFY(!reading.error.isEmpty());
}

void TestAlerts::readingFromAReportSaysSoWhenThereIsNone()
{
    AlertRule rule = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 0);
    rule.source = AlertSource::LatestReport;

    const AlertEvaluator::Reading reading = measure(rule);
    // Deliberately not falling back to a live walk: that would quietly turn
    // this into a different alert with different timing.
    QVERIFY(!reading.measured);
    QVERIFY(reading.error.contains(QStringLiteral("report"), Qt::CaseInsensitive));
}

void TestAlerts::readsFromASavedReport()
{
    auto fs = std::make_shared<LocalFileSystem>();
    auto* task = new AnalyseDirectoryTask(fs, m_tree->rootUri(), QStringLiteral("fixture"));
    bool stored = false;
    connect(task, &AnalyseDirectoryTask::reportReady, this,
        [this, &stored](const AnalysisReport& report) { stored = m_analysis->save(report); });
    m_tasks->submit(task);
    QVERIFY(waitFor([&stored] { return stored; }, 20000));

    AlertRule rule = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 0);
    rule.source = AlertSource::LatestReport;

    const AlertEvaluator::Reading reading = measure(rule);
    QVERIFY2(reading.measured, qPrintable(reading.error));
    QCOMPARE(reading.number, 10100.0);
}

// ------------------------------- a failure to read is not a reading

void TestAlerts::anUnreachableTargetIsNotAMissingOne()
{
    const VfsUri target = m_tree->rootUri().child(QStringLiteral("docs/a.txt"));
    m_disk->statFails(target.path(), VfsError::NetworkError, QStringLiteral("the share is not answering"));

    const AlertRule rule = ruleFor(AlertMetric::Exists, AlertComparison::Below, 1, target.toString());
    const AlertEvaluator::Reading reading = measure(rule);

    // "no" here would fire an Exists-below-1 alert on a share that is merely
    // down, and clear an Exists-above-0 one. Neither is an answer about whether
    // the file is there: only NotFound is that.
    QVERIFY(!reading.measured);
    QVERIFY2(reading.error.contains(QStringLiteral("not answering")), qPrintable(reading.error));
    QCOMPARE(AlertEvaluator::apply(rule, reading, QDateTime::currentDateTime()).state, AlertState::Failed);
}

void TestAlerts::aFolderItCouldNotEnterIsNotAnEmptyOne()
{
    // The walk records the folder and carries on, so every count it produces is
    // short by whatever was inside -- 1100 of the fixture's 10100 bytes here.
    m_disk->listFails(m_tree->rootUri().child(QStringLiteral("media")).path());

    const AlertRule rule = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 5000);
    const AlertEvaluator::Reading partial = measure(rule);
    QVERIFY(!partial.measured);
    QVERIFY2(partial.error.contains(QStringLiteral("could not be read")), qPrintable(partial.error));
    QCOMPARE(AlertEvaluator::apply(rule, partial, QDateTime::currentDateTime()).state, AlertState::Failed);

    // Except for the one metric that is about exactly this, which still answers.
    const AlertEvaluator::Reading counted
        = measure(ruleFor(AlertMetric::UnreadableFolders, AlertComparison::Above, 0));
    QVERIFY2(counted.measured, qPrintable(counted.error));
    QCOMPARE(counted.number, 1.0);
}

void TestAlerts::aRootItCouldNotEnterIsNotAnEmptyTree()
{
    // The worst shape of it: nothing was read at all, and every tree metric used
    // to answer 0 -- so "files below 1" fired and "size above 5000" cleared.
    m_disk->listFails(m_tree->rootUri().path());

    for (AlertMetric metric : { AlertMetric::TotalSize, AlertMetric::FileCount, AlertMetric::FolderCount,
             AlertMetric::LargestFile }) {
        const AlertEvaluator::Reading reading = measure(ruleFor(metric, AlertComparison::Below, 1));
        QVERIFY2(!reading.measured, qPrintable(alertMetricLabel(metric)));
    }
}

void TestAlerts::theAgeOfTheNewestFileIsRefusedFromAReport()
{
    // A backup folder whose large files are all forty days old and whose only
    // recent write is a small log outside the largest twenty-five. The report
    // carries no tree-wide newest timestamp, so the newest among the large files
    // is the only thing there is to read -- and it is not the answer.
    const QDateTime now = QDateTime::currentDateTime();
    AnalysisReport report;
    report.id = QStringLiteral("20260101-000000-000");
    report.rootUri = m_tree->rootUri().toString();
    report.label = QStringLiteral("fixture");
    report.createdAt = now;
    report.fileCount = 26;
    report.folderCount = 2;
    report.totalBytes = 10100;
    FileStat archive;
    archive.name = QStringLiteral("film.mkv");
    archive.uri = m_tree->rootUri().child(QStringLiteral("media/film.mkv")).toString();
    archive.bytes = 9000;
    archive.modified = now.addDays(-40);
    report.largestFiles = { archive };
    QVERIFY(m_analysis->save(report));

    AlertRule rule = ruleFor(AlertMetric::NewestFileAgeHours, AlertComparison::Above, 24);
    rule.source = AlertSource::LatestReport;

    const AlertEvaluator::Reading reading = measure(rule);
    QVERIFY(!reading.measured);
    QVERIFY2(
        reading.error.contains(QStringLiteral("cannot be read from a report")), qPrintable(reading.error));

    // Failed says "ask somebody"; Triggered would have said "the backup stopped
    // forty days ago", which is a claim about a folder written to this morning.
    QCOMPARE(AlertEvaluator::apply(rule, reading, now).state, AlertState::Failed);
}

void TestAlerts::aModifiedTimeIsSaidInUtc()
{
    const QString relative = QStringLiteral("docs/a.txt");
    const AlertEvaluator::Reading reading = measure(ruleFor(AlertMetric::ModifiedTime,
        AlertComparison::Changed, 0, m_tree->rootUri().child(relative).toString()));

    QVERIFY2(reading.measured, qPrintable(reading.error));
    // Changed compares this text against the last one, so it must not move when
    // the clocks do. A local rendering of one unchanged instant differs either
    // side of a daylight-saving switch, which is one false alert a year.
    QCOMPARE(
        reading.text, QFileInfo(m_tree->absolute(relative)).lastModified().toUTC().toString(Qt::ISODate));
    QVERIFY2(reading.text.endsWith(QLatin1Char('Z')), qPrintable(reading.text));
}

// --------------------------------------------------------------- judging

void TestAlerts::triggersWhenAboveTheThreshold()
{
    const AlertRule rule = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 5000);
    const AlertRule after = AlertEvaluator::apply(rule, measure(rule), QDateTime::currentDateTime());

    QCOMPARE(after.state, AlertState::Triggered);
    QVERIFY(after.triggeredAt.isValid());
    QVERIFY(!after.message.isEmpty());
}

void TestAlerts::staysQuietWhenWithinBounds()
{
    const AlertRule rule = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 1000000);
    const AlertRule after = AlertEvaluator::apply(rule, measure(rule), QDateTime::currentDateTime());

    QCOMPARE(after.state, AlertState::Ok);
    QVERIFY(after.message.isEmpty());
    QVERIFY(!after.triggeredAt.isValid());
}

void TestAlerts::aFirstReadingEstablishesTheBaselineForChanged()
{
    const AlertRule rule = ruleFor(AlertMetric::TotalSize, AlertComparison::Changed, 0);
    const AlertRule after = AlertEvaluator::apply(rule, measure(rule), QDateTime::currentDateTime());

    // Otherwise every alert would fire the instant it was created, which
    // teaches the user to ignore the first one they ever set.
    QCOMPARE(after.state, AlertState::Ok);
    QVERIFY(!after.lastValue.isEmpty());
}

void TestAlerts::changedTripsOnTheSecondDifferentReading()
{
    AlertRule rule = ruleFor(AlertMetric::TotalSize, AlertComparison::Changed, 0);
    rule = AlertEvaluator::apply(rule, measure(rule), QDateTime::currentDateTime());
    QCOMPARE(rule.state, AlertState::Ok);

    QVERIFY(m_tree->writeFile(QStringLiteral("docs/b.txt"), QByteArray(500, 'y')));

    rule = AlertEvaluator::apply(rule, measure(rule), QDateTime::currentDateTime());
    QCOMPARE(rule.state, AlertState::Triggered);
    QVERIFY(rule.message.contains(QStringLiteral("changed")));

    // And it settles again once the new value is the baseline.
    rule = AlertEvaluator::apply(rule, measure(rule), QDateTime::currentDateTime());
    QCOMPARE(rule.state, AlertState::Ok);
}

void TestAlerts::anUnreadableMetricIsNotOk()
{
    AlertRule rule = ruleFor(
        AlertMetric::TotalSize, AlertComparison::Above, 0, QStringLiteral("nosuchscheme://host/data"));
    const AlertRule after = AlertEvaluator::apply(rule, measure(rule), QDateTime::currentDateTime());

    // An unreachable drive reported as a green tick is the worst outcome
    // available: it looks exactly like everything being fine.
    QCOMPARE(after.state, AlertState::Failed);
    QVERIFY(!after.message.isEmpty());
}

// --------------------------------------------------------------- storing

void TestAlerts::survivesARestart()
{
    const QString path = QDir(m_profile->path()).filePath(QStringLiteral("alerts.json"));
    {
        AlertStore store(path);
        AlertRule rule = ruleFor(AlertMetric::FreeSpacePercent, AlertComparison::Below, 10);
        rule.label = QStringLiteral("Disk filling up");
        rule.state = AlertState::Triggered;
        rule.lastValue = QStringLiteral("7.5%");
        QVERIFY(store.put(rule));
    }

    AlertStore reopened(path);
    QVERIFY(reopened.load());
    QCOMPARE(reopened.rules().size(), 1);

    const AlertRule rule = reopened.rule(QStringLiteral("r1"));
    QCOMPARE(rule.label, QStringLiteral("Disk filling up"));
    QCOMPARE(rule.metric, AlertMetric::FreeSpacePercent);
    QCOMPARE(rule.comparison, AlertComparison::Below);
    QCOMPARE(rule.threshold, 10.0);
    QCOMPARE(rule.state, AlertState::Triggered);
    QCOMPARE(rule.lastValue, QStringLiteral("7.5%"));
}

void TestAlerts::aRuleNamingSomethingUnknownIsDroppedAndCounted()
{
    // What a newer build leaves behind: a metric, a comparison and a source this
    // one has never heard of, one bad name per rule and one good rule among them.
    const auto stored
        = [](const QString& id, const QString& metric, const QString& comparison, const QString& source) {
              QJsonObject rule;
              rule[QStringLiteral("id")] = id;
              rule[QStringLiteral("targetUri")] = QStringLiteral("file:///data");
              rule[QStringLiteral("metric")] = metric;
              rule[QStringLiteral("comparison")] = comparison;
              rule[QStringLiteral("source")] = source;
              return rule;
          };

    QJsonArray rules;
    rules.append(stored(QStringLiteral("good"), QStringLiteral("freeSpacePercent"), QStringLiteral("below"),
        QStringLiteral("live")));
    rules.append(stored(QStringLiteral("newMetric"), QStringLiteral("inodesLeft"), QStringLiteral("below"),
        QStringLiteral("live")));
    rules.append(stored(QStringLiteral("newComparison"), QStringLiteral("totalSize"),
        QStringLiteral("outside"), QStringLiteral("live")));
    rules.append(stored(QStringLiteral("newSource"), QStringLiteral("totalSize"), QStringLiteral("above"),
        QStringLiteral("index")));

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("rules")] = rules;

    const QString path = QDir(m_profile->path()).filePath(QStringLiteral("alerts.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QJsonDocument(root).toJson()) > 0);
    file.close();

    AlertStore store(path);
    QVERIFY(store.load());

    // Loading the unknown metric as the first enumerator would have kept the
    // rule's name and its target while watching total size instead; the unknown
    // source would have turned a report-backed watch into a full tree walk on
    // every check. Dropped, and counted, because a watch that stops watching in
    // silence is the failure alerts exist to prevent.
    QCOMPARE(store.rules().size(), 1);
    QCOMPARE(store.rules().first().id, QStringLiteral("good"));
    QCOMPARE(store.unreadable(), 3);
}

void TestAlerts::announcesOnlyTransitions()
{
    AlertStore store(QDir(m_profile->path()).filePath(QStringLiteral("alerts.json")));
    QSignalSpy raised(&store, &AlertStore::alertRaised);
    QSignalSpy cleared(&store, &AlertStore::alertCleared);

    AlertRule rule = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 1);
    rule.state = AlertState::Ok;
    store.put(rule);
    QCOMPARE(raised.count(), 0);

    rule.state = AlertState::Triggered;
    store.put(rule);
    QCOMPARE(raised.count(), 1);

    // Still triggered on the next sweep. Announcing again every minute is how
    // a user learns to ignore the alert entirely.
    store.put(rule);
    QCOMPARE(raised.count(), 1);

    rule.state = AlertState::Ok;
    store.put(rule);
    QCOMPARE(cleared.count(), 1);
}

void TestAlerts::countsWhatIsTriggered()
{
    AlertStore store(QDir(m_profile->path()).filePath(QStringLiteral("alerts.json")));

    AlertRule a = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 1);
    a.id = QStringLiteral("a");
    a.state = AlertState::Triggered;
    store.put(a);

    AlertRule b = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 1);
    b.id = QStringLiteral("b");
    b.state = AlertState::Triggered;
    b.enabled = false;
    store.put(b);

    // A paused alert is not a firing alert, or pausing one would do nothing
    // for the count the user is trying to get back to zero.
    QCOMPARE(store.triggeredCount(), 1);
}

// ------------------------------------------------------------------ task

void TestAlerts::checksEveryRuleOnAPoolThread()
{
    AlertRule big = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 1);
    big.id = QStringLiteral("big");

    AlertRule quiet = ruleFor(AlertMetric::TotalSize, AlertComparison::Above, 1000000);
    quiet.id = QStringLiteral("quiet");

    auto* task = new CheckAlertsTask(m_vfs.get(), m_analysis.get(), { big, quiet });
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 20000));

    QCOMPARE(task->state(), Task::State::Succeeded);
    const QList<AlertRule> results = task->results();
    QCOMPARE(results.size(), 2);
    QCOMPARE(results.at(0).state, AlertState::Triggered);
    QCOMPARE(results.at(1).state, AlertState::Ok);
    QCOMPARE(task->statusText(), QStringLiteral("1 triggered"));
}

MOLE_TEST_MAIN(TestAlerts)
#include "tst_Alerts.moc"
