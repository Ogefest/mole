#include "core/sets/FileSetStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace mole {

FileSetStore::FileSetStore(QString path, QObject* parent)
    : QObject(parent)
    , m_path(std::move(path))
{
}

QString FileSetStore::defaultPath()
{
    const QByteArray override = qgetenv("MOLE_SETS_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("sets.json"));
}

bool FileSetStore::load()
{
    m_sets.clear();

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return false; // no sets yet is not an error

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;

    const QJsonArray sets = document.object().value(QStringLiteral("sets")).toArray();
    for (const QJsonValue& value : sets) {
        const FileSet set = FileSet::fromJson(value.toObject());
        if (set.isValid())
            m_sets.append(set); // a malformed entry is dropped, not fatal
    }

    emit setsChanged();
    return true;
}

bool FileSetStore::save() const
{
    const QFileInfo info(m_path);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return false;

    QJsonArray sets;
    for (const FileSet& set : m_sets)
        sets.append(set.toJson());

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("sets")] = sets;

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

FileSet FileSetStore::set(const QString& id) const
{
    for (const FileSet& set : m_sets) {
        if (set.id == id)
            return set;
    }
    return {};
}

FileSet FileSetStore::create(const QString& name, const QList<QString>& uris)
{
    FileSet set;
    if (name.trimmed().isEmpty())
        return set;

    set.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    set.name = name.trimmed();
    set.createdAt = QDateTime::currentDateTime();
    set.updatedAt = set.createdAt;
    for (const QString& uri : uris) {
        if (!uri.isEmpty() && !set.uris.contains(uri))
            set.uris.append(uri);
    }

    m_sets.append(set);
    save();
    emit setsChanged();
    return set;
}

bool FileSetStore::put(const FileSet& set)
{
    if (!set.isValid())
        return false;

    for (int i = 0; i < m_sets.size(); ++i) {
        if (m_sets.at(i).id != set.id)
            continue;
        m_sets[i] = set;
        m_sets[i].updatedAt = QDateTime::currentDateTime();
        save();
        emit setsChanged();
        return true;
    }

    m_sets.append(set);
    save();
    emit setsChanged();
    return true;
}

bool FileSetStore::remove(const QString& id)
{
    const auto position
        = std::find_if(m_sets.begin(), m_sets.end(), [&id](const FileSet& set) { return set.id == id; });
    if (position == m_sets.end())
        return false;

    m_sets.erase(position);
    save();
    emit setsChanged();
    return true;
}

bool FileSetStore::rename(const QString& id, const QString& name)
{
    if (name.trimmed().isEmpty())
        return false;
    FileSet target = set(id);
    if (!target.isValid())
        return false;
    target.name = name.trimmed();
    return put(target);
}

int FileSetStore::addTo(const QString& id, const QList<QString>& uris)
{
    FileSet target = set(id);
    if (!target.isValid())
        return 0;

    int added = 0;
    for (const QString& uri : uris) {
        // Already-present members are skipped rather than duplicated: a set is
        // a set, and a duplicate would make every operation act on it twice.
        if (uri.isEmpty() || target.uris.contains(uri))
            continue;
        target.uris.append(uri);
        ++added;
    }

    if (added > 0)
        put(target);
    return added;
}

int FileSetStore::removeFrom(const QString& id, const QList<QString>& uris)
{
    FileSet target = set(id);
    if (!target.isValid())
        return 0;

    int removed = 0;
    for (const QString& uri : uris)
        removed += static_cast<int>(target.uris.removeAll(uri));

    if (removed > 0)
        put(target);
    return removed;
}

QStringList FileSetStore::setsContaining(const QString& uri) const
{
    QStringList names;
    for (const FileSet& set : m_sets) {
        if (set.contains(uri))
            names.append(set.name);
    }
    return names;
}

} // namespace mole
