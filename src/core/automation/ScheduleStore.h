#pragma once

#include "core/automation/ScheduleRule.h"

#include <QList>
#include <QObject>

namespace mole {

/// Where the schedule and its run log live.
///
/// One file, written whole and atomically: the list is small, and a torn write
/// that lost the schedule would silently stop every automated job.
class ScheduleStore : public QObject
{
    Q_OBJECT

public:
    explicit ScheduleStore(QString path, QObject* parent = nullptr);

    /// Honours MOLE_SCHEDULE_PATH so tests and the harness never touch the
    /// user's real schedule.
    static QString defaultPath();

    bool load();
    bool save() const;

    QList<ScheduleRule> rules() const { return m_rules; }
    ScheduleRule rule(const QString& id) const;
    /// Adds or replaces by id. Returns false for a rule with no id or kind.
    bool put(const ScheduleRule& rule);
    bool remove(const QString& id);

    /// Newest first. `ruleId` empty means every rule.
    QList<RunRecord> history(const QString& ruleId = {}, int limit = 100) const;
    void record(const RunRecord& record);
    /// Drops the run log. The rules keep their own last-run state, so this
    /// clears the list without pretending the jobs never ran.
    void clearHistory();

    /// How many runs are kept before the oldest are dropped.
    void setHistoryLimit(int limit) { m_historyLimit = std::max(1, limit); }

signals:
    void rulesChanged();
    void historyChanged();

private:
    void trimHistory();

    QString m_path;
    QList<ScheduleRule> m_rules;
    QList<RunRecord> m_history;
    int m_historyLimit = 200;
};

} // namespace mole
