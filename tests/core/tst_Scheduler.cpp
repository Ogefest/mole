#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"

#include <QFile>
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

    StartOutcome start(const ScheduleRule& rule, std::function<void(bool, QString)> done) override
    {
        ++starts;
        lastRule = rule;
        if (refuseToStart)
            return StartOutcome::failed(QStringLiteral("the fake was told to refuse"));
        if (!skipReason.isEmpty())
            return StartOutcome::skipped(skipReason);
        pending = std::move(done);
        return StartOutcome::started();
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
    /// Non-empty makes start() answer Skipped with this reason -- an unplugged
    /// drive, in the words the built-in jobs use.
    QString skipReason;
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
    void aJobThatCouldNotRunIsSkippedAndDoesNotCountAgainstTheRule();
    void anIntervalOfZeroIsRefusedOnTheWayIntoTheStore();
    void anInterruptedRunSaysSoAndIsCounted();
    void aPendingCallbackAfterTheSchedulerIsGoneIsHarmless();
    void anUnhandledKindIsRecordedRatherThanIgnored();
    void runNowIgnoresTheSchedule();

    void survivesARestart();
    void aRunInterruptedByAQuitComesBackAsFailed();
    void historyIsCappedAndNewestFirst();

    void aScheduleThatCannotBeParsedIsKeptRatherThanReplaced();

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

/// "The backup disk is unplugged" is not "this rule is broken".
///
/// Both built-in jobs returned false for an unmounted root, believing -- their
/// comments said so -- that the scheduler would record Skipped. It recorded
/// Failed with "The job refused to start" and incremented consecutiveFailures,
/// which is the number the tracking tab ranks by. So a laptop whose backup disk
/// was out for a week showed "Failed x7" above a rule that really was broken,
/// with a message that did not say why -- and the job's own reason had no way to
/// travel out of start(). See MOLE-379.
void TestScheduler::aJobThatCouldNotRunIsSkippedAndDoesNotCountAgainstTheRule()
{
    ScheduleStore store(m_path);
    ScheduleRule rule = makeRule(QStringLiteral("a"));
    rule.consecutiveFailures = 2; // a rule that had already failed twice
    store.put(rule);

    FakeJob job;
    job.skipReason = QStringLiteral("No drive is mounted for backup:///photos");
    Scheduler scheduler(&store);
    scheduler.setClock([this] { return m_now; });
    scheduler.registerJob(QStringLiteral("fake"), &job);

    QCOMPARE(scheduler.checkDue(), 0);
    QCOMPARE(job.starts, 1);

    const ScheduleRule after = store.rule(QStringLiteral("a"));
    QCOMPARE(after.lastStatus, RunStatus::Skipped);
    QCOMPARE(after.lastMessage, job.skipReason);
    QVERIFY2(after.consecutiveFailures == 2, "a skipped run counted against the rule");
    // Not stuck in Running, and it waits for its interval rather than being
    // retried on every poll.
    QVERIFY(scheduler.runningRules().isEmpty());
    QCOMPARE(after.lastRunAt, m_now);
    QVERIFY(!after.isDueAt(m_now));

    // And the run log says skipped rather than failed.
    QCOMPARE(store.history().size(), 1);
    QCOMPARE(store.history().first().status, RunStatus::Skipped);

    // A job that says Failed still counts, because that is a rule that cannot
    // work rather than a world that is not ready.
    job.skipReason.clear();
    job.refuseToStart = true;
    scheduler.setClock([this] { return m_now.addSecs(7200); });
    scheduler.checkDue();
    QCOMPARE(store.rule(QStringLiteral("a")).lastStatus, RunStatus::Failed);
    QCOMPARE(store.rule(QStringLiteral("a")).consecutiveFailures, 3);
}

/// The floor lived only in fromJson().
///
/// "A hand-edited 0 would spin the scheduler" is the comment there, and it is
/// right -- but three callers build a rule in memory and set intervalSeconds
/// directly, and put() took whatever they gave it. A rule with no interval is
/// always due, so it fires on every poll for the life of the process and nothing
/// on disk would ever show why. See MOLE-379.
void TestScheduler::anIntervalOfZeroIsRefusedOnTheWayIntoTheStore()
{
    ScheduleStore store(m_path);
    ScheduleRule rule = makeRule(QStringLiteral("a"), 0);
    QVERIFY(store.put(rule));
    QCOMPARE(store.rule(QStringLiteral("a")).intervalSeconds, ScheduleRule::kMinimumIntervalSeconds);

    ScheduleRule negative = makeRule(QStringLiteral("b"), -3600);
    QVERIFY(store.put(negative));
    QCOMPARE(store.rule(QStringLiteral("b")).intervalSeconds, ScheduleRule::kMinimumIntervalSeconds);

    // An interval somebody actually chose is untouched.
    QVERIFY(store.put(makeRule(QStringLiteral("c"), 6 * 3600)));
    QCOMPARE(store.rule(QStringLiteral("c")).intervalSeconds, qint64(6 * 3600));

    // And it survives the file, which is where the floor already worked.
    ScheduleStore reopened(m_path);
    QVERIFY(reopened.load());
    QCOMPARE(reopened.rule(QStringLiteral("a")).intervalSeconds, ScheduleRule::kMinimumIntervalSeconds);
}

/// An interrupted run showed a failure with no reason and no count.
///
/// dispatch() writes the rule as Running, serialisation turns Running into
/// Failed on the way out -- a process that died mid-run did not succeed -- and
/// the reload left lastMessage empty and consecutiveFailures untouched. So the
/// tracking tab showed a failed rule that would not say why, and the streak it
/// ranks by did not count it. See MOLE-379.
void TestScheduler::anInterruptedRunSaysSoAndIsCounted()
{
    {
        ScheduleStore store(m_path);
        store.put(makeRule(QStringLiteral("a")));

        FakeJob job;
        Scheduler scheduler(&store);
        scheduler.setClock([this] { return m_now; });
        scheduler.registerJob(QStringLiteral("fake"), &job);
        scheduler.checkDue(); // left Running: the application "quits" here
    }

    ScheduleStore reopened(m_path);
    QVERIFY(reopened.load());
    const ScheduleRule rule = reopened.rule(QStringLiteral("a"));
    QCOMPARE(rule.lastStatus, RunStatus::Failed);
    QVERIFY2(!rule.lastMessage.isEmpty(), "a failure the tab cannot explain is a failure nobody can act on");
    QVERIFY2(rule.lastMessage.contains(QStringLiteral("Interrupted")), qPrintable(rule.lastMessage));
    QCOMPARE(rule.consecutiveFailures, 1);
}

/// A callback outliving the scheduler.
///
/// dispatch() handed the job a lambda capturing a raw `this`, and both built-in
/// jobs store that callback inside a Task::finished connection whose context is
/// the *job* -- which outlives the Scheduler in at least one teardown order. A
/// scheduled scan still running when the Scheduler went would then call finish()
/// on a dead object. See MOLE-379.
void TestScheduler::aPendingCallbackAfterTheSchedulerIsGoneIsHarmless()
{
    ScheduleStore store(m_path);
    store.put(makeRule(QStringLiteral("a")));

    FakeJob job;
    {
        Scheduler scheduler(&store);
        scheduler.setClock([this] { return m_now; });
        scheduler.registerJob(QStringLiteral("fake"), &job);
        QCOMPARE(scheduler.checkDue(), 1);
        QVERIFY(job.isPending());
    }

    // The job answers after the scheduler is gone, which is exactly what a task
    // finishing during teardown does.
    job.succeed(QStringLiteral("finished after nobody was listening"));
    QVERIFY2(!job.isPending(), "the callback was never called");
    // Nothing was written, because there was nobody left to write it -- and
    // nothing crashed, which is the whole of the claim.
    QCOMPARE(store.rule(QStringLiteral("a")).lastStatus, RunStatus::Running);
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

/// A schedule is data somebody built, one rule at a time.
///
/// load() used to clear the list, fail to parse, return false and keep nothing,
/// and the first put() afterwards wrote the empty list over the file. Every
/// scheduled job the user had set up would simply stop, which is
/// ARCHITECTURE.md's "a job that quietly never runs is the one failure nobody
/// can diagnose" arriving through the front door. See ADR-0089.
void TestScheduler::aScheduleThatCannotBeParsedIsKeptRatherThanReplaced()
{
    const QByteArray typedByHand("{ \"rules\": [ { \"id\": \"nightly\", } ] }");
    {
        QFile file(m_path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(typedByHand);
    }

    ScheduleStore store(m_path);
    QVERIFY2(!store.load(), "a file that could not be parsed is not a load that succeeded");

    const QString kept = store.damagedCopyPath();
    QVERIFY2(!kept.isEmpty(), "the unreadable schedule has to be somewhere");
    QFile keptFile(kept);
    QVERIFY(keptFile.open(QIODevice::ReadOnly));
    QCOMPARE(keptFile.readAll(), typedByHand);

    // And it goes on working: the rules somebody adds after this reach the file.
    QVERIFY(store.put(makeRule(QStringLiteral("a"))));
    ScheduleStore reopened(m_path);
    QVERIFY(reopened.load());
    QCOMPARE(reopened.rules().size(), 1);
}

MOLE_TEST_MAIN(TestScheduler)
#include "tst_Scheduler.moc"
