#include "plugins/builtin/AnalysisFeature.h"
#include "plugins/builtin/AnalysisJob.h"
#include "plugins/builtin/AutomationFeature.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/IndexScanJob.h"
#include "plugins/builtin/SearchFeatures.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/analysis/AnalysisStore.h"
#include "core/automation/ScheduleRule.h"
#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Scheduling as the user meets it: a report put on a clock, run without a tab
/// open, and a tracking list that shows when it broke.
class TestAutomation : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void schedulingAFolderCreatesOneRule();
    void schedulingTheSameFolderTwiceUpdatesTheRule();
    void unschedulingRemovesTheRule();
    void aScheduledReportRunsWithoutATabOpen();
    void aRunThatCouldNotStartShowsUpAsSkippedRatherThanFailed();
    void thePluginsOwnJobShowsItsTargetInTheTrackingList();
    void theTrackingTabPutsFailuresFirst();
    void ruleSurvivesARestartOfTheApplication();
    void aScheduledRescanRunsSurvivesARestartAndCatchesUp();
    void anIntervalCanBeChosenChangedAndTurnedOffForAFolder();
    void nothingScheduledRunsWhileTheWindowIsStillComingUp();
    void aRunKilledPartWayIsNotDueAgainImmediately();
    void aRuleThatHasGenuinelyNeverRunIsStillDueNow();
    void aScheduledRunKeepsAsMuchHistoryAsAManualOne();

private:
    /// The search tab, which is where a folder is put on a clock for indexing.
    LiveSearchController* search();
    AnalysisTarget* analyse(const QString& uri);
    AutomationController* openAutomation();

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
};

void TestAutomation::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

void TestAutomation::init()
{
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SCHEDULE_PATH")));
    QDir(QString::fromLocal8Bit(qgetenv("MOLE_ANALYSIS_PATH"))).removeRecursively();

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->writeFile(QStringLiteral("media/film.mkv"), QByteArray(9000, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("docs/a.txt"), QByteArray(100, 'x')));

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));

    // The real poll would fire during the test and make the timing its own
    // variable; every test here drives the scheduler explicitly instead.
    m_app->scheduler()->stop();
}

void TestAutomation::cleanup()
{
    m_app.reset();
    m_tree.reset();
}

LiveSearchController* TestAutomation::search()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    return row < 0 ? nullptr : qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
}

AnalysisTarget* TestAutomation::analyse(const QString& uri)
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.analysis"));
    if (row < 0)
        return nullptr;
    auto* tab = qobject_cast<AnalysisTabController*>(m_app->tabs()->controllerAt(row));
    if (!tab)
        return nullptr;
    tab->setTargets({ uri });
    return tab->current();
}

AutomationController* TestAutomation::openAutomation()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("core.automation"));
    return row < 0 ? nullptr : qobject_cast<AutomationController*>(m_app->tabs()->controllerAt(row));
}

void TestAutomation::schedulingAFolderCreatesOneRule()
{
    AnalysisTarget* target = analyse(m_tree->rootUri().toString());
    QVERIFY(target);
    QCOMPARE(target->scheduleText(), QString());

    target->setSchedule(86400);

    QCOMPARE(target->scheduleSeconds(), 86400);
    QCOMPARE(target->scheduleText(), QStringLiteral("Every day"));

    const QList<ScheduleRule> rules = m_app->schedules()->rules();
    QCOMPARE(rules.size(), 1);
    QCOMPARE(rules.first().jobKind, AnalysisJob::kind());
    QCOMPARE(rules.first().parameters.value(AnalysisJob::rootUriParameter()).toString(),
        m_tree->rootUri().toString());
    QVERIFY(rules.first().enabled);
}

void TestAutomation::schedulingTheSameFolderTwiceUpdatesTheRule()
{
    AnalysisTarget* target = analyse(m_tree->rootUri().toString());
    QVERIFY(target);

    target->setSchedule(3600);
    target->setSchedule(604800);

    // One folder, one job. Two rules walking the same tree would double the
    // work and produce two competing histories.
    QCOMPARE(m_app->schedules()->rules().size(), 1);
    QCOMPARE(m_app->schedules()->rules().first().intervalSeconds, 604800);
    QCOMPARE(target->scheduleText(), QStringLiteral("Every week"));
}

