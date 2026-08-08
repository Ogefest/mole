#pragma once

#include "core/automation/ScheduleRule.h"

#include <QObject>
#include <QTimer>

#include <functional>

namespace mole {

class ScheduleStore;

/// Runs one kind of scheduled job.
///
/// Implemented by whoever owns the work -- the analysis feature registers one
/// for "analysis". The scheduler never learns what a report is.
class IScheduledJob
{
public:
    virtual ~IScheduledJob() = default;

    /// Human name for the tracking list ("Directory report").
    virtual QString displayName() const = 0;

    /// Starts the job and returns. `done` must be called exactly once, on the
    /// scheduler's thread, when the work finishes -- including when it fails.
    /// Returning false means it could not even start, and `done` will not be
    /// called.
    virtual bool start(const ScheduleRule& rule, std::function<void(bool ok, QString message)> done) = 0;
};

/// Decides what is due and starts it.
///
/// It polls rather than arming a timer per rule: a laptop that was asleep for
/// two days must notice on waking, and a wall-clock timer set before the sleep
/// would not fire. The poll is cheap -- a comparison per rule -- and catching
/// up late is exactly the desired behaviour for a report that was missed.
class Scheduler : public QObject
{
    Q_OBJECT

public:
    /// Injectable so tests can move time without waiting for it.
    using Clock = std::function<QDateTime()>;

    explicit Scheduler(ScheduleStore* store, QObject* parent = nullptr);
    ~Scheduler() override;

    void setClock(Clock clock);

    /// The rules and run log this scheduler works from.
    ScheduleStore* store() const { return m_store; }

    /// Registers the handler for a job kind. Replacing an existing kind is
    /// allowed so a plugin reloading does not orphan its rules.
    void registerJob(const QString& jobKind, IScheduledJob* job);
    IScheduledJob* job(const QString& jobKind) const;
    QStringList jobKinds() const;

    /// Begins polling. The first check happens immediately, so a rule that
    /// came due while the application was closed runs on start.
    void start(int pollIntervalMs = 60000);
    void stop();
    bool isRunning() const { return m_timer.isActive(); }

    /// Starts everything due now. Returns how many were dispatched.
    int checkDue();
    /// Starts one rule regardless of when it is next due.
    bool runNow(const QString& ruleId);

    /// Rules currently in flight.
    QStringList runningRules() const;

signals:
    /// A run started. The tracking list uses these to show live state without
    /// polling the store.
    void runStarted(const QString& ruleId);
    void runFinished(const QString& ruleId, bool ok, const QString& message);

private:
    bool dispatch(const ScheduleRule& rule);
    void finish(const QString& ruleId, bool ok, const QString& message);
    QDateTime now() const;

    ScheduleStore* m_store = nullptr;
    QHash<QString, IScheduledJob*> m_jobs;
    QHash<QString, QDateTime> m_inFlight;
    QTimer m_timer;
    Clock m_clock;
};

} // namespace mole
