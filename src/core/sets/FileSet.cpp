#include "core/sets/FileSet.h"

#include <QJsonArray>
#include <QSet>

namespace mole {

QList<VfsUri> FileSet::targets() const
{
    QList<VfsUri> out;
    out.reserve(uris.size());
    for (const QString& uri : uris) {
        const VfsUri parsed = VfsUri::fromString(uri);
        // A member that no longer parses is skipped rather than handed on: an
        // operation should not have to defend against nonsense from a store.
        if (parsed.isValid())
            out.append(parsed);
    }
    return out;
}

int FileSet::driveCount() const
{
    QSet<QString> drives;
    for (const QString& uri : uris) {
        const VfsUri parsed = VfsUri::fromString(uri);
        if (parsed.isValid())
            drives.insert(parsed.scheme() + QLatin1Char('/') + parsed.authority());
    }
    return static_cast<int>(drives.size());
}

QJsonObject FileSet::toJson() const
{
    QJsonArray members;
    for (const QString& uri : uris)
        members.append(uri);

    QJsonObject json;
    json[QStringLiteral("id")] = id;
    json[QStringLiteral("name")] = name;
    json[QStringLiteral("note")] = note;
    json[QStringLiteral("createdAt")] = createdAt.toUTC().toString(Qt::ISODate);
    json[QStringLiteral("updatedAt")] = updatedAt.toUTC().toString(Qt::ISODate);
    json[QStringLiteral("uris")] = members;
    return json;
}

FileSet FileSet::fromJson(const QJsonObject& json)
{
    FileSet set;
    set.id = json.value(QStringLiteral("id")).toString();
    set.name = json.value(QStringLiteral("name")).toString();
    set.note = json.value(QStringLiteral("note")).toString();
    set.createdAt = QDateTime::fromString(json.value(QStringLiteral("createdAt")).toString(), Qt::ISODate);
    set.updatedAt = QDateTime::fromString(json.value(QStringLiteral("updatedAt")).toString(), Qt::ISODate);

    const QJsonArray members = json.value(QStringLiteral("uris")).toArray();
    for (const QJsonValue& value : members) {
        const QString uri = value.toString();
        // Duplicates dropped on the way in: a set is a set, and adding the same
        // file twice would make every operation act on it twice.
        if (!uri.isEmpty() && !set.uris.contains(uri))
            set.uris.append(uri);
    }
    return set;
}

} // namespace mole
