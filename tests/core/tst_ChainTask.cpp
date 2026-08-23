#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/automation/ChainTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

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
    /// What it says it *would* do. Absent means it cannot say, which is the
    /// default every step kind starts with.
    using Foresight = std::function<StepPreview(const QStringList& incoming, const StepContext&)>;

    ScriptedStep(QString id, StepRole role, Body body, Foresight foresight = {})
        : m_id(std::move(id))
        , m_role(role)
        , m_body(std::move(body))
        , m_foresight(std::move(foresight))
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

    StepPreview preview(
        const ChainStep& step, const QStringList& incoming, const StepContext& context) override
    {
        m_previews.fetch_add(1, std::memory_order_relaxed);
        if (!m_foresight)
            return IChainStepKind::preview(step, incoming, context);
        return m_foresight(incoming, context);
    }

    int runs() const { return m_runs.load(std::memory_order_relaxed); }
    int previews() const { return m_previews.load(std::memory_order_relaxed); }
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
    Foresight m_foresight;
    mutable QMutex m_guard;
    QStringList m_received;
    std::atomic_int m_runs { 0 };
    std::atomic_int m_previews { 0 };
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

    // ---- a dry run -------------------------------------------------------
    void aDryRunListsWhatCameInAndWhatWouldGoOutPerStep();
    void aDryRunWritesNothingAtAll();
    void aFilterInADryRunNamesTheFilesItWouldKeep();
    void aStepThatCannotPredictItsOutputEndsThePreviewAndSaysSo();
    void aDryRunWhoseSourceFindsNothingReportsTheEmptyStop();

private:
    std::unique_ptr<TaskManager> m_tasks;
    ChainRegistry m_registry;
    std::vector<std::unique_ptr<ScriptedStep>> m_kinds;

    ScriptedStep* give(
        const QString& id, StepRole role, ScriptedStep::Body body, ScriptedStep::Foresight foresight = {})
    {
        m_kinds.push_back(std::make_unique<ScriptedStep>(id, role, std::move(body), std::move(foresight)));
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

// ---- a dry run --------------------------------------------------------------
//
// A chain that finds files, compresses them, moves the archive and deletes the
// originals is four irreversible acts behind one button, over a set of files
// chosen by a query somebody wrote a fortnight ago. See MOLE-166.

void TestChainTask::aDryRunListsWhatCameInAndWhatWouldGoOutPerStep()
{
    const QStringList found { QStringLiteral("mem:///a.txt"), QStringLiteral("mem:///b.txt"),
        QStringLiteral("mem:///c.txt") };

    // A source that really evaluates, which is the whole point: a preview that
    // does not name the actual files answers a different question.
    give(
        QStringLiteral("find"), StepRole::Source,
        [found](const QStringList&, const StepContext&) { return StepOutcome::produced(found); },
        [found](const QStringList&, const StepContext&) {
            return StepPreview::would(found, QStringLiteral("look in mem:///"));
        });
    // Something that writes: it says what it would do, and its output is known
    // because the archive's name is already decided.
    give(
        QStringLiteral("pack"), StepRole::Transform,
        [](const QStringList&, const StepContext&) {
            return StepOutcome::produced({ QStringLiteral("mem:///reports.zip") });
        },
        [](const QStringList& incoming, const StepContext&) {
            return StepPreview::would({ QStringLiteral("mem:///reports.zip") },
                QStringLiteral("compress %1 files into reports.zip").arg(incoming.size()));
        });
    give(
        QStringLiteral("move"), StepRole::Sink,
        [](const QStringList&, const StepContext&) { return StepOutcome::nothing(); },
        [](const QStringList& incoming, const StepContext&) {
            return StepPreview::nothing(
                QStringLiteral("move %1 file(s) to the archive drive").arg(incoming.size()));
        });

    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.steps
        = { stepOf(QStringLiteral("find")), stepOf(QStringLiteral("pack")), stepOf(QStringLiteral("move")) };
    auto* task = new ChainTask(chain, &m_registry);
    task->setDryRun(true);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->ending() == ChainTask::Ending::Ran);

    const QList<ChainTask::PreviewLine> lines = task->preview();
    QCOMPARE(lines.size(), 3);
    QVERIFY(lines.at(0).predictable);
    QVERIFY2(lines.at(0).incoming.isEmpty(), "a source was shown as receiving something");
    QCOMPARE(lines.at(0).outgoing, found);
    // The files by name, at every step, and not a count of them.
    QCOMPARE(lines.at(1).incoming, found);
    QCOMPARE(lines.at(1).outgoing, QStringList { QStringLiteral("mem:///reports.zip") });
    QVERIFY2(lines.at(1).intent.contains(QStringLiteral("compress 3 files")), qPrintable(lines.at(1).intent));
    QCOMPARE(lines.at(2).incoming, QStringList { QStringLiteral("mem:///reports.zip") });
    QVERIFY2(lines.at(2).outgoing.isEmpty(), "a sink was shown as producing something");
    QVERIFY2(lines.at(2).intent.contains(QStringLiteral("move 1 file")), qPrintable(lines.at(2).intent));
}

