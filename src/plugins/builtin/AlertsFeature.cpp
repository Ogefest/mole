#include "plugins/builtin/AlertsFeature.h"

#include "core/alerts/CheckAlertsTask.h"
#include "core/analysis/AnalysisStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QLocale>
#include <QRegularExpression>
#include <QUuid>

#include <optional>

namespace mole {
namespace {

    QString relativeTime(const QDateTime& when, const QDateTime& now)
    {
        if (!when.isValid())
            return QStringLiteral("never");
        const qint64 seconds = std::llabs(when.secsTo(now));
        if (seconds < 60)
            return QStringLiteral("just now");
        if (seconds < 3600)
            return QStringLiteral("%1 min ago").arg(seconds / 60);
        if (seconds < 86400)
            return QStringLiteral("%1 h ago").arg(seconds / 3600);
        return QStringLiteral("%1 d ago").arg(seconds / 86400);
    }

    QString stateLabel(AlertState state)
    {
        switch (state) {
        case AlertState::Unknown:
            return QStringLiteral("Not checked yet");
        case AlertState::Ok:
            return QStringLiteral("OK");
        case AlertState::Triggered:
            return QStringLiteral("Triggered");
        case AlertState::Failed:
            return QStringLiteral("Could not check");
        }
        return {};
    }

    /// The threshold as a number, or nothing when the text is not one.
    ///
    /// Nothing rather than 0, which is what this used to answer for "10 GBB",
    /// "ten" and an empty field alike -- and "above 0" fires on the first check
    /// of any folder that is not empty. `LiveSearchController::parseSize()`
    /// refuses the same shapes for the same reason.
    std::optional<double> thresholdFrom(const QString& text, AlertMetric metric)
    {
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty())
            return std::nullopt;

        if (alertMetricUnit(metric) != QLatin1String("bytes")) {
            // toDouble() rather than QLocale::toDouble(): strict, and it takes
            // the decimal comma once the comma has been turned into a point.
            QString number = trimmed;
            number.replace(QLatin1Char(','), QLatin1Char('.'));
            bool ok = false;
            const double value = number.toDouble(&ok);
            return ok ? std::optional<double>(value) : std::nullopt;
        }

        // "10 GB", "500m", "2 TiB" -- nobody types a byte count, and making them
        // is how a threshold ends up wrong by three orders of magnitude.
        static const QRegularExpression pattern(
            QStringLiteral("^\\s*([0-9]+(?:[.,][0-9]+)?)\\s*([kmgtp]?)i?b?\\s*$"),
            QRegularExpression::CaseInsensitiveOption);

        const QRegularExpressionMatch match = pattern.match(trimmed);
        if (!match.hasMatch())
            return std::nullopt;

        QString number = match.captured(1);
        number.replace(QLatin1Char(','), QLatin1Char('.'));
        bool ok = false;
        double value = number.toDouble(&ok);
        if (!ok)
            return std::nullopt;

        const QString scale = match.captured(2).toLower();
        if (scale == QLatin1String("k"))
            value *= 1024.0;
        else if (scale == QLatin1String("m"))
            value *= 1024.0 * 1024.0;
        else if (scale == QLatin1String("g"))
            value *= 1024.0 * 1024.0 * 1024.0;
        else if (scale == QLatin1String("t"))
            value *= 1024.0 * 1024.0 * 1024.0 * 1024.0;
        else if (scale == QLatin1String("p"))
            value *= 1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0;

        return value;
    }

    /// Every metric the evaluator can measure, in the order that reads best.
    const QList<AlertMetric>& metricOrder()
    {
        static const QList<AlertMetric> order = { AlertMetric::TotalSize, AlertMetric::FreeSpace,
            AlertMetric::FreeSpacePercent, AlertMetric::FileCount, AlertMetric::FolderCount,
            AlertMetric::LargestFile, AlertMetric::NewestFileAgeHours, AlertMetric::Permissions,
            AlertMetric::ModifiedTime, AlertMetric::Exists, AlertMetric::UnreadableFolders };
        return order;
    }

} // namespace

AlertsController::AlertsController(
    PluginServices services, AlertStore* store, AnalysisStore* analysis, QObject* parent)
    : FeatureController(QStringLiteral("Alerts"), parent)
    , m_services(services)
    , m_store(store)
    , m_analysis(analysis)
{
    if (m_store) {
        connect(m_store, &AlertStore::rulesChanged, this, &AlertsController::refresh);
        connect(m_store, &AlertStore::historyChanged, this, &AlertsController::historyChanged);
    }
    refresh();
}

AlertsController::~AlertsController()
{
    if (m_task)
        m_task->requestCancel();
}

void AlertsController::refresh()
{
    const int triggered = triggeredCount();
    setSubtitle(triggered > 0 ? QStringLiteral("%1 triggered").arg(triggered)
                              : QStringLiteral("%1 watched").arg(m_store ? m_store->rules().size() : 0));
    emit alertsChanged();
    emit historyChanged();
}

int AlertsController::triggeredCount() const
{
    return m_store ? m_store->triggeredCount() : 0;
}

