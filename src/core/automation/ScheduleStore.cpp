#include "core/automation/ScheduleStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace mole {

ScheduleStore::ScheduleStore(QString path, QObject* parent)
    : QObject(parent)
    , m_path(std::move(path))
{
}

QString ScheduleStore::defaultPath()
{
    const QByteArray override = qgetenv("MOLE_SCHEDULE_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("schedule.json"));
}

bool ScheduleStore::load()
{
    m_rules.clear();
    m_history.clear();

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return false; // no schedule yet is not an error

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;

    const QJsonObject root = document.object();
    const QJsonArray rules = root.value(QStringLiteral("rules")).toArray();
    for (const QJsonValue& value : rules) {
        const ScheduleRule rule = ScheduleRule::fromJson(value.toObject());
        if (rule.isValid())
            m_rules.append(rule); // a malformed entry is dropped, not fatal
    }

    const QJsonArray history = root.value(QStringLiteral("history")).toArray();
    for (const QJsonValue& value : history)
        m_history.append(RunRecord::fromJson(value.toObject()));

    trimHistory();
    emit rulesChanged();
    emit historyChanged();
    return true;
}

bool ScheduleStore::save() const
{
    const QFileInfo info(m_path);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return false;

    QJsonArray rules;
    for (const ScheduleRule& rule : m_rules)
        rules.append(rule.toJson());

    QJsonArray history;
    for (const RunRecord& record : m_history)
        history.append(record.toJson());

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("rules")] = rules;
    root[QStringLiteral("history")] = history;

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

ScheduleRule ScheduleStore::rule(const QString& id) const
{
    for (const ScheduleRule& rule : m_rules) {
        if (rule.id == id)
            return rule;
    }
    return {};
}

bool ScheduleStore::put(const ScheduleRule& rule)
{
    if (!rule.isValid())
        return false;

    for (int i = 0; i < m_rules.size(); ++i) {
        if (m_rules.at(i).id == rule.id) {
            m_rules[i] = rule;
            save();
            emit rulesChanged();
            return true;
        }
    }

    m_rules.append(rule);
    save();
    emit rulesChanged();
    return true;
}

bool ScheduleStore::remove(const QString& id)
{
    const auto position = std::find_if(
        m_rules.begin(), m_rules.end(), [&id](const ScheduleRule& rule) { return rule.id == id; });
    if (position == m_rules.end())
        return false;

    m_rules.erase(position);
    save();
    emit rulesChanged();
    return true;
}

QList<RunRecord> ScheduleStore::history(const QString& ruleId, int limit) const
{
    QList<RunRecord> out;
    for (int i = m_history.size() - 1; i >= 0 && out.size() < limit; --i) {
        const RunRecord& record = m_history.at(i);
        if (ruleId.isEmpty() || record.ruleId == ruleId)
            out.append(record);
    }
    return out;
}

void ScheduleStore::record(const RunRecord& record)
{
    m_history.append(record);
    trimHistory();
    save();
    emit historyChanged();
}

void ScheduleStore::clearHistory()
{
    if (m_history.isEmpty())
        return;
    m_history.clear();
    save();
    emit historyChanged();
}

void ScheduleStore::trimHistory()
{
    while (m_history.size() > m_historyLimit)
        m_history.removeFirst();
}

} // namespace mole
