#include "core/automation/ScheduleRule.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace mole {
namespace {

    constexpr qint64 kHour = 3600;
    constexpr qint64 kDay = 24 * kHour;

} // namespace

QString runStatusToString(RunStatus status)
{
    switch (status) {
    case RunStatus::Never:
        return QStringLiteral("never");
    case RunStatus::Running:
        return QStringLiteral("running");
    case RunStatus::Succeeded:
        return QStringLiteral("succeeded");
    case RunStatus::Failed:
        return QStringLiteral("failed");
    case RunStatus::Skipped:
        return QStringLiteral("skipped");
    }
    return QStringLiteral("never");
}

RunStatus runStatusFromString(const QString& text)
{
    if (text == QLatin1String("running"))
        return RunStatus::Running;
    if (text == QLatin1String("succeeded"))
        return RunStatus::Succeeded;
    if (text == QLatin1String("failed"))
        return RunStatus::Failed;
    if (text == QLatin1String("skipped"))
        return RunStatus::Skipped;
    return RunStatus::Never;
}

QDateTime ScheduleRule::dueAt() const
{
    if (!enabled)
        return {};
    if (!lastRunAt.isValid()) {
        // Never run: due now, and stays due. This is what makes a rule whose
        // turn came while the application was closed run on the next start
        // rather than waiting a whole further interval.
        return QDateTime::fromSecsSinceEpoch(0);
    }
    return lastRunAt.addSecs(intervalSeconds);
}

bool ScheduleRule::isDueAt(const QDateTime& now) const
{
    if (!enabled || lastStatus == RunStatus::Running)
        return false;
    const QDateTime due = dueAt();
    return due.isValid() && due <= now;
}

QJsonObject ScheduleRule::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("id")] = id;
    json[QStringLiteral("jobKind")] = jobKind;
    json[QStringLiteral("label")] = label;
    json[QStringLiteral("parameters")] = QJsonObject::fromVariantMap(parameters);
    json[QStringLiteral("intervalSeconds")] = static_cast<double>(intervalSeconds);
    json[QStringLiteral("enabled")] = enabled;
    if (lastRunAt.isValid())
        json[QStringLiteral("lastRunAt")] = lastRunAt.toUTC().toString(Qt::ISODate);
    if (lastSuccessAt.isValid())
        json[QStringLiteral("lastSuccessAt")] = lastSuccessAt.toUTC().toString(Qt::ISODate);
    // A run interrupted by a crash or a quit must not come back as "running"
    // and be skipped forever; it is filed as a failure it can recover from.
    //
    // **And it says so, and is counted.** This wrote the status and left the
    // message empty and the streak untouched, so the tracking tab showed a
    // failure that would not say why and the number it ranks rules by did not
    // count it. Written here rather than at the moment of the interruption,
    // because nothing runs then. See MOLE-379.
    if (lastStatus == RunStatus::Running) {
        json[QStringLiteral("lastStatus")] = runStatusToString(RunStatus::Failed);
        json[QStringLiteral("lastMessage")]
            = QStringLiteral("Interrupted: the application closed while it was running");
        json[QStringLiteral("consecutiveFailures")] = consecutiveFailures + 1;
        return json;
    }
    json[QStringLiteral("lastStatus")] = runStatusToString(lastStatus);
    json[QStringLiteral("lastMessage")] = lastMessage;
    json[QStringLiteral("consecutiveFailures")] = consecutiveFailures;
    return json;
}