void TestAutomation::unschedulingRemovesTheRule()
{
    AnalysisTarget* target = analyse(m_tree->rootUri().toString());
    QVERIFY(target);

    target->setSchedule(3600);
    QCOMPARE(m_app->schedules()->rules().size(), 1);

    target->setSchedule(0);
    QCOMPARE(m_app->schedules()->rules().size(), 0);
    QCOMPARE(target->scheduleText(), QString());
}

void TestAutomation::aScheduledReportRunsWithoutATabOpen()
{
    const QString uri = m_tree->rootUri().toString();
    AnalysisTarget* target = analyse(uri);
    QVERIFY(target);
    target->setSchedule(3600);

    // The tab is closed: the whole point of scheduling is that the work no
    // longer depends on someone looking at it.
    m_app->tabs()->closeTab(m_app->tabs()->currentIndex());

    ScheduleRule rule = m_app->schedules()->rules().first();
    rule.lastRunAt = QDateTime::currentDateTime().addSecs(-7200); // overdue
    m_app->schedules()->put(rule);

    QCOMPARE(m_app->scheduler()->checkDue(), 1);

    QVERIFY(waitFor([this] { return m_app->scheduler()->runningRules().isEmpty(); }, 20000));

    const ScheduleRule after = m_app->schedules()->rules().first();
    QCOMPARE(after.lastStatus, RunStatus::Succeeded);
    QVERIFY(after.lastSuccessAt.isValid());

    // And the report is filed where a tab opened later will find it.
    AnalysisStore store(QString::fromLocal8Bit(qgetenv("MOLE_ANALYSIS_PATH")));
    QVERIFY2(!store.history(uri).isEmpty(), "the scheduled run must leave a report behind");
}

namespace {

/// A job that starts and never answers -- which is what a process killed
/// mid-run looks like from the store's point of view. `done` is dropped on
/// purpose: the point is the state left on disk when nobody ever calls it.
class JobThatNeverFinishes final : public IScheduledJob
{
public:
    static QString kind() { return QStringLiteral("never-finishes"); }
    QString displayName() const override { return QStringLiteral("A job that hangs"); }
    StartOutcome start(const ScheduleRule&, std::function<void(bool, QString)>) override
    {
        ++started;
        return StartOutcome::started();
    }
    int started = 0;
};

/// A job whose parameters are its own, the way a plugin's would be.
class JobWithItsOwnParameters final : public IScheduledJob
{
public:
    static QString kind() { return QStringLiteral("plugin-shaped"); }
    QString displayName() const override { return QStringLiteral("A plugin's job"); }
    StartOutcome start(const ScheduleRule&, std::function<void(bool, QString)>) override
    {
        return StartOutcome::started();
    }
};

} // namespace

/// A killed run must not look exactly like one that never happened.
///
/// `dispatch()` wrote the rule back before handing it to the job and set only
/// `lastStatus`; `lastRunAt` was set in `finish()` and nowhere else. So what reached
/// disk the moment a run started was `Running` with no `lastRunAt` -- and
/// serialisation turns `Running` into `Failed` on the way out, deliberately, discarding
/// the one fact that would stop the rule re-firing. `dueAt()` treats a rule with no
/// `lastRunAt` as due now and staying due, which is right on its own terms. The three
/// together are a loop: due, fires, killed, still due.
///
/// Found through MOLE-264, where a scan made the window unreachable so killing the
/// process was the only way out, and every restart began the scan again. See MOLE-268.
void TestAutomation::aRunKilledPartWayIsNotDueAgainImmediately()
{
    m_app->scheduler()->stop();

    JobThatNeverFinishes job;
    m_app->scheduler()->registerJob(JobThatNeverFinishes::kind(), &job);

    ScheduleRule rule;
    rule.id = QStringLiteral("killed-part-way");
    rule.jobKind = JobThatNeverFinishes::kind();
    rule.label = QStringLiteral("Something that will be interrupted");
    rule.intervalSeconds = 86400;
    m_app->schedules()->put(rule);

    // It is due, because it has never run. That is the behaviour being built on.
    QCOMPARE(m_app->scheduler()->checkDue(), 1);
    QCOMPARE(job.started, 1);

    // And now the process dies. Read the rule back off disk, which is all a new
    // process gets: a fresh store over the same file, so nothing in memory can carry
    // the answer for it.
    const QString path = QString::fromLocal8Bit(qgetenv("MOLE_SCHEDULE_PATH"));
    QVERIFY(!path.isEmpty());
    ScheduleStore reopened(path);
    QVERIFY(reopened.load());
    const ScheduleRule survived = reopened.rule(rule.id);
    QVERIFY2(survived.isValid(), "the rule itself has to survive a restart");

    QVERIFY2(survived.lastRunAt.isValid(),
        "a run that started has to say so on disk, or nothing can tell it from one that never did");
    QVERIFY2(!survived.isDueAt(QDateTime::currentDateTime()),
        "the rule is due again immediately, so the next start runs it again -- and the one after that");
}