void TestChainTask::aDryRunWritesNothingAtAll()
{
    // The steps here write for real when they run, into a MemoryFileSystem, and
    // the same chain is asked both ways: what the run does is what proves the dry
    // run did not do it. A step's run() being called at all is the failure -- so
    // the writes are counted rather than asserted from the worker thread, where a
    // QVERIFY cannot be trusted.
    auto disk = std::make_shared<MemoryFileSystem>();
    disk->addFile(QStringLiteral("/a.txt"), QByteArray("one"));

    std::atomic_int writes { 0 };
    give(
        QStringLiteral("find"), StepRole::Source,
        [](const QStringList&, const StepContext&) {
            return StepOutcome::produced({ QStringLiteral("mem:///a.txt") });
        },
        [](const QStringList&, const StepContext&) {
            return StepPreview::would({ QStringLiteral("mem:///a.txt") });
        });
    ScriptedStep* destroy = give(
        QStringLiteral("destroy"), StepRole::Sink,
        [disk, &writes](const QStringList& incoming, const StepContext&) {
            for (const QString& uri : incoming) {
                ++writes;
                disk->remove(VfsUri::fromString(uri), false);
            }
            return StepOutcome::nothing();
        },
        [](const QStringList& incoming, const StepContext&) {
            return StepPreview::nothing(QStringLiteral("delete %1 file(s)").arg(incoming.size()));
        });

    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.steps = { stepOf(QStringLiteral("find")), stepOf(QStringLiteral("destroy")) };

    auto* dry = new ChainTask(chain, &m_registry);
    dry->setDryRun(true);
    m_tasks->submit(dry);
    QVERIFY(waitForTask(dry, 30000));

    QCOMPARE(writes.load(), 0);
    QCOMPARE(destroy->runs(), 0);
    QVERIFY2(destroy->previews() > 0, "the step was not even asked what it would do");
    QVERIFY2(disk->stat(VfsUri::fromString(QStringLiteral("mem:///a.txt"))).ok(), "a dry run deleted a file");
    QVERIFY2(dry->preview().last().intent.contains(QStringLiteral("delete 1 file")),
        qPrintable(dry->preview().last().intent));

    // And the same chain run for real does the thing, which is what makes the
    // assertion above about the dry run rather than about a chain that does
    // nothing.
    auto* real = new ChainTask(chain, &m_registry);
    m_tasks->submit(real);
    QVERIFY(waitForTask(real, 30000));
    QCOMPARE(writes.load(), 1);
    QVERIFY2(!disk->stat(VfsUri::fromString(QStringLiteral("mem:///a.txt"))).ok(),
        "the real run left the file behind, so the case above proved nothing");
}

void TestChainTask::aFilterInADryRunNamesTheFilesItWouldKeep()
{
    // A filter genuinely evaluates, so the preview says *which* files survive it.
    // "3 of 5 would be kept" is the answer to a question nobody asked.
    const QStringList all { QStringLiteral("mem:///a.txt"), QStringLiteral("mem:///b.pdf"),
        QStringLiteral("mem:///c.txt") };
    give(
        QStringLiteral("find"), StepRole::Source,
        [all](const QStringList&, const StepContext&) { return StepOutcome::produced(all); },
        [all](const QStringList&, const StepContext&) { return StepPreview::would(all); });

    const auto onlyText = [](const QStringList& incoming) {
        QStringList kept;
        for (const QString& uri : incoming) {
            if (uri.endsWith(QStringLiteral(".txt")))
                kept.append(uri);
        }
        return kept;
    };
    give(
        QStringLiteral("filter"), StepRole::Transform,
        [onlyText](const QStringList& incoming, const StepContext&) {
            return StepOutcome::produced(onlyText(incoming));
        },
        [onlyText](const QStringList& incoming, const StepContext&) {
            return StepPreview::would(onlyText(incoming));
        });

    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.steps = { stepOf(QStringLiteral("find")), stepOf(QStringLiteral("filter")) };
    auto* task = new ChainTask(chain, &m_registry);
    task->setDryRun(true);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    const QList<ChainTask::PreviewLine> lines = task->preview();
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(1).outgoing,
        QStringList({ QStringLiteral("mem:///a.txt"), QStringLiteral("mem:///c.txt") }));
}