void AlertsController::setSuggestedTarget(const QString& target)
{
    if (m_suggestedTarget == target)
        return;
    m_suggestedTarget = target;
    emit suggestedTargetChanged();
}

QVariantList AlertsController::alerts() const
{
    QVariantList out;
    if (!m_store)
        return out;

    const QDateTime now = QDateTime::currentDateTime();
    QList<AlertRule> rules = m_store->rules();

    // Triggered first, then failures, then the quiet ones: the reason to open
    // this tab is almost always that something fired.
    const auto rank = [](const AlertRule& rule) {
        switch (rule.state) {
        case AlertState::Triggered:
            return 0;
        case AlertState::Failed:
            return 1;
        case AlertState::Unknown:
            return 2;
        case AlertState::Ok:
            return 3;
        }
        return 4;
    };
    std::sort(rules.begin(), rules.end(), [&rank](const AlertRule& a, const AlertRule& b) {
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        return a.label.localeAwareCompare(b.label) < 0;
    });

    for (const AlertRule& rule : std::as_const(rules)) {
        out.append(QVariantMap {
            { QStringLiteral("id"), rule.id },
            { QStringLiteral("label"), rule.label },
            { QStringLiteral("target"), rule.targetUri },
            { QStringLiteral("condition"), rule.describe() },
            { QStringLiteral("metric"), alertMetricToString(rule.metric) },
            { QStringLiteral("source"), alertSourceToString(rule.source) },
            { QStringLiteral("sourceText"),
                rule.source == AlertSource::LatestReport ? QStringLiteral("from the latest report")
                                                         : QStringLiteral("measured live") },
            { QStringLiteral("enabled"), rule.enabled },
            { QStringLiteral("state"), alertStateToString(rule.state) },
            { QStringLiteral("stateText"), stateLabel(rule.state) },
            { QStringLiteral("triggered"), rule.state == AlertState::Triggered },
            { QStringLiteral("failed"), rule.state == AlertState::Failed },
            { QStringLiteral("value"), rule.lastValue },
            { QStringLiteral("message"), rule.message },
            { QStringLiteral("threshold"), rule.threshold },
            { QStringLiteral("lastCheckedText"), relativeTime(rule.lastCheckedAt, now) },
        });
    }
    return out;
}

QVariantList AlertsController::history() const
{
    QVariantList out;
    if (!m_store)
        return out;

    const QDateTime now = QDateTime::currentDateTime();
    const QList<AlertEvent> events = m_store->history({}, 200);
    for (const AlertEvent& event : events) {
        out.append(QVariantMap {
            { QStringLiteral("ruleId"), event.ruleId },
            { QStringLiteral("label"), event.ruleLabel },
            { QStringLiteral("state"), alertStateToString(event.state) },
            { QStringLiteral("stateText"), stateLabel(event.state) },
            { QStringLiteral("bad"),
                event.state == AlertState::Triggered || event.state == AlertState::Failed },
            { QStringLiteral("value"), event.value },
            { QStringLiteral("message"), event.message },
            { QStringLiteral("whenText"), relativeTime(event.at, now) },
            { QStringLiteral("at"), event.at.toString(QStringLiteral("yyyy-MM-dd HH:mm")) },
        });
    }
    return out;
}

QVariantList AlertsController::metricChoices() const
{
    QVariantList out;
    for (AlertMetric metric : metricOrder()) {
        out.append(QVariantMap { { QStringLiteral("id"), alertMetricToString(metric) },
            { QStringLiteral("label"), alertMetricLabel(metric) },
            { QStringLiteral("unit"), alertMetricUnit(metric) },
            { QStringLiteral("numeric"), alertMetricIsNumeric(metric) },
            { QStringLiteral("fromReport"), alertMetricNeedsTree(metric) } });
    }
    return out;
}

QVariantList AlertsController::comparisonChoices() const
{
    QVariantList out;
    for (AlertComparison comparison : { AlertComparison::Above, AlertComparison::Below,
             AlertComparison::Changed, AlertComparison::Equals }) {
        out.append(QVariantMap { { QStringLiteral("id"), alertComparisonToString(comparison) },
            { QStringLiteral("label"), alertComparisonLabel(comparison) },
            { QStringLiteral("needsNumber"), comparison != AlertComparison::Changed } });
    }
    return out;
}

QVariantList AlertsController::sourceChoices() const
{
    return { QVariantMap { { QStringLiteral("id"), QStringLiteral("live") },
                 { QStringLiteral("label"), QStringLiteral("Measure it now") } },
        QVariantMap { { QStringLiteral("id"), QStringLiteral("report") },
            { QStringLiteral("label"), QStringLiteral("Read the latest report") } } };
}

QString AlertsController::unitFor(const QString& metric) const
{
    const std::optional<AlertMetric> parsed = alertMetricFromString(metric);
    return parsed ? alertMetricUnit(*parsed) : QString();
}

bool AlertsController::metricNeedsNumber(const QString& metric) const
{
    const std::optional<AlertMetric> parsed = alertMetricFromString(metric);
    return parsed && alertMetricIsNumeric(*parsed);
}