/// The other half, and the reason the fix is one field away from breaking it: a rule
/// that has genuinely never run is due now, so a nightly job missed while the
/// application was closed happens at the next start rather than waiting a whole
/// further interval.
void TestAutomation::aRuleThatHasGenuinelyNeverRunIsStillDueNow()
{
    ScheduleRule rule;
    rule.id = QStringLiteral("never-run");
    rule.jobKind = AnalysisJob::kind();
    rule.label = QStringLiteral("Something nobody has run yet");
    rule.intervalSeconds = 86400;
    QVERIFY(!rule.lastRunAt.isValid());
    QVERIFY2(rule.isDueAt(QDateTime::currentDateTime()),
        "a rule that has never run has to be due, or a missed night is skipped entirely");
}

/// A scheduled run and a manual one keep the same history.
///
/// `AnalysisFeature.cpp` kept 50 and `AnalysisJob.h` defaulted to 30, and nothing
/// ever called `setHistoryKept()` -- so a folder with 45 manual runs lost runs 31
/// to 45 the first night its schedule fired. History is the reason reports are
/// kept at all, and `AnalysisJob.h`'s own header says the job "deliberately shares
/// the store with the analysis tab … or the two would drift apart and the diffs
/// would lie". The depth is the one parameter that drifted. See MOLE-380.
void TestAutomation::aScheduledRunKeepsAsMuchHistoryAsAManualOne()
{
    AnalysisStore* store = m_app->services().reports;
    QVERIFY(store);
    const QString root = m_tree->rootUri().toString();

    // More reports than the job's old default of thirty, filed the way a manual
    // run files them.
    constexpr int kFiled = 40;
    for (int i = 0; i < kFiled; ++i) {
        AnalysisReport report;
        report.id = QStringLiteral("2026010%1-0000%2-000").arg(i / 10).arg(i, 2, 10, QLatin1Char('0'));
        report.rootUri = root;
        report.label = QStringLiteral("fixture");
        report.createdAt = QDateTime::currentDateTime().addSecs(-3600 * (kFiled - i));
        report.fileCount = i;
        QVERIFY(store->save(report));
    }
    QCOMPARE(store->history(root).size(), kFiled);

    // What the scheduler prunes to, asked of the job itself rather than of a
    // number typed here.
    AnalysisJob job(m_app->services(), store);
    bool finished = false;
    QCOMPARE(job.start(
                    [&] {
                        ScheduleRule rule;
                        rule.id = QStringLiteral("nightly");
                        rule.jobKind = AnalysisJob::kind();
                        rule.parameters.insert(AnalysisJob::rootUriParameter(), root);
                        return rule;
                    }(),
                    [&finished](bool, const QString&) { finished = true; })
                 .what,
        StartOutcome::What::Started);
    QVERIFY(waitFor([&finished] { return finished; }, 20000));

    // One more report than were filed, and none of the old ones gone: the depth
    // the tab keeps is the depth the job keeps.
    QCOMPARE(store->history(root).size(), kFiled + 1);
    QVERIFY(AnalysisStore::kHistoryKept > kFiled);
}

