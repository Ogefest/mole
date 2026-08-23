#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/automation/ChainTask.h"
#include "core/tasks/TaskManager.h"

#include <QTest>
#include <QThread>

#include <atomic>

using namespace mole;
using namespace mole::test;

namespace {

/// A step that answers however the case told it to, and records what it was
/// handed.
///
/// Recording what came in is most of the point: the rule "a step hands on what
/// it produced, not what it consumed" cannot be checked by looking at the end
/// of the chain, only by asking each step what it received.
class ScriptedStep final : public IChainStepKind
{
public:
    using Body = std::function<StepOutcome(const QStringList& incoming, const StepContext&)>;

    ScriptedStep(QString id, StepRole role, Body body)
        : m_id(std::move(id))
        , m_role(role)
        , m_body(std::move(body))
    {
    }

    QString kind() const override { return m_id; }
    QString displayName() const override { return m_id; }
    StepRole role() const override { return m_role; }
    QList<StepParameter> parameters() const override { return {}; }

    StepOutcome run(const ChainStep&, const QStringList& incoming, const StepContext& context) override
    {
        m_runs.fetch_add(1, std::memory_order_relaxed);
        {
            QMutexLocker lock(&m_guard);
            m_received = incoming;
        }
        m_started.store(true, std::memory_order_release);
        return m_body(incoming, context);
    }

    int runs() const { return m_runs.load(std::memory_order_relaxed); }
    bool hasStarted() const { return m_started.load(std::memory_order_acquire); }
    QStringList received() const
    {
        QMutexLocker lock(&m_guard);
        return m_received;
    }

private:
    QString m_id;
    StepRole m_role;
    Body m_body;
    mutable QMutex m_guard;
    QStringList m_received;
    std::atomic_int m_runs { 0 };
    std::atomic_bool m_started { false };
};

ChainStep stepOf(const QString& kind, bool stopWhenEmpty = true)
{
    ChainStep step;
    step.kind = kind;
    if (!stopWhenEmpty)
        step.properties = { { QString::fromLatin1(kStopWhenEmpty), false } };
    return step;
}

} // namespace

/// Running a chain: in order, carrying the list, and stopping for the right
/// reasons. See MOLE-165 and ADR-0082.
class TestChainTask : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void threeStepsRunInOrderAndEachGetsWhatTheLastProduced();
    void aStepHandsOnWhatItProducedRatherThanWhatItConsumed();
    void anEmptyResultStopsTheChainAndIsNeitherSuccessNorFailure();
    void aStepThatOverridesTheRulePassesTheEmptyListOn();
    void aFailedStepStopsTheChainAndTheOnesAfterItDoNotRun();
    void aSinkThatProducesNothingIsNotAnEmptyResult();
    void cancellingReachesTheRunningStepAndStartsNoFurtherOne();
    void aChainCancelledBeforeItStartsRunsNoStepAtAll();
    void aChainThatCannotRunIsRefusedBeforeAnythingRuns();

private:
    std::unique_ptr<TaskManager> m_tasks;
    ChainRegistry m_registry;
    std::vector<std::unique_ptr<ScriptedStep>> m_kinds;

    ScriptedStep* give(const QString& id, StepRole role, ScriptedStep::Body body)
    {
        m_kinds.push_back(std::make_unique<ScriptedStep>(id, role, std::move(body)));
        m_registry.registerKind(m_kinds.back().get());
        return m_kinds.back().get();
    }

    /// A step that hands back exactly these uris.
    ScriptedStep* giving(const QString& id, StepRole role, const QStringList& uris)
    {
        return give(
            id, role, [uris](const QStringList&, const StepContext&) { return StepOutcome::produced(uris); });
    }

    ChainTask* runChain(const QList<ChainStep>& steps, int timeoutMs = 30000)
    {
        Chain chain;
        chain.id = QStringLiteral("c1");
        chain.name = QStringLiteral("A chain");
        chain.steps = steps;
        auto* task = new ChainTask(chain, &m_registry);
        m_tasks->submit(task);
        const bool finished = waitForTask(task, timeoutMs);
        return finished ? task : nullptr;
    }
};

void TestChainTask::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_registry = ChainRegistry {};
    m_kinds.clear();
}

void TestChainTask::cleanup()
{
    // Destroyed here rather than at the top of the next case: the destructor
    // cancels and joins the pool, and a task still running while the harness
    // moves on is what MOLE-273 was about.
    m_tasks.reset();
}

