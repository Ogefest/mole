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

    // ---- judging ----
    void triggersWhenAboveTheThreshold();
    void staysQuietWhenWithinBounds();
    void aFirstReadingEstablishesTheBaselineForChanged();
    void changedTripsOnTheSecondDifferentReading();
    void anUnreadableMetricIsNotOk();

    // ---- storing ----
    void survivesARestart();
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
    Mount mount;
    mount.id = QStringLiteral("local");
    mount.displayName = QStringLiteral("Local");
    mount.root = VfsUri::fromLocalPath(QStringLiteral("/"));
    mount.fileSystem = std::make_shared<LocalFileSystem>();
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
