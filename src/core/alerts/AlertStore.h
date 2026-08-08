#pragma once

#include "core/alerts/AlertRule.h"

#include <QList>
#include <QObject>

namespace mole {

/// Where the alerts and their history live. One file, written whole.
class AlertStore : public QObject
{
    Q_OBJECT

public:
    explicit AlertStore(QString path, QObject* parent = nullptr);

    /// Honours MOLE_ALERTS_PATH so tests never touch the real one.
    static QString defaultPath();

    bool load();
    bool save() const;

    QList<AlertRule> rules() const { return m_rules; }
    AlertRule rule(const QString& id) const;
    bool put(const AlertRule& rule);
    bool remove(const QString& id);

    /// Newest first. An empty id means every alert.
    QList<AlertEvent> history(const QString& ruleId = {}, int limit = 100) const;
    void record(const AlertEvent& event);
    void clearHistory();

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

    QString m_path;
    QList<AlertRule> m_rules;
    QList<AlertEvent> m_history;
    int m_historyLimit = 200;
};

} // namespace mole