void TestChainTask::threeStepsRunInOrderAndEachGetsWhatTheLastProduced()
{
    ScriptedStep* find = giving(QStringLiteral("find"), StepRole::Source,
        { QStringLiteral("mem:///a.txt"), QStringLiteral("mem:///b.txt") });
    ScriptedStep* pack
        = giving(QStringLiteral("pack"), StepRole::Transform, { QStringLiteral("mem:///bundle.zip") });
    ScriptedStep* move = give(QStringLiteral("move"), StepRole::Sink,
        [](const QStringList&, const StepContext&) { return StepOutcome::nothing(); });

    ChainTask* task = runChain(
        { stepOf(QStringLiteral("find")), stepOf(QStringLiteral("pack")), stepOf(QStringLiteral("move")) });
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->ending() == ChainTask::Ending::Ran);
    QCOMPARE(task->stepsRun(), 3);

    // In order, and each with exactly what the one before produced.
    QCOMPARE(find->runs(), 1);
    QVERIFY2(find->received().isEmpty(), "a source was handed a list");
    QCOMPARE(
        pack->received(), QStringList({ QStringLiteral("mem:///a.txt"), QStringLiteral("mem:///b.txt") }));
    QCOMPARE(move->received(), QStringList { QStringLiteral("mem:///bundle.zip") });
}

void TestChainTask::aStepHandsOnWhatItProducedRatherThanWhatItConsumed()
{
    // The rule that would otherwise be found by accident: a chain that compresses
    // and then moves must move the *archive*. Getting it wrong moves the twelve
    // originals and leaves the archive behind, which is a data loss dressed as a
    // feature.
    QStringList twelve;
    for (int i = 0; i < 12; ++i)
        twelve.append(QStringLiteral("mem:///report-%1.txt").arg(i));

    giving(QStringLiteral("find"), StepRole::Source, twelve);
    giving(QStringLiteral("pack"), StepRole::Transform, { QStringLiteral("mem:///reports.zip") });
    ScriptedStep* move = give(QStringLiteral("move"), StepRole::Sink,
        [](const QStringList&, const StepContext&) { return StepOutcome::nothing(); });

    ChainTask* task = runChain(
        { stepOf(QStringLiteral("find")), stepOf(QStringLiteral("pack")), stepOf(QStringLiteral("move")) });
    QVERIFY(task);
    QCOMPARE(move->received(), QStringList { QStringLiteral("mem:///reports.zip") });
    QCOMPARE(move->received().size(), 1);
}

void TestChainTask::anEmptyResultStopsTheChainAndIsNeitherSuccessNorFailure()
{
    give(QStringLiteral("find"), StepRole::Source, [](const QStringList&, const StepContext&) {
        return StepOutcome::nothing(QStringLiteral("nothing matched"));
    });
    ScriptedStep* pack
        = giving(QStringLiteral("pack"), StepRole::Transform, { QStringLiteral("mem:///bundle.zip") });

    ChainTask* task = runChain({ stepOf(QStringLiteral("find")), stepOf(QStringLiteral("pack")) });
    QVERIFY(task);

    // Distinguishable from failure: nothing went wrong.
    QCOMPARE(task->state(), Task::State::Succeeded);
    // And from success: nothing happened either, and the reason is readable.
    QVERIFY(task->ending() == ChainTask::Ending::StoppedEmpty);
    QVERIFY2(
        task->endedBecause().contains(QStringLiteral("nothing matched")), qPrintable(task->endedBecause()));
    QCOMPARE(task->stepsRun(), 1);
    QCOMPARE(pack->runs(), 0);
    QVERIFY(task->produced().isEmpty());
}

void TestChainTask::aStepThatOverridesTheRulePassesTheEmptyListOn()
{
    // The step's own property, and the point of it living on the step: a chain
    // that should run its last step whatever the search found is a decision
    // somebody made, not a default.
    give(QStringLiteral("find"), StepRole::Source,
        [](const QStringList&, const StepContext&) { return StepOutcome::nothing(); });
    ScriptedStep* report = give(QStringLiteral("report"), StepRole::Sink,
        [](const QStringList&, const StepContext&) { return StepOutcome::nothing(); });

    ChainTask* task = runChain({ stepOf(QStringLiteral("find"), false), stepOf(QStringLiteral("report")) });
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->ending() == ChainTask::Ending::Ran);
    QCOMPARE(report->runs(), 1);
    QVERIFY2(report->received().isEmpty(), "the empty list was not what was handed on");
}

void TestChainTask::aFailedStepStopsTheChainAndTheOnesAfterItDoNotRun()
{
    giving(QStringLiteral("find"), StepRole::Source, { QStringLiteral("mem:///a.txt") });
    give(QStringLiteral("pack"), StepRole::Transform, [](const QStringList&, const StepContext&) {
        return StepOutcome::failed(QStringLiteral("the archive could not be written"));
    });
    ScriptedStep* move = give(QStringLiteral("move"), StepRole::Sink,
        [](const QStringList&, const StepContext&) { return StepOutcome::nothing(); });

    ChainTask* task = runChain(
        { stepOf(QStringLiteral("find")), stepOf(QStringLiteral("pack")), stepOf(QStringLiteral("move")) });
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Failed);
    QVERIFY(task->ending() == ChainTask::Ending::Failed);
    QVERIFY2(task->endedBecause().contains(QStringLiteral("could not be written")),
        qPrintable(task->endedBecause()));
    // Nothing is handed on from a step that did not fully succeed, so there is
    // nothing for the step after it to act on.
    QCOMPARE(move->runs(), 0);
    QCOMPARE(task->stepsRun(), 2);
}

