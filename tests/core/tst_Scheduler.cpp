#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"

#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

namespace {

/// A job that does exactly what the test tells it to, when the test says so.
///
/// Jobs finish asynchronously in production, so the fake holds its completion
/// callback instead of calling it: that is the only way to prove the scheduler
/// does not start the same rule twice while it is still working.
class FakeJob final : public IScheduledJob
{
public:
    QString displayName() const override { return QStringLiteral("Fake"); }

    bool start(const ScheduleRule& rule, std::function<void(bool, QString)> done) override
    {
        ++starts;
        lastRule = rule;
        if (refuseToStart)
            return false;
        pending = std::move(done);
        return true;
    }

    void succeed(const QString& message = QStringLiteral("done"))
    {
        auto callback = std::move(pending);
        pending = nullptr;
        if (callback)
            callback(true, message);
    }

    void fail(const QString& message = QStringLiteral("no"))
    {
        auto callback = std::move(pending);
        pending = nullptr;
        if (callback)
            callback(false, message);
    }

    bool isPending() const { return static_cast<bool>(pending); }

    int starts = 0;
    bool refuseToStart = false;
    ScheduleRule lastRule;

private:
    std::function<void(bool, QString)> pending;
};

ScheduleRule makeRule(const QString& id, qint64 intervalSeconds = 3600)
{
    ScheduleRule rule;
    rule.id = id;
    rule.jobKind = QStringLiteral("fake");
    rule.label = QStringLiteral("Nightly %1").arg(id);
    rule.intervalSeconds = intervalSeconds;
    return rule;
}

} // namespace

class TestScheduler : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void aRuleThatNeverRanIsDueImmediately();
    void aRuleIsNotDueUntilItsIntervalHasPassed();
    void aDisabledRuleIsNeverDue();
    void missingTheWindowWhileClosedStillRuns();

    void runsWhatIsDueAndRecordsSuccess();
    void recordsFailureAndCountsRepeats();
    void successResetsTheFailureCount();
    void willNotStartARuleThatIsAlreadyRunning();
    void aJobThatRefusesToStartIsRecordedAsFailed();
    void anUnhandledKindIsRecordedRatherThanIgnored();
    void runNowIgnoresTheSchedule();

    void survivesARestart();
    void aRunInterruptedByAQuitComesBackAsFailed();
    void historyIsCappedAndNewestFirst();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    QDateTime m_now;
};

void TestScheduler::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = QDir(m_dir->path()).filePath(QStringLiteral("schedule.json"));
    m_now = QDateTime::fromString(QStringLiteral("2026-03-01T12:00:00"), Qt::ISODate);
    QVERIFY(m_now.isValid());
}

// ---- when a rule is due -------------------------------------------------

void TestScheduler::aRuleThatNeverRanIsDueImmediately()
{
    const ScheduleRule rule = makeRule(QStringLiteral("a"));
    QVERIFY(rule.isDueAt(m_now));
}

void TestScheduler::aRuleIsNotDueUntilItsIntervalHasPassed()
{
    ScheduleRule rule = makeRule(QStringLiteral("a"), 3600);
    rule.lastRunAt = m_now;

    QVERIFY(!rule.isDueAt(m_now.addSecs(3599)));
    QVERIFY(rule.isDueAt(m_now.addSecs(3600)));
    QVERIFY(rule.isDueAt(m_now.addSecs(7200)));
}

void TestScheduler::aDisabledRuleIsNeverDue()
{
    ScheduleRule rule = makeRule(QStringLiteral("a"));
    rule.enabled = false;
    QVERIFY(!rule.isDueAt(m_now.addYears(1)));
}

void TestScheduler::missingTheWindowWhileClosedStillRuns()
{
    // The machine was off for a week; a daily job must run once on the next
    // start rather than waiting a further day for its "proper" slot.
    ScheduleStore store(m_path);
    ScheduleRule rule = makeRule(QStringLiteral("a"), 86400);
    rule.lastRunAt = m_now.addDays(-7);
    store.put(rule);

    FakeJob job;
    Scheduler scheduler(&store);
    scheduler.setClock([this] { return m_now; });
    scheduler.registerJob(QStringLiteral("fake"), &job);

    QCOMPARE(scheduler.checkDue(), 1);
    QCOMPARE(job.starts, 1);
}

// ---- running ------------------------------------------------------------

