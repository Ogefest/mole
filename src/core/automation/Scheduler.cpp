#include "core/automation/Scheduler.h"

#include "core/automation/ScheduleStore.h"

namespace mole {

Scheduler::Scheduler(ScheduleStore* store, QObject* parent)
    : QObject(parent)
    , m_store(store)
{
    m_grace.setSingleShot(true);
    connect(&m_grace, &QTimer::timeout, this, [this] { checkDue(); });
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, [this] { checkDue(); });
}

Scheduler::~Scheduler() = default;

void Scheduler::setClock(Clock clock)
{
    m_clock = std::move(clock);
}

QDateTime Scheduler::now() const
{
    return m_clock ? m_clock() : QDateTime::currentDateTime();
}

void Scheduler::registerJob(const QString& jobKind, IScheduledJob* job)
{
    if (jobKind.isEmpty() || !job)
        return;
    m_jobs.insert(jobKind, job);
}

IScheduledJob* Scheduler::job(const QString& jobKind) const
{
    return m_jobs.value(jobKind, nullptr);
}

QStringList Scheduler::jobKinds() const
{
    QStringList kinds = m_jobs.keys();
    kinds.sort();
    return kinds;
}

void Scheduler::start(int pollIntervalMs, int graceMs)
{
    m_timer.setInterval(std::max(1000, pollIntervalMs));
    m_timer.start();

    // The window first, the scheduled work after it. This used to call checkDue()
    // here, synchronously, from inside AppController::initialise() -- so a rule that
    // was due submitted its scan before the window existed. On a large tree that is a
    // start that drags, and it was worse than that: the scan holds the index's one
    // mutex for the length of the walk, and session restore asks the index which
    // volumes it knows about, so the window never appeared at all. See MOLE-264.
    if (graceMs <= 0) {
        checkDue();
        return;
    }
    m_grace.setInterval(graceMs);
    m_grace.start();
}

void Scheduler::stop()
{
    m_timer.stop();
    // Or a grace still pending would start a job just after somebody asked for none.
    m_grace.stop();
}

int Scheduler::checkDue()
{
    if (!m_store)
        return 0;

    const QDateTime at = now();
    int dispatched = 0;
    const QList<ScheduleRule> rules = m_store->rules();
    for (const ScheduleRule& rule : rules) {
        if (m_inFlight.contains(rule.id))
            continue; // a slow job must not be started again by the next poll
        if (!rule.isDueAt(at))
            continue;
        if (dispatch(rule))
            ++dispatched;
    }
    return dispatched;
}

bool Scheduler::runNow(const QString& ruleId)
{
    if (!m_store || m_inFlight.contains(ruleId))
        return false;
    const ScheduleRule rule = m_store->rule(ruleId);
    if (!rule.isValid())
        return false;
    return dispatch(rule);
}

QString IScheduledJob::describeTarget(const ScheduleRule& rule) const
{
    // The first string parameter. QVariantMap is ordered by key, so this is the
    // same answer every time rather than whatever a hash gave today.
    for (auto it = rule.parameters.cbegin(); it != rule.parameters.cend(); ++it) {
        const QString text = it.value().toString();
        if (it.value().typeId() == QMetaType::QString && !text.isEmpty())
            return text;
    }
    return {};
}

bool Scheduler::dispatch(const ScheduleRule& rule)
{
    IScheduledJob* handler = job(rule.jobKind);
    const QDateTime startedAt = now();

    if (!handler) {
        // The plugin that provided this kind is gone. Recorded rather than
        // ignored: a rule that silently never runs is the worst outcome.
        skip(rule.id, startedAt, QStringLiteral("Nothing handles \"%1\" jobs").arg(rule.jobKind));
        return false;
    }

    m_inFlight.insert(rule.id, startedAt);

    ScheduleRule running = rule;
    running.lastStatus = RunStatus::Running;
    running.lastMessage.clear();
    // Recorded here and not only in finish(), so a run that never finishes still says
    // it started.
    //
    // `put()` saves immediately, and serialisation writes a Running rule out as Failed
    // on purpose -- a process that died mid-run did not succeed. What it used to
    // discard is this field, and without it dueAt() reads the rule as never run, which
    // means due now and staying due. So a job killed along with the process fired again
    // at the next start, and again after that: the loop MOLE-264's unreachable window
    // made unavoidable, because killing the process was the only way out of it.
    running.lastRunAt = startedAt;
    m_store->put(running);
    emit runStarted(rule.id);

    const QString ruleId = rule.id;
    // A QPointer and not `this`: both built-in jobs hold this callback inside a
    // Task::finished connection whose context is the job, which outlives the
    // scheduler in at least one teardown order -- so a scheduled scan still
    // running when the Scheduler goes would have called finish() on a dead
    // object.
    QPointer<Scheduler> alive(this);
    const StartOutcome outcome = handler->start(rule, [alive, ruleId](bool ok, QString message) {
        if (alive)
            alive->finish(ruleId, ok, message);
    });

    switch (outcome.what) {
    case StartOutcome::What::Started:
        return true;
    case StartOutcome::What::Skipped:
        // Not the rule's fault, so the streak the tracking list ranks by is left
        // alone. The reason is the job's own words.
        m_inFlight.remove(ruleId);
        skip(ruleId, startedAt,
            outcome.reason.isEmpty() ? QStringLiteral("Could not run just now") : outcome.reason);
        return false;
    case StartOutcome::What::Failed:
        finish(ruleId, false,
            outcome.reason.isEmpty() ? QStringLiteral("The job refused to start") : outcome.reason);
        return false;
    }
    return false;
}

void Scheduler::skip(const QString& ruleId, const QDateTime& at, const QString& reason)
{
    if (!m_store)
        return;

    ScheduleRule rule = m_store->rule(ruleId);
    if (!rule.isValid())
        return;
    // lastRunAt moves, so a rule whose drive is unplugged is not re-tried on
    // every poll -- it waits for its interval like any other. consecutiveFailures
    // is deliberately untouched: nothing about the rule failed.
    rule.lastRunAt = at;
    rule.lastStatus = RunStatus::Skipped;
    rule.lastMessage = reason;
    m_store->put(rule);
    m_store->record(RunRecord { rule.id, rule.label, at, at, RunStatus::Skipped, reason });
    emit runFinished(ruleId, false, reason);
}

void Scheduler::finish(const QString& ruleId, bool ok, const QString& message)
{
    const QDateTime startedAt = m_inFlight.take(ruleId);
    const QDateTime finishedAt = now();

    ScheduleRule rule = m_store ? m_store->rule(ruleId) : ScheduleRule {};
    if (rule.isValid()) {
        rule.lastRunAt = startedAt.isValid() ? startedAt : finishedAt;
        rule.lastStatus = ok ? RunStatus::Succeeded : RunStatus::Failed;
        rule.lastMessage = message;
        if (ok) {
            rule.lastSuccessAt = finishedAt;
            rule.consecutiveFailures = 0;
        } else {
            ++rule.consecutiveFailures;
        }
        m_store->put(rule);
        m_store->record(RunRecord { rule.id, rule.label, startedAt.isValid() ? startedAt : finishedAt,
            finishedAt, rule.lastStatus, message });
    }

    emit runFinished(ruleId, ok, message);
}

QStringList Scheduler::runningRules() const
{
    return m_inFlight.keys();
}

} // namespace mole
