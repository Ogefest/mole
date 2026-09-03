#include "core/settings/Preferences.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace mole {

QString Preferences::defaultPath()
{
    return pathFor("MOLE_PREFERENCES_PATH", QStringLiteral("preferences.json"));
}

Preferences::Preferences(QString path, QObject* parent)
    : JsonFileStore(std::move(path), parent)
{
    load();
}

bool Preferences::load()
{
    QJsonObject root;
    const Read read = readRoot(&root);
    if (read == Read::Damaged)
        return false; // kept, and nothing is written over it until somebody says

    // Nothing remembered yet is the normal state on a first run, not a failure.
    m_values = read == Read::Missing ? QVariantMap() : root.toVariantMap();
    return true;
}

bool Preferences::save()
{
    // Written whole -- the file is replaced whenever anything changes -- so an
    // interruption part way through the write is an interruption part way
    // through the only copy. writeRoot() goes through QSaveFile for that: the
    // file is either the settings as they were or as they now are, and never a
    // moment of neither. This one used to open the real file with Truncate, and
    // a process killed at the wrong instant left an empty file where every
    // setting anybody had ever chosen used to be.
    return writeRoot(QJsonObject::fromVariantMap(m_values));
}

QVariant Preferences::value(const QString& key, const QVariant& fallback) const
{
    return m_values.value(key, fallback);
}

bool Preferences::setValue(const QString& key, const QVariant& value)
{
    if (m_values.value(key) == value)
        return true;

    m_values.insert(key, value);
    const bool written = save();
    emit changed(key);
    return written;
}

bool Preferences::contains(const QString& key) const
{
    return m_values.contains(key);
}

bool Preferences::remove(const QString& key)
{
    if (m_values.remove(key) == 0)
        return true;
    const bool written = save();
    emit changed(key);
    return written;
}

} // namespace mole
