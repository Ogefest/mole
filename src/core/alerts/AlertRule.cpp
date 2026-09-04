#include "core/alerts/AlertRule.h"

#include "core/text/SizeWords.h"

#include <QLocale>

namespace mole {
namespace {

    struct MetricInfo
    {
        const char* id;
        const char* label;
        const char* unit;
        bool numeric;
        bool needsTree;
    };

    /// One row per metric. Everything the interface and the evaluator need to know
    /// about a metric lives here, so adding one is a single edit.
    const QHash<AlertMetric, MetricInfo>& metricTable()
    {
        static const QHash<AlertMetric, MetricInfo> table = {
            { AlertMetric::TotalSize, { "totalSize", "Total size", "bytes", true, true } },
            { AlertMetric::FileCount, { "fileCount", "Number of files", "files", true, true } },
            { AlertMetric::FolderCount, { "folderCount", "Number of folders", "folders", true, true } },
            { AlertMetric::FreeSpace, { "freeSpace", "Free space", "bytes", true, false } },
            { AlertMetric::FreeSpacePercent, { "freeSpacePercent", "Free space (%)", "%", true, false } },
            { AlertMetric::LargestFile, { "largestFile", "Largest file", "bytes", true, true } },
            { AlertMetric::NewestFileAgeHours,
                { "newestFileAge", "Hours since anything changed", "hours", true, true } },
            { AlertMetric::Permissions, { "permissions", "Permissions", "", false, false } },
            { AlertMetric::ModifiedTime, { "modifiedTime", "Last modified", "", false, false } },
            { AlertMetric::Exists, { "exists", "Exists", "", true, false } },
            { AlertMetric::UnreadableFolders,
                { "unreadableFolders", "Unreadable folders", "folders", true, true } },
        };
        return table;
    }

} // namespace

QString alertMetricToString(AlertMetric metric)
{
    return QString::fromLatin1(metricTable().value(metric).id);
}

std::optional<AlertMetric> alertMetricFromString(const QString& text)
{
    for (auto it = metricTable().constBegin(); it != metricTable().constEnd(); ++it) {
        if (text == QLatin1String(it.value().id))
            return it.key();
    }
    return std::nullopt;
}

QString alertMetricLabel(AlertMetric metric)
{
    return QString::fromLatin1(metricTable().value(metric).label);
}

QString alertMetricUnit(AlertMetric metric)
{
    return QString::fromLatin1(metricTable().value(metric).unit);
}

bool alertMetricIsNumeric(AlertMetric metric)
{
    return metricTable().value(metric).numeric;
}

bool alertMetricNeedsTree(AlertMetric metric)
{
    return metricTable().value(metric).needsTree;
}

QString alertComparisonToString(AlertComparison comparison)
{
    switch (comparison) {
    case AlertComparison::Above:
        return QStringLiteral("above");
    case AlertComparison::Below:
        return QStringLiteral("below");
    case AlertComparison::Changed:
        return QStringLiteral("changed");
    case AlertComparison::Equals:
        return QStringLiteral("equals");
    }
    return QStringLiteral("above");
}

std::optional<AlertComparison> alertComparisonFromString(const QString& text)
{
    if (text == QLatin1String("above"))
        return AlertComparison::Above;
    if (text == QLatin1String("below"))
        return AlertComparison::Below;
    if (text == QLatin1String("changed"))
        return AlertComparison::Changed;
    if (text == QLatin1String("equals"))
        return AlertComparison::Equals;
    return std::nullopt;
}

QString alertComparisonLabel(AlertComparison comparison)
{
    switch (comparison) {
    case AlertComparison::Above:
        return QStringLiteral("is above");
    case AlertComparison::Below:
        return QStringLiteral("is below");
    case AlertComparison::Changed:
        return QStringLiteral("changes");
    case AlertComparison::Equals:
        return QStringLiteral("equals");
    }
    return {};
}

QString alertSourceToString(AlertSource source)
{
    return source == AlertSource::LatestReport ? QStringLiteral("report") : QStringLiteral("live");
}

std::optional<AlertSource> alertSourceFromString(const QString& text)
{
    if (text == QLatin1String("report"))
        return AlertSource::LatestReport;
    if (text == QLatin1String("live"))
        return AlertSource::Live;
    return std::nullopt;
}

QString alertStateToString(AlertState state)
{
    switch (state) {
    case AlertState::Unknown:
        return QStringLiteral("unknown");
    case AlertState::Ok:
        return QStringLiteral("ok");
    case AlertState::Triggered:
        return QStringLiteral("triggered");
    case AlertState::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

AlertState alertStateFromString(const QString& text)
{
    if (text == QLatin1String("ok"))
        return AlertState::Ok;
    if (text == QLatin1String("triggered"))
        return AlertState::Triggered;
    if (text == QLatin1String("failed"))
        return AlertState::Failed;
    return AlertState::Unknown;
}

QString AlertRule::describe() const
{
    const QString metricName = alertMetricLabel(metric);
    if (comparison == AlertComparison::Changed)
        return QStringLiteral("%1 changes").arg(metricName);

    const QString unit = alertMetricUnit(metric);
    QString amount;
    if (unit == QLatin1String("bytes"))
        amount = sizeInWords(static_cast<qint64>(threshold));
    else if (unit.isEmpty())
        amount = QLocale().toString(threshold, 'g', 6);
    else
        amount = QStringLiteral("%1 %2").arg(QLocale().toString(threshold, 'g', 6), unit);

    return QStringLiteral("%1 %2 %3").arg(metricName, alertComparisonLabel(comparison), amount);
}

QJsonObject AlertRule::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("id")] = id;
    json[QStringLiteral("label")] = label;
    json[QStringLiteral("targetUri")] = targetUri;
    json[QStringLiteral("metric")] = alertMetricToString(metric);
    json[QStringLiteral("comparison")] = alertComparisonToString(comparison);
    json[QStringLiteral("source")] = alertSourceToString(source);
    json[QStringLiteral("threshold")] = threshold;
    json[QStringLiteral("enabled")] = enabled;
    json[QStringLiteral("state")] = alertStateToString(state);
    json[QStringLiteral("lastValue")] = lastValue;
    json[QStringLiteral("lastNumericValue")] = lastNumericValue;
    if (lastCheckedAt.isValid())
        json[QStringLiteral("lastCheckedAt")] = lastCheckedAt.toUTC().toString(Qt::ISODate);
    if (triggeredAt.isValid())
        json[QStringLiteral("triggeredAt")] = triggeredAt.toUTC().toString(Qt::ISODate);
    json[QStringLiteral("message")] = message;
    return json;
}

std::optional<AlertRule> AlertRule::fromJson(const QJsonObject& json)
{
    const std::optional<AlertMetric> metric
        = alertMetricFromString(json.value(QStringLiteral("metric")).toString());
    const std::optional<AlertComparison> comparison
        = alertComparisonFromString(json.value(QStringLiteral("comparison")).toString());
    const std::optional<AlertSource> source
        = alertSourceFromString(json.value(QStringLiteral("source")).toString());
    if (!metric || !comparison || !source)
        return std::nullopt;

    AlertRule rule;
    rule.id = json.value(QStringLiteral("id")).toString();
    rule.label = json.value(QStringLiteral("label")).toString();
    rule.targetUri = json.value(QStringLiteral("targetUri")).toString();
    rule.metric = *metric;
    rule.comparison = *comparison;
    rule.source = *source;
    rule.threshold = json.value(QStringLiteral("threshold")).toDouble();
    rule.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    rule.state = alertStateFromString(json.value(QStringLiteral("state")).toString());
    rule.lastValue = json.value(QStringLiteral("lastValue")).toString();
    rule.lastNumericValue = json.value(QStringLiteral("lastNumericValue")).toDouble();
    rule.lastCheckedAt
        = QDateTime::fromString(json.value(QStringLiteral("lastCheckedAt")).toString(), Qt::ISODate);
    rule.triggeredAt
        = QDateTime::fromString(json.value(QStringLiteral("triggeredAt")).toString(), Qt::ISODate);
    rule.message = json.value(QStringLiteral("message")).toString();
    return rule;
}

QJsonObject AlertEvent::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("ruleId")] = ruleId;
    json[QStringLiteral("ruleLabel")] = ruleLabel;
    json[QStringLiteral("at")] = at.toUTC().toString(Qt::ISODateWithMs);
    json[QStringLiteral("state")] = alertStateToString(state);
    json[QStringLiteral("value")] = value;
    json[QStringLiteral("message")] = message;
    return json;
}

AlertEvent AlertEvent::fromJson(const QJsonObject& json)
{
    AlertEvent event;
    event.ruleId = json.value(QStringLiteral("ruleId")).toString();
    event.ruleLabel = json.value(QStringLiteral("ruleLabel")).toString();
    event.at = QDateTime::fromString(json.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs);
    event.state = alertStateFromString(json.value(QStringLiteral("state")).toString());
    event.value = json.value(QStringLiteral("value")).toString();
    event.message = json.value(QStringLiteral("message")).toString();
    return event;
}

} // namespace mole
