#include "core/automation/ChainTask.h"

namespace mole {

ChainTask::ChainTask(Chain chain, const ChainRegistry* registry, QObject* parent)
    : Task(chain.name.isEmpty() ? QStringLiteral("Chain") : chain.name, parent)
    , m_chain(std::move(chain))
    , m_registry(registry)
{
}

void ChainTask::announce(int index, const IChainStepKind& kind)
{
    const int count = m_chain.steps.size();
    reportText(QStringLiteral("step"), QStringLiteral("Step"),
        QStringLiteral("%1 of %2: %3").arg(index + 1).arg(count).arg(kind.displayName()));
    setStatusText(kind.displayName());
    // Per step rather than per file: a bar that moves four times in an hour tells
    // nobody anything, and the step's own metrics are underneath it.
    setProgress(count > 0 ? (index * 100) / count : 0);
}

void ChainTask::run()
{
    if (!m_registry) {
        m_ending = Ending::Unrunnable;
        m_because = QStringLiteral("There is nothing here that knows how to run a chain");
        fail(VfsError::make(VfsError::NotSupported, m_because));
        return;
    }

    QString why;
    if (!m_registry->isRunnable(m_chain, &why)) {
        // Refused before anything runs, rather than half way along: a chain that
        // would drop its work on the floor should never start.
        m_ending = Ending::Unrunnable;
        m_because = why;
        fail(VfsError::make(VfsError::NotSupported, why));
        return;
    }

    QStringList carried;
    for (int i = 0; i < m_chain.steps.size(); ++i) {
        // Between two steps as well as inside one: cancelling must not let the
        // next step start.
        if (isCancelRequested()) {
            m_ending = Ending::Cancelled;
            m_because = QStringLiteral("Cancelled after %1 of %2 steps").arg(i).arg(m_chain.steps.size());
            setStatusText(m_because);
            return;
        }

        const ChainStep& step = m_chain.steps.at(i);
        IChainStepKind* kind = m_registry->kind(step.kind);
        // isRunnable() already said every kind is here; a kind that went away
        // between then and now is a plugin unloading mid-run.
        if (!kind) {
            m_ending = Ending::Failed;
            m_because = QStringLiteral("Step %1 (%2) is no longer available").arg(i + 1).arg(step.kind);
            fail(VfsError::make(VfsError::NotSupported, m_because));
            return;
        }

        announce(i, *kind);

        StepContext context;
        context.cancel = cancelToken();
        context.say = [this, kind](const QString& text) {
            setStatusText(QStringLiteral("%1: %2").arg(kind->displayName(), text));
        };
        context.progress = [this](qint64 done, qint64 total) {
            if (total > 0)
                reportCount(QStringLiteral("stepProgress"), QStringLiteral("This step"),
                    double(done) * 100.0 / double(total), 110);
        };

        const StepOutcome outcome = kind->run(step, carried, context);
        ++m_stepsRun;

        if (outcome.result == StepOutcome::Result::Failed) {
            // Nothing is handed on from a step that did not fully succeed, so
            // there is nothing for the steps after it to act on -- which is why
            // they do not run at all rather than running on a short list.
            m_ending = Ending::Failed;
            m_because = outcome.message.isEmpty()
                ? QStringLiteral("Step %1 (%2) failed").arg(i + 1).arg(kind->displayName())
                : outcome.message;
            fail(VfsError::make(VfsError::Unknown, m_because));
            return;
        }

        // A cancelled step comes back having done what it could; the chain stops
        // here either way, and says it was cancelled rather than that it failed.
        if (isCancelRequested()) {
            m_ending = Ending::Cancelled;
            m_because = QStringLiteral("Cancelled during %1").arg(kind->displayName());
            setStatusText(m_because);
            return;
        }

        if (outcome.result == StepOutcome::Result::Nothing) {
            const bool last = i == m_chain.steps.size() - 1;
            // A sink is allowed to produce nothing: there is nothing after it for
            // an empty list to reach. Anywhere else it is the third outcome.
            if (kind->role() == StepRole::Sink && last)
                break;

            if (step.stopsWhenEmpty()) {
                m_ending = Ending::StoppedEmpty;
                m_because = outcome.message.isEmpty()
                    ? QStringLiteral("%1 found nothing, so the chain stopped there").arg(kind->displayName())
                    : outcome.message;
                setStatusText(m_because);
                // Not a failure: nothing went wrong, and nothing happened. The
                // difference is what somebody reading a run log needs.
                reportText(QStringLiteral("ending"), QStringLiteral("Ended"), m_because, 120);
                return;
            }
            // Told to carry on, so the empty list is handed on as itself.
            carried.clear();
            continue;
        }

        carried = outcome.uris;
        m_produced = carried;
        reportCount(QStringLiteral("carried"), QStringLiteral("Files"), carried.size(), 100);
    }

    m_ending = Ending::Ran;
    setProgress(100);
    m_because = QStringLiteral("%1 step(s) ran").arg(m_stepsRun);
    setStatusText(m_because);
}

} // namespace mole
