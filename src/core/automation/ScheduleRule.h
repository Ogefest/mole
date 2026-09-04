#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>

namespace mole {

/// How a scheduled run ended.
enum class RunStatus {
    Never, ///< it has not run yet
    Running, ///< dispatched and still working
    Succeeded, ///< finished and produced what it promised
    Failed, ///< finished without producing it
    /// Could not start: nothing mounted, target gone, nobody handles the kind.
    ///
    /// **Not counted towards consecutiveFailures**, because nothing about the
    /// rule is wrong -- see StartOutcome in Scheduler.h.
    Skipped,
};

QString runStatusToString(RunStatus status);
RunStatus runStatusFromString(const QString& text);

/// A job the application should repeat on its own.
///
/// The rule holds only what a job needs to be started again from cold: which
/// kind of job it is, its parameters, and when it last ran. The scheduler
/// deliberately knows nothing about analysis, or any other job kind -- that
/// belongs to whoever registers a runner for `jobKind`.
struct ScheduleRule
{
    QString id;
    /// Which registered runner handles this. "analysis" is the built-in one.
    QString jobKind;
    /// What the user called it, shown in the tracking list.
    QString label;
    /// Job-specific input. For analysis: "rootUri".
    QVariantMap parameters;

    /// How long after a run the next one is due.
    qint64 intervalSeconds = 86400;
    bool enabled = true;

    /// Invalid until the first run. An unrun rule is due immediately, which is
    /// what makes a rule created while the machine was off catch up on start.
    QDateTime lastRunAt;
    QDateTime lastSuccessAt;
    RunStatus lastStatus = RunStatus::Never;
    QString lastMessage;
    /// Reset by any success. Surfaced so a rule failing every night is
    /// distinguishable from one that failed once.
    int consecutiveFailures = 0;

    bool isValid() const { return !id.isEmpty() && !jobKind.isEmpty(); }

    /// The shortest interval the scheduler can honour.
    ///
    /// The poll is a minute, so anything below this is either a rule that fires
    /// every poll or -- at zero or negative -- one that is always due, which
    /// spins the scheduler.
    static constexpr qint64 kMinimumIntervalSeconds = 60;
    /// Brings the interval up to something that can be honoured.
    ///
    /// Called by fromJson() *and* by ScheduleStore::put(), because the floor used
    /// to live only in fromJson(): the three callers that build a rule and set
    /// intervalSeconds directly could store a 0 that no reload would ever see,
    /// because a spinning scheduler is a symptom of the running process rather
    /// than of the file. See MOLE-379.
    void clampInterval()
    {
        if (intervalSeconds < kMinimumIntervalSeconds)
            intervalSeconds = kMinimumIntervalSeconds;
    }

    /// When this rule wants to run next. Invalid when it is disabled; the
    /// epoch-ish "long ago" when it has never run, so it sorts first.
    QDateTime dueAt() const;
    bool isDueAt(const QDateTime& now) const;

    QJsonObject toJson() const;
    static ScheduleRule fromJson(const QJsonObject& json);

    /// The named intervals the interface offers. Any second count is legal;
    /// these are only what the picker shows.
    static QList<QPair<QString, qint64>> presets();
    /// "Every day", "Every 3 hours" -- for the tracking list.
    static QString describeInterval(qint64 seconds);
};

/// One attempt, kept so a failure is still visible tomorrow.
struct RunRecord
{
    QString ruleId;
    QString ruleLabel;
    QDateTime startedAt;
    QDateTime finishedAt;
    RunStatus status = RunStatus::Never;
    QString message;

    qint64 durationMs() const;

    QJsonObject toJson() const;
    static RunRecord fromJson(const QJsonObject& json);
};

} // namespace mole