void TestChainTask::aStepThatCannotPredictItsOutputEndsThePreviewAndSaysSo()
{
    // The honest answer, and the default for a kind that has not been taught to
    // predict: it says so, and the preview ends there naming what is left --
    // rather than inventing a list, or stopping in a way that makes the chain look
    // shorter than it is.
    give(
        QStringLiteral("find"), StepRole::Source,
        [](const QStringList&, const StepContext&) {
            return StepOutcome::produced({ QStringLiteral("mem:///a.txt") });
        },
        [](const QStringList&, const StepContext&) {
            return StepPreview::would({ QStringLiteral("mem:///a.txt") });
        });
    // No foresight given, so this one falls back on the default.
    ScriptedStep* murky
        = give(QStringLiteral("transcode"), StepRole::Transform, [](const QStringList&, const StepContext&) {
              return StepOutcome::produced({ QStringLiteral("mem:///out.mkv") });
          });
    ScriptedStep* after = give(QStringLiteral("move"), StepRole::Sink,
        [](const QStringList&, const StepContext&) { return StepOutcome::nothing(); });

    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.steps = { stepOf(QStringLiteral("find")), stepOf(QStringLiteral("transcode")),
        stepOf(QStringLiteral("move")) };
    auto* task = new ChainTask(chain, &m_registry);
    task->setDryRun(true);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    // Not a failure: nothing went wrong, and the chain is simply not knowable
    // beyond here.
    QCOMPARE(task->state(), Task::State::Succeeded);
    const QList<ChainTask::PreviewLine> lines = task->preview();
    QCOMPARE(lines.size(), 3);
    QVERIFY2(!lines.at(1).predictable, "a step that could not answer was shown as if it had");
    QVERIFY2(lines.at(2).step.isEmpty(), "the closing line was shown as a step");
    QVERIFY2(lines.at(2).intent.contains(QStringLiteral("1 more step")), qPrintable(lines.at(2).intent));
    QVERIFY2(task->endedBecause().contains(QStringLiteral("cannot be predicted")),
        qPrintable(task->endedBecause()));
    // And the steps past it were neither run nor previewed, because there is
    // nothing to hand them.
    QCOMPARE(murky->runs(), 0);
    QCOMPARE(after->previews(), 0);
    QCOMPARE(after->runs(), 0);
}

void TestChainTask::aDryRunWhoseSourceFindsNothingReportsTheEmptyStop()
{
    // The same rules, asked the other way: a preview of a chain that would stop
    // has to say it would stop, or somebody schedules a chain that never does
    // anything and reads the preview as proof that it will.
    give(
        QStringLiteral("find"), StepRole::Source,
        [](const QStringList&, const StepContext&) { return StepOutcome::nothing(); },
        [](const QStringList&, const StepContext&) {
            return StepPreview::nothing(QStringLiteral("nothing matches in mem:///"));
        });
    ScriptedStep* pack
        = give(QStringLiteral("pack"), StepRole::Transform, [](const QStringList&, const StepContext&) {
              return StepOutcome::produced({ QStringLiteral("mem:///reports.zip") });
          });

    Chain chain;
    chain.id = QStringLiteral("c1");
    chain.steps = { stepOf(QStringLiteral("find")), stepOf(QStringLiteral("pack")) };
    auto* task = new ChainTask(chain, &m_registry);
    task->setDryRun(true);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->ending() == ChainTask::Ending::StoppedEmpty);
    QVERIFY2(
        task->endedBecause().contains(QStringLiteral("nothing matches")), qPrintable(task->endedBecause()));
    QCOMPARE(pack->previews(), 0);
    QCOMPARE(pack->runs(), 0);
}

MOLE_TEST_MAIN(TestChainTask)

#include "tst_ChainTask.moc"
