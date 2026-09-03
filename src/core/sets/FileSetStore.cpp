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
    : JsonFileStore(std::move(path), parent)
{
}

QString FileSetStore::defaultPath()
{
    return pathFor("MOLE_SETS_PATH", QStringLiteral("sets.json"));
}

bool FileSetStore::load()
{
    QJsonObject root;
    const Read read = readRoot(&root);
    if (read == Read::Damaged)
        return false; // kept, and nothing is written over it until somebody says

    m_sets.clear();
    if (read == Read::Missing) {
        emit setsChanged();
        return true; // nothing saved yet is the ordinary first run
    }

    const QJsonArray sets = root.value(QStringLiteral("sets")).toArray();
    for (const QJsonValue& value : sets) {
        const FileSet set = FileSet::fromJson(value.toObject());
        if (set.isValid())
            m_sets.append(set); // a malformed entry is dropped, not fatal
    }

    emit setsChanged();
    return true;
}

bool FileSetStore::save()
{
    QJsonArray sets;
    for (const FileSet& set : m_sets)
        sets.append(set.toJson());

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("sets")] = sets;

    return writeRoot(root);
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
    // The set exists whether or not the file took it; a caller that needs to
    // know goes through put(), and the failure is reported once by
    // JsonFileStore::saveFailed() either way. See ADR-0089.
    (void)save();
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
        const bool written = save();
        emit setsChanged();
        return written;
    }

    m_sets.append(set);
    const bool written = save();
    emit setsChanged();
    return written;
}

bool FileSetStore::remove(const QString& id)
{
    const auto position
        = std::find_if(m_sets.begin(), m_sets.end(), [&id](const FileSet& set) { return set.id == id; });
    if (position == m_sets.end())
        return false;

    m_sets.erase(position);
    const bool written = save();
    emit setsChanged();
    return written;
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
        (void)put(target);
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
        (void)put(target);
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
