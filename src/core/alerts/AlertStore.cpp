#include "core/alerts/AlertStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace mole {

AlertStore::AlertStore(QString path, QObject* parent)
    : JsonFileStore(std::move(path), parent)
{
}

QString AlertStore::defaultPath()
{
    return pathFor("MOLE_ALERTS_PATH", QStringLiteral("alerts.json"));
}

bool AlertStore::load()
{
    QJsonObject root;
    const Read read = readRoot(&root);
    if (read == Read::Damaged)
        return false; // kept, and nothing is written over it until somebody says

    m_rules.clear();
    m_history.clear();
    m_unreadable = 0;
    if (read == Read::Missing) {
        emit rulesChanged();
        emit historyChanged();
        return true; // nothing saved yet is the ordinary first run
    }

    const QJsonArray rules = root.value(QStringLiteral("rules")).toArray();
    for (const QJsonValue& value : rules) {
        const std::optional<AlertRule> rule = AlertRule::fromJson(value.toObject());
        if (!rule || !rule->isValid()) {
            ++m_unreadable;
            continue;
        }
        m_rules.append(*rule);
    }

    const QJsonArray history = root.value(QStringLiteral("history")).toArray();
    for (const QJsonValue& value : history)
        m_history.append(AlertEvent::fromJson(value.toObject()));

    trimHistory();
    emit rulesChanged();
    emit historyChanged();
    return true;
}

bool AlertStore::save()
{
    QJsonArray rules;
    for (const AlertRule& rule : m_rules)
        rules.append(rule.toJson());

    QJsonArray history;
    for (const AlertEvent& event : m_history)
        history.append(event.toJson());

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("rules")] = rules;
    root[QStringLiteral("history")] = history;

    return writeRoot(root);
}

AlertRule AlertStore::rule(const QString& id) const
{
    for (const AlertRule& rule : m_rules) {
        if (rule.id == id)
            return rule;
    }
    return {};
}

bool AlertStore::put(const AlertRule& rule)
{
    if (!rule.isValid())
        return false;

    for (int i = 0; i < m_rules.size(); ++i) {
        if (m_rules.at(i).id != rule.id)
            continue;

        // Only a transition is worth announcing. Re-announcing a rule that was
        // already triggered on every check would train the user to ignore it.
        const AlertState before = m_rules.at(i).state;
        m_rules[i] = rule;
        const bool written = save();
        emit rulesChanged();

        if (before != AlertState::Triggered && rule.state == AlertState::Triggered)
            emit alertRaised(rule);
        else if (before == AlertState::Triggered && rule.state == AlertState::Ok)
            emit alertCleared(rule);
        return written;
    }

    m_rules.append(rule);
    const bool written = save();
    emit rulesChanged();
    if (rule.state == AlertState::Triggered)
        emit alertRaised(rule);
    return written;
}

bool AlertStore::remove(const QString& id)
{
    const auto position = std::find_if(
        m_rules.begin(), m_rules.end(), [&id](const AlertRule& rule) { return rule.id == id; });
    if (position == m_rules.end())
        return false;

    m_rules.erase(position);
    const bool written = save();
    emit rulesChanged();
    return written;
}

QList<AlertEvent> AlertStore::history(const QString& ruleId, int limit) const
{
    QList<AlertEvent> out;
    for (int i = m_history.size() - 1; i >= 0 && out.size() < limit; --i) {
        const AlertEvent& event = m_history.at(i);
        if (ruleId.isEmpty() || event.ruleId == ruleId)
            out.append(event);
    }
    return out;
}

bool AlertStore::record(const AlertEvent& event)
{
    m_history.append(event);
    trimHistory();
    const bool written = save();
    emit historyChanged();
    return written;
}

bool AlertStore::clearHistory()
{
    if (m_history.isEmpty())
        return true;
    m_history.clear();
    const bool written = save();
    emit historyChanged();
    return written;
}

int AlertStore::triggeredCount() const
{
    int count = 0;
    for (const AlertRule& rule : m_rules) {
        if (rule.enabled && rule.state == AlertState::Triggered)
            ++count;
    }
    return count;
}

void AlertStore::trimHistory()
{
    while (m_history.size() > m_historyLimit)
        m_history.removeFirst();
}

} // namespace mole
