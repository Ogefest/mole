#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"

#include "core/automation/Scheduler.h"

#include <QTimer>
#include <QVariantList>

namespace mole {

class ScheduleStore;

/// The tab that shows what runs by itself, and whether it worked.
///
/// Automation that fails quietly is worse than no automation: the user stops
/// checking and trusts numbers that stopped being refreshed weeks ago. So the
/// failure count is a headline, not a detail, and every attempt is listed --
/// including the ones that never got as far as starting.
class AutomationController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(QVariantList rules READ rules NOTIFY rulesChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QVariantList intervalPresets READ intervalPresets CONSTANT)
    /// Rules whose most recent run did not succeed.
    Q_PROPERTY(int failingCount READ failingCount NOTIFY rulesChanged)
    Q_PROPERTY(int runningCount READ runningCount NOTIFY rulesChanged)
    Q_PROPERTY(QString historyFilter READ historyFilter WRITE setHistoryFilter NOTIFY historyFilterChanged)

public:
    AutomationController(ScheduleStore* store, Scheduler* scheduler, QObject* parent = nullptr);

    QVariantList rules() const;
    QVariantList history() const;
    QVariantList intervalPresets() const;
    int failingCount() const;
    int runningCount() const;

    QString historyFilter() const { return m_historyFilter; }
    void setHistoryFilter(const QString& filter);

    /// Runs it now, whatever its schedule says. For "is it broken, or was it
    /// just not its turn yet".
    Q_INVOKABLE bool runNow(const QString& ruleId);
    Q_INVOKABLE bool setEnabled(const QString& ruleId, bool enabled);
    Q_INVOKABLE bool setInterval(const QString& ruleId, qint64 seconds);
    Q_INVOKABLE bool rename(const QString& ruleId, const QString& label);
    Q_INVOKABLE bool removeRule(const QString& ruleId);
    /// Clears the run log. The rules themselves are untouched.
    Q_INVOKABLE void clearHistory();

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void rulesChanged();
    void historyChanged();
    void historyFilterChanged();

private:
    void refresh();

    ScheduleStore* m_store = nullptr;
    Scheduler* m_scheduler = nullptr;
    QString m_historyFilter;
    QTimer m_tick;
};

class AutomationFeature final : public IFeature
{
public:
    AutomationFeature(ScheduleStore* store, Scheduler* scheduler);

    QString id() const override { return QStringLiteral("core.automation"); }
    QString title() const override { return QStringLiteral("Automation"); }
    QString description() const override
    {
        return QStringLiteral("Jobs that run on their own, and how they went");
    }
    QString iconText() const override { return QStringLiteral("⟳"); }
    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    ScheduleStore* m_store = nullptr;
    Scheduler* m_scheduler = nullptr;
};

} // namespace mole
