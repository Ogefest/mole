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
    : JsonFileStore(std::move(path), parent)
{
}

QString ScheduleStore::defaultPath()
{
    return pathFor("MOLE_SCHEDULE_PATH", QStringLiteral("schedule.json"));
}

bool ScheduleStore::load()
{
    QJsonObject root;
    const Read read = readRoot(&root);
    if (read == Read::Damaged)
        return false; // kept, and nothing is written over it until somebody says

    m_rules.clear();
    m_history.clear();
    if (read == Read::Missing) {
        emit rulesChanged();
        emit historyChanged();
        return true; // nothing saved yet is the ordinary first run
    }

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

bool ScheduleStore::save()
{
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

    return writeRoot(root);
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
            const bool written = save();
            emit rulesChanged();
            return written;
        }
    }

    m_rules.append(rule);
    const bool written = save();
    emit rulesChanged();
    return written;
}

bool ScheduleStore::remove(const QString& id)
{
    const auto position = std::find_if(
        m_rules.begin(), m_rules.end(), [&id](const ScheduleRule& rule) { return rule.id == id; });
    if (position == m_rules.end())
        return false;

    m_rules.erase(position);
    const bool written = save();
    emit rulesChanged();
    return written;
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

bool ScheduleStore::record(const RunRecord& record)
{
    m_history.append(record);
    trimHistory();
    const bool written = save();
    emit historyChanged();
    return written;
}

bool ScheduleStore::clearHistory()
{
    if (m_history.isEmpty())
        return true;
    m_history.clear();
    const bool written = save();
    emit historyChanged();
    return written;
}

void ScheduleStore::trimHistory()
{
    while (m_history.size() > m_historyLimit)
        m_history.removeFirst();
}

} // namespace mole