void TestScheduler::runsWhatIsDueAndRecordsSuccess()
{
    ScheduleStore store(m_path);
    store.put(makeRule(QStringLiteral("a")));

    FakeJob job;
    Scheduler scheduler(&store);
    scheduler.setClock([this] { return m_now; });
    scheduler.registerJob(QStringLiteral("fake"), &job);

    QSignalSpy started(&scheduler, &Scheduler::runStarted);
    QSignalSpy finished(&scheduler, &Scheduler::runFinished);

    QCOMPARE(scheduler.checkDue(), 1);
    QCOMPARE(started.count(), 1);
    QCOMPARE(scheduler.runningRules(), QStringList { QStringLiteral("a") });
    QCOMPARE(store.rule(QStringLiteral("a")).lastStatus, RunStatus::Running);

    m_now = m_now.addSecs(30);
    job.succeed(QStringLiteral("42 files"));

    QCOMPARE(finished.count(), 1);
    QVERIFY(scheduler.runningRules().isEmpty());

    const ScheduleRule after = store.rule(QStringLiteral("a"));
    QCOMPARE(after.lastStatus, RunStatus::Succeeded);
    QCOMPARE(after.lastMessage, QStringLiteral("42 files"));
    QCOMPARE(after.consecutiveFailures, 0);
    QVERIFY(after.lastSuccessAt.isValid());

    const QList<RunRecord> history = store.history();
    QCOMPARE(history.size(), 1);
    QCOMPARE(history.first().status, RunStatus::Succeeded);
    QCOMPARE(history.first().durationMs(), 30000);
}

void TestScheduler::recordsFailureAndCountsRepeats()
{
    ScheduleStore store(m_path);
    store.put(makeRule(QStringLiteral("a"), 60));

    FakeJob job;
    Scheduler scheduler(&store);
    scheduler.setClock([this] { return m_now; });
    scheduler.registerJob(QStringLiteral("fake"), &job);

    for (int attempt = 1; attempt <= 3; ++attempt) {
        QCOMPARE(scheduler.checkDue(), 1);
        job.fail(QStringLiteral("drive is not there"));
        m_now = m_now.addSecs(120);

        const ScheduleRule after = store.rule(QStringLiteral("a"));
        QCOMPARE(after.lastStatus, RunStatus::Failed);
        // A job failing every night must be distinguishable from one that
        // failed once, or the tracking list cannot rank what needs attention.
        QCOMPARE(after.consecutiveFailures, attempt);
    }

    QCOMPARE(store.history().size(), 3);
}

void TestScheduler::successResetsTheFailureCount()
{
    ScheduleStore store(m_path);
    ScheduleRule rule = makeRule(QStringLiteral("a"), 60);
    rule.consecutiveFailures = 5;
    rule.lastStatus = RunStatus::Failed;
    store.put(rule);

    FakeJob job;
    Scheduler scheduler(&store);
    scheduler.setClock([this] { return m_now; });
    scheduler.registerJob(QStringLiteral("fake"), &job);

    scheduler.checkDue();
    job.succeed();

    QCOMPARE(store.rule(QStringLiteral("a")).consecutiveFailures, 0);
}

void TestScheduler::willNotStartARuleThatIsAlreadyRunning()
{
    ScheduleStore store(m_path);
    store.put(makeRule(QStringLiteral("a"), 60));

    FakeJob job;
    Scheduler scheduler(&store);
    scheduler.setClock([this] { return m_now; });
    scheduler.registerJob(QStringLiteral("fake"), &job);

    QCOMPARE(scheduler.checkDue(), 1);

    // An hour of polling passes while the first run is still walking a large
    // tree. Starting it again would double the work and corrupt the history.
    for (int i = 0; i < 60; ++i) {
        m_now = m_now.addSecs(60);
        QCOMPARE(scheduler.checkDue(), 0);
    }
    QCOMPARE(job.starts, 1);
    QVERIFY(job.isPending());

    job.succeed();
    QCOMPARE(scheduler.checkDue(), 1); // and it is due again straight away
    QCOMPARE(job.starts, 2);
}

void TestScheduler::aJobThatRefusesToStartIsRecordedAsFailed()
{
    ScheduleStore store(m_path);
    store.put(makeRule(QStringLiteral("a")));

    FakeJob job;
    job.refuseToStart = true;
    Scheduler scheduler(&store);
    scheduler.setClock([this] { return m_now; });
    scheduler.registerJob(QStringLiteral("fake"), &job);

    scheduler.checkDue();

    // The rule must not be left stuck in Running, or it would never run again.
    const ScheduleRule after = store.rule(QStringLiteral("a"));
    QCOMPARE(after.lastStatus, RunStatus::Failed);
    QVERIFY(scheduler.runningRules().isEmpty());
    QCOMPARE(store.history().size(), 1);
}

void TestScheduler::anUnhandledKindIsRecordedRatherThanIgnored()
{
    ScheduleStore store(m_path);
    store.put(makeRule(QStringLiteral("a"))); // nothing registered for "fake"

    Scheduler scheduler(&store);
    scheduler.setClock([this] { return m_now; });

    QSignalSpy finished(&scheduler, &Scheduler::runFinished);
    QCOMPARE(scheduler.checkDue(), 0);

    // A rule left over from an uninstalled plugin has to say so. Silently
    // never running is the one outcome the user cannot diagnose.
    const ScheduleRule after = store.rule(QStringLiteral("a"));
    QCOMPARE(after.lastStatus, RunStatus::Skipped);
    QVERIFY(after.lastMessage.contains(QStringLiteral("fake")));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(store.history().size(), 1);
}