/// A start that has work waiting must still put the window up first.
///
/// `Scheduler::start()` called `checkDue()` synchronously, from inside
/// `AppController::initialise()` -- so an overdue rule submitted its job before there
/// was a window. On a large tree that is a start that drags, and it was worse: the
/// scan holds the index's one mutex for the length of the walk and session restore
/// asks the index which volumes there are, so the window never appeared at all. The
/// first check now waits. See MOLE-264.
void TestAutomation::nothingScheduledRunsWhileTheWindowIsStillComingUp()
{
    m_app->scheduler()->stop();

    ScheduleRule rule;
    rule.id = QStringLiteral("overdue");
    rule.jobKind = AnalysisJob::kind();
    rule.label = QStringLiteral("Something that was due while we were closed");
    rule.parameters.insert(AnalysisJob::rootUriParameter(), m_tree->rootUri().toString());
    rule.intervalSeconds = 3600;
    rule.lastRunAt = QDateTime::currentDateTime().addSecs(-7200);
    m_app->schedules()->put(rule);

    // A grace short enough for a test to wait out and long enough that the check
    // below is not a race: what is asserted is that nothing has run *yet*, and that
    // is true the instant start() returns whatever the grace is.
    m_app->scheduler()->start(60000, 250);
    QVERIFY2(
        m_app->scheduler()->runningRules().isEmpty(), "a job was dispatched before the window could exist");
    QCOMPARE(m_app->schedules()->rules().first().lastStatus, RunStatus::Never);

    // And then it does run, because a rule that came due while the application was
    // closed still has to happen -- just not first.
    QVERIFY2(
        waitFor([this] { return m_app->schedules()->rules().first().lastStatus != RunStatus::Never; }, 20000),
        "the overdue rule never ran at all");
}

/// A run that could not start, and what the tab says about it.
///
/// This case used to be called "a failed run shows up in the tracking list" and
/// pinned the behaviour the jobs' own comments call wrong: a rule pointing at an
/// unmounted drive was recorded Failed, with "The job refused to start", and
/// counted towards the streak the tab ranks by. A backup disk left out for a
/// week read as "Failed x7" above a rule that really was broken. It is visible
/// either way -- that was never the fault -- but it now says the true thing, in
/// the job's own words. See MOLE-379.
void TestAutomation::aRunThatCouldNotStartShowsUpAsSkippedRatherThanFailed()
{
    ScheduleRule rule;
    rule.id = QStringLiteral("broken");
    rule.jobKind = AnalysisJob::kind();
    rule.label = QStringLiteral("Report on a drive that is gone");
    rule.parameters.insert(AnalysisJob::rootUriParameter(), QStringLiteral("nosuchscheme://host/data"));
    m_app->schedules()->put(rule);

    QCOMPARE(m_app->scheduler()->checkDue(), 0);

    const ScheduleRule after = m_app->schedules()->rule(QStringLiteral("broken"));
    QCOMPARE(after.lastStatus, RunStatus::Skipped);
    QVERIFY2(
        after.lastMessage.contains(QStringLiteral("No drive is mounted")), qPrintable(after.lastMessage));
    QVERIFY2(after.consecutiveFailures == 0, "an unplugged drive counted against the rule");

    AutomationController* automation = openAutomation();
    QVERIFY(automation);
    // Still shown, and still shown first: a rule that cannot run is the reason
    // somebody opens this tab.
    QCOMPARE(automation->failingCount(), 1);

    const QVariantList rules = automation->rules();
    QCOMPARE(rules.size(), 1);
    const QVariantMap row = rules.first().toMap();
    QCOMPARE(row.value(QStringLiteral("failing")).toBool(), true);
    QCOMPARE(row.value(QStringLiteral("status")).toString(), QStringLiteral("skipped"));
    QVERIFY2(row.value(QStringLiteral("message")).toString().contains(QStringLiteral("No drive is mounted")),
        qPrintable(row.value(QStringLiteral("message")).toString()));
    QCOMPARE(row.value(QStringLiteral("consecutiveFailures")).toInt(), 0);
    // The target comes from the job rather than from a key this tab knows.
    QCOMPARE(row.value(QStringLiteral("target")).toString(), QStringLiteral("nosuchscheme://host/data"));

    const QVariantList history = automation->history();
    QCOMPARE(history.size(), 1);
}