void TestChainTask::aSinkThatProducesNothingIsNotAnEmptyResult()
{
    // A sink gives nothing back by definition, so its "nothing" must not be read
    // as a chain that found nothing -- otherwise every chain ending in an action
    // would report itself as having stopped early.
    giving(QStringLiteral("find"), StepRole::Source, { QStringLiteral("mem:///a.txt") });
    give(QStringLiteral("move"), StepRole::Sink, [](const QStringList& incoming, const StepContext&) {
        return incoming.isEmpty() ? StepOutcome::failed(QStringLiteral("nothing to move"))
                                  : StepOutcome::nothing();
    });

    ChainTask* task = runChain({ stepOf(QStringLiteral("find")), stepOf(QStringLiteral("move")) });
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->ending() == ChainTask::Ending::Ran);
    QCOMPARE(task->stepsRun(), 2);
}

void TestChainTask::cancellingReachesTheRunningStepAndStartsNoFurtherOne()
{
    // Waited on by condition and not by clock: the case blocks the step until it
    // sees cancellation, and the test waits until the step says it has started.
    // A sleep here would pass on one machine and fail on another.
    std::atomic_bool sawCancel { false };
    ScriptedStep* slow = give(QStringLiteral("slow"), StepRole::Source,
        [&sawCancel](const QStringList&, const StepContext& context) {
            while (!context.cancel.isCancelled())
                QThread::msleep(1);
            sawCancel.store(true, std::memory_order_release);
            // What it managed before it was stopped, which the chain must not
            // hand on to anything.
            return StepOutcome::produced({ QStringLiteral("mem:///half-done.txt") });
        });
    ScriptedStep* next
        = giving(QStringLiteral("next"), StepRole::Transform, { QStringLiteral("mem:///never.zip") });

    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.steps = { stepOf(QStringLiteral("slow")), stepOf(QStringLiteral("next")) };
    auto* task = new ChainTask(chain, &m_registry);
    m_tasks->submit(task);

    QVERIFY2(waitFor([slow] { return slow->hasStarted(); }, 30000), "the step never started");
    task->requestCancel();
    QVERIFY(waitForTask(task, 30000));

    QVERIFY2(sawCancel.load(std::memory_order_acquire), "cancellation never reached the step");
    QVERIFY(task->ending() == ChainTask::Ending::Cancelled);
    QVERIFY2(next->runs() == 0, "the next step started after the chain was cancelled");
    QVERIFY2(task->endedBecause().contains(QStringLiteral("Cancelled")), qPrintable(task->endedBecause()));
}

void TestChainTask::aChainCancelledBeforeItStartsRunsNoStepAtAll()
{
    // The other half of "cancelling between two steps must not let the next one
    // start": before the *first* one, there is no previous step to have noticed.
    // A chain cancelled while it sat in the queue must run nothing.
    ScriptedStep* find = giving(QStringLiteral("find"), StepRole::Source, { QStringLiteral("mem:///a.txt") });

    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.steps = { stepOf(QStringLiteral("find")) };
    auto* task = new ChainTask(chain, &m_registry);
    task->requestCancel();
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    QCOMPARE(find->runs(), 0);
    QCOMPARE(task->stepsRun(), 0);
}

void TestChainTask::aChainThatCannotRunIsRefusedBeforeAnythingRuns()
{
    // A chain that would drop its work on the floor should never start, so the
    // refusal is before the first step rather than at the step that is wrong.
    ScriptedStep* first = give(QStringLiteral("move"), StepRole::Sink,
        [](const QStringList&, const StepContext&) { return StepOutcome::nothing(); });
    giving(QStringLiteral("pack"), StepRole::Transform, { QStringLiteral("mem:///bundle.zip") });

    ChainTask* task = runChain({ stepOf(QStringLiteral("move")), stepOf(QStringLiteral("pack")) });
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Failed);
    QVERIFY(task->ending() == ChainTask::Ending::Unrunnable);
    QCOMPARE(task->stepsRun(), 0);
    QCOMPARE(first->runs(), 0);
    QVERIFY2(task->endedBecause().contains(QStringLiteral("step 1 of 2")), qPrintable(task->endedBecause()));
}

MOLE_TEST_MAIN(TestChainTask)

#include "tst_ChainTask.moc"
