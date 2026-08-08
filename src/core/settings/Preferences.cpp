#include "core/settings/Preferences.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace mole {

QString Preferences::defaultPath()
{
    const QByteArray override = qgetenv("MOLE_PREFERENCES_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("preferences.json"));
}

Preferences::Preferences(QString path, QObject* parent)
    : QObject(parent)
    , m_path(std::move(path))
{
    load();
}

bool Preferences::load()
{
    m_values.clear();

    QFile file(m_path);
    // Nothing remembered yet is the normal state on a first run, not a failure.
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;

    m_values = document.object().toVariantMap();
    return true;
}

bool Preferences::save() const
{
    QDir().mkpath(QFileInfo(m_path).absolutePath());

    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    // Indented: it is a handful of keys and someone will read it with an editor.
    const QJsonDocument document(QJsonObject::fromVariantMap(m_values));
    return file.write(document.toJson(QJsonDocument::Indented)) > 0;
}

QVariant Preferences::value(const QString& key, const QVariant& fallback) const
{
    return m_values.value(key, fallback);
}

void Preferences::setValue(const QString& key, const QVariant& value)
{
    if (m_values.value(key) == value)
        return;

    m_values.insert(key, value);
    save();
    emit changed(key);
}

bool Preferences::contains(const QString& key) const
{
    return m_values.contains(key);
}

void Preferences::remove(const QString& key)
{
    if (m_values.remove(key) == 0)
        return;
    save();
    emit changed(key);
}

} // namespace mole