/// A plugin's job showed an empty target.
///
/// The tracking tab read `rule.parameters.value("rootUri")` -- the one place a
/// generic tab knew a built-in job's parameter key. A job registered by a plugin,
/// with parameters of its own naming, had a blank column. IScheduledJob answers
/// it now, defaulting to the first string parameter. See MOLE-379.
void TestAutomation::thePluginsOwnJobShowsItsTargetInTheTrackingList()
{
    JobWithItsOwnParameters job;
    m_app->scheduler()->registerJob(JobWithItsOwnParameters::kind(), &job);

    ScheduleRule rule;
    rule.id = QStringLiteral("plugin-job");
    rule.jobKind = JobWithItsOwnParameters::kind();
    rule.label = QStringLiteral("Something a plugin does");
    rule.parameters.insert(QStringLiteral("mailbox"), QStringLiteral("imap://host/INBOX"));
    m_app->schedules()->put(rule);

    AutomationController* automation = openAutomation();
    QVERIFY(automation);
    const QVariantList rules = automation->rules();
    QCOMPARE(rules.size(), 1);
    QCOMPARE(rules.first().toMap().value(QStringLiteral("target")).toString(),
        QStringLiteral("imap://host/INBOX"));
}

void TestAutomation::theTrackingTabPutsFailuresFirst()
{
    ScheduleRule healthy;
    healthy.id = QStringLiteral("ok");
    healthy.jobKind = AnalysisJob::kind();
    healthy.label = QStringLiteral("AAA healthy");
    healthy.lastStatus = RunStatus::Succeeded;
    healthy.lastRunAt = QDateTime::currentDateTime();
    m_app->schedules()->put(healthy);

    ScheduleRule broken;
    broken.id = QStringLiteral("bad");
    broken.jobKind = AnalysisJob::kind();
    broken.label = QStringLiteral("ZZZ broken");
    broken.lastStatus = RunStatus::Failed;
    broken.lastMessage = QStringLiteral("drive is not there");
    broken.consecutiveFailures = 4;
    broken.lastRunAt = QDateTime::currentDateTime();
    m_app->schedules()->put(broken);

    AutomationController* automation = openAutomation();
    QVERIFY(automation);

    // Alphabetically the healthy one comes first; what needs attention wins.
    const QVariantList rules = automation->rules();
    QCOMPARE(rules.size(), 2);
    QCOMPARE(rules.first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("ZZZ broken"));
    QCOMPARE(rules.first().toMap().value(QStringLiteral("consecutiveFailures")).toInt(), 4);
    QCOMPARE(automation->subtitle(), QStringLiteral("1 failing"));
}

void TestAutomation::ruleSurvivesARestartOfTheApplication()
{
    const QString uri = m_tree->rootUri().toString();
    AnalysisTarget* target = analyse(uri);
    QVERIFY(target);
    target->setSchedule(604800);

    m_app.reset();

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(uri));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));
    m_app->scheduler()->stop();

    QCOMPARE(m_app->schedules()->rules().size(), 1);
    QCOMPARE(m_app->schedules()->rules().first().intervalSeconds, 604800);

    // And the analysis tab shows it, so the user is not offered a second
    // schedule for a folder that already has one.
    AnalysisTarget* reopened = analyse(uri);
    QVERIFY(reopened);
    QCOMPARE(reopened->scheduleText(), QStringLiteral("Every week"));
}