QVariant AlertsController::parseThreshold(const QString& text, const QString& metric) const
{
    const std::optional<AlertMetric> parsed = alertMetricFromString(metric);
    if (!parsed)
        return {};
    const std::optional<double> value = thresholdFrom(text, *parsed);
    // An invalid QVariant reaches QML as `undefined`, which is what the Add
    // button tests. A number here would be indistinguishable from a real one.
    return value ? QVariant(*value) : QVariant();
}

QString AlertsController::addAlert(const QString& label, const QString& targetUri, const QString& metric,
    const QString& comparison, const QString& threshold, const QString& source)
{
    if (!m_store || targetUri.trimmed().isEmpty())
        return {};

    const std::optional<AlertMetric> parsedMetric = alertMetricFromString(metric);
    const std::optional<AlertComparison> parsedComparison = alertComparisonFromString(comparison);
    const std::optional<AlertSource> parsedSource = alertSourceFromString(source);
    if (!parsedMetric || !parsedComparison || !parsedSource)
        return {};

    AlertRule rule;
    rule.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    rule.targetUri = targetUri.trimmed();
    rule.metric = *parsedMetric;
    rule.comparison = *parsedComparison;
    rule.source = *parsedSource;

    // The form keeps Add off until the threshold reads, and this is the second
    // line. A threshold that could not be read used to arrive here as 0, and
    // "above 0" fires on the first check of anything that is not empty --
    // an alert the user never asked for, on a folder they were watching.
    if (*parsedComparison != AlertComparison::Changed && alertMetricIsNumeric(*parsedMetric)) {
        const std::optional<double> value = thresholdFrom(threshold, *parsedMetric);
        if (!value)
            return {};
        rule.threshold = *value;
    }

    rule.label = label.trimmed().isEmpty()
        ? QStringLiteral("%1 — %2").arg(VfsUri::fromString(rule.targetUri).fileName(), rule.describe())
        : label.trimmed();

    if (!m_store->put(rule))
        return {};

    // Checked straight away, so the user finds out whether it was a sensible
    // threshold rather than waiting for the next sweep to tell them.
    runCheck({ rule });
    return rule.id;
}

bool AlertsController::removeAlert(const QString& id)
{
    return m_store && m_store->remove(id);
}

bool AlertsController::setEnabled(const QString& id, bool enabled)
{
    if (!m_store)
        return false;
    AlertRule rule = m_store->rule(id);
    if (!rule.isValid())
        return false;
    rule.enabled = enabled;
    return m_store->put(rule);
}

bool AlertsController::setThreshold(const QString& id, double threshold)
{
    if (!m_store)
        return false;
    AlertRule rule = m_store->rule(id);
    if (!rule.isValid())
        return false;
    rule.threshold = threshold;
    if (!m_store->put(rule))
        return false;
    runCheck({ rule });
    return true;
}

void AlertsController::checkNow(const QString& id)
{
    if (!m_store)
        return;

    QList<AlertRule> rules;
    if (id.isEmpty()) {
        const QList<AlertRule> all = m_store->rules();
        for (const AlertRule& rule : all) {
            if (rule.enabled)
                rules.append(rule);
        }
    } else {
        const AlertRule rule = m_store->rule(id);
        if (rule.isValid())
            rules.append(rule);
    }
    runCheck(std::move(rules));
}

void AlertsController::runCheck(QList<AlertRule> rules)
{
    if (rules.isEmpty() || !m_services.isValid() || !m_store)
        return;

    if (m_task)
        m_task->requestCancel();

    setBusy(true);
    auto* task = new CheckAlertsTask(m_services.vfs, m_analysis, std::move(rules));
    m_task = task;

    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_task.clear();
        setBusy(false);

        if (task->state() != Task::State::Succeeded)
            return;

        const QDateTime at = QDateTime::currentDateTime();
        const QList<AlertRule> results = task->results();
        for (const AlertRule& rule : results) {
            m_store->put(rule);
            // Every check is recorded, not only the ones that fired: knowing a
            // value was fine an hour ago is half of what a history is for.
            m_store->record(AlertEvent { rule.id, rule.label, at, rule.state, rule.lastValue, rule.message });
        }
        refresh();
    });

    m_services.tasks->submit(task);
}

void AlertsController::clearHistory()
{
    if (m_store)
        m_store->clearHistory();
}

QVariantMap AlertsController::saveState() const
{
    return { { QStringLiteral("suggestedTarget"), m_suggestedTarget } };
}

void AlertsController::restoreState(const QVariantMap& state)
{
    setSuggestedTarget(state.value(QStringLiteral("suggestedTarget")).toString());
}

AlertsFeature::AlertsFeature(PluginServices services, AlertStore* store, AnalysisStore* analysis)
    : m_services(services)
    , m_store(store)
    , m_analysis(analysis)
{
}

QUrl AlertsFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/AlertsView.qml"));
}

FeatureController* AlertsFeature::createController(QObject* parent)
{
    return new AlertsController(m_services, m_store, m_analysis, parent);
}

} // namespace mole
