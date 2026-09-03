#pragma once

#include "core/alerts/AlertRule.h"
#include "core/data/JsonFileStore.h"

#include <QList>
#include <QObject>

namespace mole {

/// Where the alerts and their history live. One file, written whole.
class AlertStore : public JsonFileStore
{
    Q_OBJECT

public:
    explicit AlertStore(QString path, QObject* parent = nullptr);

    /// Honours MOLE_ALERTS_PATH so tests never touch the real one.
    static QString defaultPath();

    /// False when the file is there and could not be read: it has been kept
    /// beside itself and this store will not write over it. A rule nobody rewrote is a watch nobody is
    /// keeping.
    bool load();
    [[nodiscard]] bool save();

    QList<AlertRule> rules() const { return m_rules; }
    AlertRule rule(const QString& id) const;
    /// False when the rule was refused *or* when it was taken and the file
    /// could not be written -- the two are told apart by JsonFileStore's
    /// saveFailed(), which the shell turns into a notification. See ADR-0089.
    bool put(const AlertRule& rule);
    bool remove(const QString& id);

    /// Newest first. An empty id means every alert.
    QList<AlertEvent> history(const QString& ruleId = {}, int limit = 100) const;
    bool record(const AlertEvent& event);
    bool clearHistory();

    /// Alerts currently outside their bounds.
    int triggeredCount() const;

    void setHistoryLimit(int limit) { m_historyLimit = std::max(1, limit); }

signals:
    void rulesChanged();
    void historyChanged();
    /// A rule went from anything else to Triggered. The shell turns this into
    /// a notification -- an alert nobody is told about is not an alert.
    void alertRaised(const AlertRule& rule);
    void alertCleared(const AlertRule& rule);

private:
    void trimHistory();

    QList<AlertRule> m_rules;
    QList<AlertEvent> m_history;
    int m_historyLimit = 200;
};

} // namespace mole