/// An index is only ever as fresh as the last time somebody remembered, and for
/// the volume it matters most on that is "not very". This is the answer to
/// staleness the search deliberately does not have.
void TestAutomation::aScheduledRescanRunsSurvivesARestartAndCatchesUp()
{
    const QString uri = m_tree->rootUri().toString();
    LiveSearchController* form = search();
    QVERIFY(form);
    QVERIFY(!form->scheduleScan(uri, 24 * 3600).isEmpty());
    QCOMPARE(form->scheduledScanId(uri).isEmpty(), false);
    // Noted before the restart takes the form with it: what the scan was asked
    // for is what the rule has to repeat.
    const bool askedForMetadata = form->scanReadsMetadata();
    const bool askedForArchives = form->scanOpensArchives();

    // Survives a restart, which is the scheduler's own behaviour and has to
    // hold for this job type like any other.
    m_app.reset();
    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(uri));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));
    m_app->scheduler()->stop();

    QCOMPARE(m_app->schedules()->rules().size(), 1);
    const ScheduleRule restored = m_app->schedules()->rules().first();
    QCOMPARE(restored.jobKind, QStringLiteral("index"));
    QCOMPARE(restored.intervalSeconds, 24 * 3600);

    // Every option the scan was asked for, read back out of the store: a rule
    // that carried only the incremental flag repeated the scan as a poorer one
    // and stripped the metadata out of every subtree it re-walked. MOLE-226.
    const ScanOptions asked = IndexScanJob::optionsFor(restored);
    QVERIFY(asked.incremental);
    QCOMPARE(asked.metadata, askedForMetadata);
    QCOMPARE(asked.archives, askedForArchives);

    // A rule created and never run is due at once, so the restart may already
    // have caught it. Let that settle before staging the missed night.
    QVERIFY(waitFor([this] { return m_app->scheduler()->runningRules().isEmpty(); }, 30000));

    // Overdue, the way a rule is after a night with the machine off.
    ScheduleRule missed = m_app->schedules()->rules().first();
    missed.lastRunAt = QDateTime::currentDateTime().addSecs(-2 * 24 * 3600);
    missed.lastStatus = RunStatus::Succeeded;
    m_app->schedules()->put(missed);

    QCOMPARE(m_app->scheduler()->checkDue(), 1);
    QVERIFY(waitFor([this] { return m_app->scheduler()->runningRules().isEmpty(); }, 30000));

    const ScheduleRule after = m_app->schedules()->rules().first();
    QCOMPARE(after.lastStatus, RunStatus::Succeeded);
    QVERIFY(after.lastSuccessAt.isValid());

    // And it really indexed: the folder is searchable without a tab having been
    // open at any point.
    LiveSearchController* reopened = search();
    QVERIFY(reopened);
    QVERIFY2(reopened->indexCoversRoot() || !reopened->volumeLabels().isEmpty(),
        "a scheduled scan has to leave a volume behind");
}

/// The nightly re-index used to be a checkbox that created a rule when it was
/// ticked and did nothing at all when it was unticked, over an interval written
/// as 24 in the QML. All three of those are one control now, and it reads like
/// the report tab's. See MOLE-227.
void TestAutomation::anIntervalCanBeChosenChangedAndTurnedOffForAFolder()
{
    const QString uri = m_tree->rootUri().toString();
    LiveSearchController* form = search();
    QVERIFY(form);
    QCOMPARE(form->scheduledScanSeconds(uri), 0);

    // Chosen for a folder that has none.
    const QString id = form->scheduleScan(uri, 7 * 24 * 3600);
    QVERIFY(!id.isEmpty());
    QCOMPARE(m_app->schedules()->rules().size(), 1);
    QCOMPARE(form->scheduledScanSeconds(uri), 7 * 24 * 3600);

    // Changed for a folder that has one, keeping the rule rather than being
    // ignored -- which is what happened while the first rule won.
    QCOMPARE(form->scheduleScan(uri, 24 * 3600), id);
    QCOMPARE(m_app->schedules()->rules().size(), 1);
    QCOMPARE(form->scheduledScanSeconds(uri), 24 * 3600);

    // And turned off, which is what the zero entry of the picker means.
    QVERIFY(form->scheduleScan(uri, 0).isEmpty());
    QVERIFY(m_app->schedules()->rules().isEmpty());
    QVERIFY(form->scheduledScanId(uri).isEmpty());
    QCOMPARE(form->scheduledScanSeconds(uri), 0);

    // The intervals offered are the ones every other repeating job is offered,
    // so the two dialogs cannot drift apart.
    QCOMPARE(form->schedulePresets().size(), ScheduleRule::presets().size());

    // Named for the folder, because a rule sits in Automation beside the report
    // rules and the whole uri of a deep tree tells a reader nothing.
    QVERIFY(!form->scheduleScan(uri, 24 * 3600).isEmpty());
    const QString label = m_app->schedules()->rules().first().label;
    QVERIFY2(!label.contains(uri), qPrintable(label));
    QVERIFY2(label.contains(QFileInfo(m_tree->path()).fileName()), qPrintable(label));
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestAutomation testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_Automation.moc"
