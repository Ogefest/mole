#pragma once

#include "core/automation/ScheduleRule.h"
#include "core/data/JsonFileStore.h"

#include <QList>
#include <QObject>

namespace mole {

/// Where the schedule and its run log live.
///
/// One file, written whole and atomically: the list is small, and a torn write
/// that lost the schedule would silently stop every automated job.
class ScheduleStore : public JsonFileStore
{
    Q_OBJECT

public:
    explicit ScheduleStore(QString path, QObject* parent = nullptr);

    /// Honours MOLE_SCHEDULE_PATH so tests and the harness never touch the
    /// user's real schedule.
    static QString defaultPath();

    /// False when the file is there and could not be read: it has been kept
    /// beside itself and this store will not write over it. A job that quietly never runs is the one failure
    /// nobody can diagnose.
    bool load();
    [[nodiscard]] bool save();

    QList<ScheduleRule> rules() const { return m_rules; }
    ScheduleRule rule(const QString& id) const;
    /// Adds or replaces by id. Returns false for a rule with no id or kind.
    /// False when the rule was refused *or* when it was taken and the file
    /// could not be written. A schedule that did not reach the disk is the case
    /// ARCHITECTURE.md's "a job that quietly never runs is the one failure
    /// nobody can diagnose" is about. See ADR-0089.
    /// Adds or replaces a rule. The interval is clamped to
    /// ScheduleRule::kMinimumIntervalSeconds on the way in -- see clampInterval().
    bool put(const ScheduleRule& rule);
    bool remove(const QString& id);

    /// Newest first. `ruleId` empty means every rule.
    QList<RunRecord> history(const QString& ruleId = {}, int limit = 100) const;
    bool record(const RunRecord& record);
    /// Drops the run log. The rules keep their own last-run state, so this
    /// clears the list without pretending the jobs never ran.
    bool clearHistory();

    /// How many runs are kept before the oldest are dropped.
    void setHistoryLimit(int limit) { m_historyLimit = std::max(1, limit); }

signals:
    void rulesChanged();
    void historyChanged();

private:
    void trimHistory();

    QList<ScheduleRule> m_rules;
    QList<RunRecord> m_history;
    int m_historyLimit = 200;
};

} // namespace mole
