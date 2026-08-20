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

bool Scheduler::dispatch(const ScheduleRule& rule)
{
    IScheduledJob* handler = job(rule.jobKind);
    const QDateTime startedAt = now();

    if (!handler) {
        // The plugin that provided this kind is gone. Recorded rather than
        // ignored: a rule that silently never runs is the worst outcome.
        ScheduleRule updated = rule;
        updated.lastRunAt = startedAt;
        updated.lastStatus = RunStatus::Skipped;
        updated.lastMessage = QStringLiteral("Nothing handles \"%1\" jobs").arg(rule.jobKind);
        m_store->put(updated);
        m_store->record(
            RunRecord { rule.id, rule.label, startedAt, startedAt, RunStatus::Skipped, updated.lastMessage });
        emit runFinished(rule.id, false, updated.lastMessage);
        return false;
    }

    m_inFlight.insert(rule.id, startedAt);

    ScheduleRule running = rule;
    running.lastStatus = RunStatus::Running;
    running.lastMessage.clear();
    m_store->put(running);
    emit runStarted(rule.id);

    const QString ruleId = rule.id;
    const bool started
        = handler->start(rule, [this, ruleId](bool ok, QString message) { finish(ruleId, ok, message); });

    if (!started) {
        finish(ruleId, false, QStringLiteral("The job refused to start"));
        return false;
    }
    return true;
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
