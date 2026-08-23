#pragma once

#include "core/automation/Chain.h"
#include "core/tasks/Task.h"

namespace mole {

/// Runs a chain: each step in order, carrying the list from one to the next.
///
/// A Task like everything else, which is not a detail: cancellation, progress,
/// the metrics strip and a row beside an ordinary copy all come from being one,
/// and a chain running for an hour is then something somebody can watch and stop.
///
/// **Three rules, and they are what makes a chain safe to leave running
/// unattended.**
///
/// *An empty list stops the chain and says so.* A third outcome beside success
/// and failure, not a quiet success: a chain that carries on with nothing writes
/// an empty archive and moves it somewhere, which reads exactly like working. The
/// scheduler already draws this line between Skipped and Failed. A step may
/// override it through the property it declares for itself -- and then the empty
/// list is handed on, which is a decision somebody made rather than a default.
///
/// *A step that did not fully succeed hands nothing on.* TransferTask already
/// refuses to delete a source unless every entry arrived; this is the same trade
/// made once, at the chain level, rather than negotiated per step. There is no
/// `set -e` to configure, because the only other setting is "carry on with
/// whatever survived", and nothing downstream can tell that from a complete list.
///
/// *Cancellation reaches the running step*, through the token every task already
/// has -- and a chain cancelled between two steps starts no further one.
class ChainTask final : public Task
{
    Q_OBJECT

public:
    /// How it ended. `state()` says whether it succeeded; this says why, which is
    /// the distinction a chain needs and a task on its own does not have.
    enum class Ending {
        Ran, ///< every step ran, and the last one produced something
        StoppedEmpty, ///< a step produced nothing, and stopping was the rule
        Failed,
        Cancelled,
        Unrunnable, ///< refused before anything ran -- see ChainRegistry::isRunnable()
    };
    Q_ENUM(Ending)

    /// The registry is borrowed and must outlive the task, the way a task's
    /// filesystem does. A chain naming a kind it does not hold is refused before
    /// any step runs rather than half way along.
    ChainTask(Chain chain, const ChainRegistry* registry, QObject* parent = nullptr);

    /// Read after the task has finished.
    [[nodiscard]] Ending ending() const { return m_ending; }
    /// What the last step that produced anything handed back.
    [[nodiscard]] QStringList produced() const { return m_produced; }
    /// How many steps ran, which is how far along it got.
    [[nodiscard]] int stepsRun() const { return m_stepsRun; }
    /// The reason it ended, in the words a strip or a run log can show.
    [[nodiscard]] QString endedBecause() const { return m_because; }

protected:
    void run() override;

private:
    /// The step count in the strip: which of how many, and its name.
    void announce(int index, const IChainStepKind& kind);

    Chain m_chain;
    const ChainRegistry* m_registry = nullptr;
    Ending m_ending = Ending::Ran;
    QStringList m_produced;
    int m_stepsRun = 0;
    QString m_because;
};

} // namespace mole