ScheduleRule ScheduleRule::fromJson(const QJsonObject& json)
{
    ScheduleRule rule;
    rule.id = json.value(QStringLiteral("id")).toString();
    rule.jobKind = json.value(QStringLiteral("jobKind")).toString();
    rule.label = json.value(QStringLiteral("label")).toString();
    rule.parameters = json.value(QStringLiteral("parameters")).toObject().toVariantMap();
    rule.intervalSeconds = static_cast<qint64>(json.value(QStringLiteral("intervalSeconds")).toDouble(kDay));
    rule.clampInterval(); // a hand-edited 0 would spin the scheduler
    rule.enabled = json.value(QStringLiteral("enabled")).toBool(true);
    rule.lastRunAt = QDateTime::fromString(json.value(QStringLiteral("lastRunAt")).toString(), Qt::ISODate);
    rule.lastSuccessAt
        = QDateTime::fromString(json.value(QStringLiteral("lastSuccessAt")).toString(), Qt::ISODate);
    rule.lastStatus = runStatusFromString(json.value(QStringLiteral("lastStatus")).toString());
    rule.lastMessage = json.value(QStringLiteral("lastMessage")).toString();
    rule.consecutiveFailures = json.value(QStringLiteral("consecutiveFailures")).toInt();
    if (rule.lastStatus == RunStatus::Running) {
        // A file that says "running" -- hand-edited, or written by a version
        // before toJson() converted it. The same answer as there, for the same
        // reason: it did not succeed, and a failure has to say why.
        rule.lastStatus = RunStatus::Failed;
        if (rule.lastMessage.isEmpty()) {
            rule.lastMessage = QStringLiteral("Interrupted: the application closed while it was running");
            ++rule.consecutiveFailures;
        }
    }
    return rule;
}

QList<QPair<QString, qint64>> ScheduleRule::presets()
{
    return {
        { QStringLiteral("Every hour"), kHour },
        { QStringLiteral("Every 6 hours"), 6 * kHour },
        { QStringLiteral("Every day"), kDay },
        { QStringLiteral("Every week"), 7 * kDay },
        { QStringLiteral("Every month"), 30 * kDay },
    };
}

QString ScheduleRule::describeInterval(qint64 seconds)
{
    const auto plural = [](qint64 count, const QString& unit) {
        return count == 1 ? QStringLiteral("Every %1").arg(unit)
                          : QStringLiteral("Every %1 %2s").arg(count).arg(unit);
    };

    if (seconds % (30 * kDay) == 0)
        return plural(seconds / (30 * kDay), QStringLiteral("month"));
    if (seconds % (7 * kDay) == 0)
        return plural(seconds / (7 * kDay), QStringLiteral("week"));
    if (seconds % kDay == 0)
        return plural(seconds / kDay, QStringLiteral("day"));
    if (seconds % kHour == 0)
        return plural(seconds / kHour, QStringLiteral("hour"));
    return plural(std::max<qint64>(1, seconds / 60), QStringLiteral("minute"));
}

qint64 RunRecord::durationMs() const
{
    if (!startedAt.isValid() || !finishedAt.isValid())
        return 0;
    return startedAt.msecsTo(finishedAt);
}

QJsonObject RunRecord::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("ruleId")] = ruleId;
    json[QStringLiteral("ruleLabel")] = ruleLabel;
    json[QStringLiteral("startedAt")] = startedAt.toUTC().toString(Qt::ISODateWithMs);
    json[QStringLiteral("finishedAt")] = finishedAt.toUTC().toString(Qt::ISODateWithMs);
    json[QStringLiteral("status")] = runStatusToString(status);
    json[QStringLiteral("message")] = message;
    return json;
}

RunRecord RunRecord::fromJson(const QJsonObject& json)
{
    RunRecord record;
    record.ruleId = json.value(QStringLiteral("ruleId")).toString();
    record.ruleLabel = json.value(QStringLiteral("ruleLabel")).toString();
    record.startedAt
        = QDateTime::fromString(json.value(QStringLiteral("startedAt")).toString(), Qt::ISODateWithMs);
    record.finishedAt
        = QDateTime::fromString(json.value(QStringLiteral("finishedAt")).toString(), Qt::ISODateWithMs);
    record.status = runStatusFromString(json.value(QStringLiteral("status")).toString());
    record.message = json.value(QStringLiteral("message")).toString();
    return record;
}

} // namespace mole
