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
    : QObject(parent)
    , m_path(std::move(path))
{
}

QString AlertStore::defaultPath()
{
    const QByteArray override = qgetenv("MOLE_ALERTS_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("alerts.json"));
}

bool AlertStore::load()
{
    m_rules.clear();
    m_history.clear();

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return false; // no alerts yet is not an error

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;

    const QJsonObject root = document.object();
    const QJsonArray rules = root.value(QStringLiteral("rules")).toArray();
    for (const QJsonValue& value : rules) {
        const AlertRule rule = AlertRule::fromJson(value.toObject());
        if (rule.isValid())
            m_rules.append(rule);
    }

    const QJsonArray history = root.value(QStringLiteral("history")).toArray();
    for (const QJsonValue& value : history)
        m_history.append(AlertEvent::fromJson(value.toObject()));

    trimHistory();
    emit rulesChanged();
    emit historyChanged();
    return true;
}

bool AlertStore::save() const
{
    const QFileInfo info(m_path);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return false;

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

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
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
        save();
        emit rulesChanged();

        if (before != AlertState::Triggered && rule.state == AlertState::Triggered)
            emit alertRaised(rule);
        else if (before == AlertState::Triggered && rule.state == AlertState::Ok)
            emit alertCleared(rule);
        return true;
    }

    m_rules.append(rule);
    save();
    emit rulesChanged();
    if (rule.state == AlertState::Triggered)
        emit alertRaised(rule);
    return true;
}

bool AlertStore::remove(const QString& id)
{
    const auto position = std::find_if(
        m_rules.begin(), m_rules.end(), [&id](const AlertRule& rule) { return rule.id == id; });
    if (position == m_rules.end())
        return false;

    m_rules.erase(position);
    save();
    emit rulesChanged();
    return true;
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

void AlertStore::record(const AlertEvent& event)
{
    m_history.append(event);
    trimHistory();
    save();
    emit historyChanged();
}

void AlertStore::clearHistory()
{
    if (m_history.isEmpty())
        return;
    m_history.clear();
    save();
    emit historyChanged();
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
