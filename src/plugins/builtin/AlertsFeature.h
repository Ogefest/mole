#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/PluginServices.h"

#include "core/alerts/AlertStore.h"

#include <QPointer>
#include <QVariant>
#include <QVariantList>

namespace mole {

class AnalysisStore;
class CheckAlertsTask;

/// The tab where alerts live: what is being watched, what tripped, and the
/// form for watching something else.
///
/// Everything is in one place on purpose. An alert defined somewhere you
/// cannot see the others is an alert you forget you set.
class AlertsController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(QVariantList alerts READ alerts NOTIFY alertsChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(int triggeredCount READ triggeredCount NOTIFY alertsChanged)
    /// The metric and comparison choices, so the form is built from the same
    /// table the evaluator measures against.
    Q_PROPERTY(QVariantList metricChoices READ metricChoices CONSTANT)
    Q_PROPERTY(QVariantList comparisonChoices READ comparisonChoices CONSTANT)
    Q_PROPERTY(QVariantList sourceChoices READ sourceChoices CONSTANT)
    /// Pre-filled into the form: wherever the user was when they opened this.
    Q_PROPERTY(
        QString suggestedTarget READ suggestedTarget WRITE setSuggestedTarget NOTIFY suggestedTargetChanged)

public:
    AlertsController(
        PluginServices services, AlertStore* store, AnalysisStore* analysis, QObject* parent = nullptr);
    ~AlertsController() override;

    QVariantList alerts() const;
    QVariantList history() const;
    int triggeredCount() const;
    QVariantList metricChoices() const;
    QVariantList comparisonChoices() const;
    QVariantList sourceChoices() const;

    QString suggestedTarget() const { return m_suggestedTarget; }
    void setSuggestedTarget(const QString& target);

    /// Creates a watch. Returns the new id, or an empty string when the input
    /// does not describe anything checkable -- which includes a threshold this
    /// cannot read. The threshold arrives as the text that was typed rather than
    /// as a number, because "10 GBB" and "10 GB" are different answers and a
    /// double cannot carry the difference.
    Q_INVOKABLE QString addAlert(const QString& label, const QString& targetUri, const QString& metric,
        const QString& comparison, const QString& threshold, const QString& source);
    Q_INVOKABLE bool removeAlert(const QString& id);
    Q_INVOKABLE bool setEnabled(const QString& id, bool enabled);
    Q_INVOKABLE bool setThreshold(const QString& id, double threshold);

    /// Checks one alert, or all of them when the id is empty.
    Q_INVOKABLE void checkNow(const QString& id = {});
    Q_INVOKABLE void clearHistory();

    /// A byte threshold is entered as "10 GB", not as 10737418240. Undefined in
    /// QML when the text is not a number this understands, which is what keeps
    /// the Add button off: it used to answer 0 for "10 GBB", "ten" and an empty
    /// field alike, and "above 0" fires on the first check.
    Q_INVOKABLE QVariant parseThreshold(const QString& text, const QString& metric) const;
    Q_INVOKABLE QString unitFor(const QString& metric) const;
    Q_INVOKABLE bool metricNeedsNumber(const QString& metric) const;

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void alertsChanged();
    void historyChanged();
    void suggestedTargetChanged();

private:
    void refresh();
    void runCheck(QList<AlertRule> rules);

    PluginServices m_services;
    AlertStore* m_store = nullptr;
    AnalysisStore* m_analysis = nullptr;
    QString m_suggestedTarget;
    QPointer<CheckAlertsTask> m_task;
};

class AlertsFeature final : public IFeature
{
public:
    AlertsFeature(PluginServices services, AlertStore* store, AnalysisStore* analysis);

    QString id() const override { return QStringLiteral("core.alerts"); }
    QString title() const override { return QStringLiteral("Alerts"); }
    QString description() const override
    {
        return QStringLiteral("Watch a metric on a file, folder, drive or report");
    }
    QString iconText() const override { return QStringLiteral("!"); }
    int sortOrder() const override { return 45; }
    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
    AlertStore* m_store = nullptr;
    AnalysisStore* m_analysis = nullptr;
};

} // namespace mole
