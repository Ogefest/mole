#pragma once

#include "core/automation/ScheduleRule.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

namespace mole {

class ScheduleStore;

/// Runs one kind of scheduled job.
///
/// Implemented by whoever owns the work -- the analysis feature registers one
/// for "analysis". The scheduler never learns what a report is.
/// What start() managed to do, and why when it did not start.
///
/// **A bare bool could not tell "the backup disk is unplugged" from "this job is
/// broken", and the difference is the whole of what the tracking list is for.**
/// Both built-in jobs returned false for an unmounted root, believing -- their
/// comments said so -- that the scheduler would record Skipped; it recorded
/// Failed with "The job refused to start" and counted it towards the streak the
/// tab ranks by. A laptop whose backup disk was unplugged for a week showed
/// "Failed x7", above a rule that really was broken, with a message that did not
/// say why. And the job's own reason -- "No drive is mounted for ..." -- had
/// nowhere to travel. See MOLE-405's sibling MOLE-379.
struct StartOutcome
{
    enum class What {
        Started, ///< running; `done` will be called
        Skipped, ///< could not run through no fault of the rule; `done` will not be called
        Failed, ///< the rule itself is wrong; `done` will not be called
    };

    What what = What::Started;
    /// One sentence for a person, for Skipped and Failed. Empty for Started.
    QString reason;

    static StartOutcome started() { return {}; }
    static StartOutcome skipped(QString why) { return { What::Skipped, std::move(why) }; }
    static StartOutcome failed(QString why) { return { What::Failed, std::move(why) }; }
};

class IScheduledJob
{
public:
    virtual ~IScheduledJob() = default;

    /// Human name for the tracking list ("Directory report").
    virtual QString displayName() const = 0;

    /// Starts the job and returns. `done` must be called exactly once, on the
    /// scheduler's thread, when the work finishes -- including when it fails.
    ///
    /// Anything but `Started` means `done` will not be called, and the reason
    /// given is what the tracking list shows. **Skipped for a condition outside
    /// the rule** -- nothing mounted, the target gone, the same volume already
    /// being scanned -- which is recorded without touching the failure streak.
    /// **Failed for a rule that cannot work**: a parameter missing, a service
    /// absent.
    virtual StartOutcome start(const ScheduleRule& rule, std::function<void(bool ok, QString message)> done)
        = 0;

    /// What this rule is aimed at, for the tracking list.
    ///
    /// The default is the first string parameter, which is right for every job
    /// whose target is one place. The tracking tab used to read
    /// `parameters["rootUri"]` itself -- the one place a generic tab knew a
    /// built-in job's key, so a plugin's job showed an empty target.
    virtual QString describeTarget(const ScheduleRule& rule) const;
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

    /// How long after start() the first check waits, by default.
    ///
    /// A rule that came due while the application was closed should run -- but not
    /// while the window is still being built. Starting a scan of a large tree during
    /// startup makes the application feel like it is dragging itself up, and in the
    /// worst case takes the window with it: the scan holds the index's mutex and the
    /// first thing the interface does is ask the index what volumes there are. So the
    /// window goes first and the scheduled work follows a few seconds later, by which
    /// time somebody is looking at something. See MOLE-264.
    static constexpr int kStartupGraceMs = 5000;
    /// Begins polling. The first check waits `graceMs`; pass 0 for immediately, which
    /// is what a test wants when it is not testing the wait.
    void start(int pollIntervalMs = 60000, int graceMs = kStartupGraceMs);
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
    /// Records a run that could not start through no fault of the rule. Moves
    /// lastRunAt, leaves consecutiveFailures alone. See StartOutcome.
    void skip(const QString& ruleId, const QDateTime& at, const QString& reason);
    QDateTime now() const;

    ScheduleStore* m_store = nullptr;
    QHash<QString, IScheduledJob*> m_jobs;
    QHash<QString, QDateTime> m_inFlight;
    QTimer m_timer;
    /// The wait before the first check. Its own timer rather than a singleShot, so
    /// stop() can cancel it -- a grace still pending after stop() would start a job
    /// in a test that had just asked for silence.
    QTimer m_grace;
    Clock m_clock;
};

} // namespace mole