void TestScheduler::runNowIgnoresTheSchedule()
{
    ScheduleStore store(m_path);
    ScheduleRule rule = makeRule(QStringLiteral("a"), 86400);
    rule.lastRunAt = m_now; // nowhere near due
    store.put(rule);

    FakeJob job;
    Scheduler scheduler(&store);
    scheduler.setClock([this] { return m_now; });
    scheduler.registerJob(QStringLiteral("fake"), &job);

    QCOMPARE(scheduler.checkDue(), 0);
    QVERIFY(scheduler.runNow(QStringLiteral("a")));
    QCOMPARE(job.starts, 1);

    QVERIFY2(!scheduler.runNow(QStringLiteral("a")), "not while it is still going");
    QVERIFY2(!scheduler.runNow(QStringLiteral("nope")), "and not a rule that does not exist");
}

// ---- persistence --------------------------------------------------------

void TestScheduler::survivesARestart()
{
    {
        ScheduleStore store(m_path);
        ScheduleRule rule = makeRule(QStringLiteral("a"), 7200);
        rule.parameters.insert(QStringLiteral("rootUri"), QStringLiteral("file:///data"));
        rule.lastRunAt = m_now;
        rule.lastStatus = RunStatus::Succeeded;
        rule.lastMessage = QStringLiteral("all good");
        store.put(rule);
    }

    ScheduleStore reopened(m_path);
    QVERIFY(reopened.load());
    QCOMPARE(reopened.rules().size(), 1);

    const ScheduleRule rule = reopened.rule(QStringLiteral("a"));
    QCOMPARE(rule.jobKind, QStringLiteral("fake"));
    QCOMPARE(rule.intervalSeconds, 7200);
    QCOMPARE(rule.parameters.value(QStringLiteral("rootUri")).toString(), QStringLiteral("file:///data"));
    QCOMPARE(rule.lastStatus, RunStatus::Succeeded);
    QCOMPARE(rule.lastRunAt.toSecsSinceEpoch(), m_now.toSecsSinceEpoch());
}

void TestScheduler::aRunInterruptedByAQuitComesBackAsFailed()
{
    {
        ScheduleStore store(m_path);
        store.put(makeRule(QStringLiteral("a")));

        FakeJob job;
        Scheduler scheduler(&store);
        scheduler.setClock([this] { return m_now; });
        scheduler.registerJob(QStringLiteral("fake"), &job);
        scheduler.checkDue(); // left Running: the application "quits" here
        QCOMPARE(store.rule(QStringLiteral("a")).lastStatus, RunStatus::Running);
    }

    // Reloaded, it must not still claim to be running -- a rule in that state
    // is skipped by every future poll and would never recover.
    ScheduleStore reopened(m_path);
    QVERIFY(reopened.load());
    const ScheduleRule rule = reopened.rule(QStringLiteral("a"));
    QCOMPARE(rule.lastStatus, RunStatus::Failed);

    // It recovers on its own clock rather than at once. This case used to assert it
    // was due immediately, which was how "it recovered" showed up when a started run
    // left nothing behind: no `lastRunAt` reads as never run, and never run is due
    // now. That turned out to be a loop -- due, fires, killed, still due -- and
    // MOLE-268 records the start, so the next attempt is an interval away.
    //
    // The claim this case exists for is unchanged and is the line above: a rule does
    // not come back stuck in `Running`. Being due one interval later is recovery; being
    // due again the instant the application restarts is what made a scan that killed
    // the window impossible to escape.
    QVERIFY2(!rule.isDueAt(m_now), "a run that just started is not owed another one");
    QVERIFY2(rule.isDueAt(m_now.addSecs(3600)), "and it is owed one when its interval is up");
}

void TestScheduler::historyIsCappedAndNewestFirst()
{
    ScheduleStore store(m_path);
    store.setHistoryLimit(5);
    store.put(makeRule(QStringLiteral("a")));

    for (int i = 0; i < 12; ++i) {
        store.record(RunRecord { QStringLiteral("a"), QStringLiteral("Nightly a"), m_now.addSecs(i),
            m_now.addSecs(i + 1), RunStatus::Succeeded, QStringLiteral("run %1").arg(i) });
    }

    const QList<RunRecord> history = store.history();
    QCOMPARE(history.size(), 5);
    QCOMPARE(history.first().message, QStringLiteral("run 11"));
    QCOMPARE(history.last().message, QStringLiteral("run 7"));

    store.record(RunRecord { QStringLiteral("b"), QStringLiteral("Other"), m_now, m_now, RunStatus::Failed,
        QStringLiteral("nope") });
    QCOMPARE(store.history(QStringLiteral("a")).size(), 4);
    QCOMPARE(store.history(QStringLiteral("b")).size(), 1);
}

MOLE_TEST_MAIN(TestScheduler)
#include "tst_Scheduler.moc"
