#include "ui/models/BookmarkModel.h"

#include "core/vfs/VfsUri.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace mole {

QString BookmarkModel::defaultFilePath()
{
    const QByteArray override = qgetenv("MOLE_BOOKMARKS_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("bookmarks.json"));
}

BookmarkModel::BookmarkModel(QString filePath, QObject* parent)
    : QAbstractListModel(parent)
    , m_filePath(std::move(filePath))
{
    load();
}

QString BookmarkModel::defaultNameFor(const QString& uri)
{
    const VfsUri parsed = VfsUri::fromString(uri);
    if (!parsed.isValid())
        return uri;

    const QString name = parsed.fileName();
    if (!name.isEmpty())
        return name;
    // The root of a drive has no file name of its own.
    return parsed.authority().isEmpty() ? parsed.scheme() : parsed.authority();
}

int BookmarkModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_bookmarks.size());
}

QVariant BookmarkModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_bookmarks.size())
        return {};

    const Bookmark& bookmark = m_bookmarks.at(index.row());
    switch (role) {
    case NameRole:
    case Qt::DisplayRole:
        return bookmark.name;
    case UriRole:
        return bookmark.uri;
    default:
        return {};
    }
}

QHash<int, QByteArray> BookmarkModel::roleNames() const
{
    return { { NameRole, "name" }, { UriRole, "uri" } };
}

bool BookmarkModel::contains(const QString& uri) const
{
    return std::any_of(m_bookmarks.begin(), m_bookmarks.end(),
        [&uri](const Bookmark& bookmark) { return bookmark.uri == uri; });
}

bool BookmarkModel::add(const QString& uri, const QString& name)
{
    if (uri.isEmpty() || contains(uri))
        return false;

    const int row = static_cast<int>(m_bookmarks.size());
    beginInsertRows({}, row, row);
    m_bookmarks.append(Bookmark { name.isEmpty() ? defaultNameFor(uri) : name, uri });
    endInsertRows();

    emit countChanged();
    save();
    return true;
}

bool BookmarkModel::removeUri(const QString& uri)
{
    for (int row = 0; row < m_bookmarks.size(); ++row) {
        if (m_bookmarks.at(row).uri == uri) {
            removeAt(row);
            return true;
        }
    }
    return false;
}

void BookmarkModel::removeAt(int row)
{
    if (row < 0 || row >= m_bookmarks.size())
        return;

    beginRemoveRows({}, row, row);
    m_bookmarks.removeAt(row);
    endRemoveRows();

    emit countChanged();
    save();
}

bool BookmarkModel::rename(int row, const QString& name)
{
    if (row < 0 || row >= m_bookmarks.size() || name.trimmed().isEmpty())
        return false;

    m_bookmarks[row].name = name.trimmed();
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, { NameRole, Qt::DisplayRole });
    save();
    return true;
}

QString BookmarkModel::uriAt(int row) const
{
    if (row < 0 || row >= m_bookmarks.size())
        return {};
    return m_bookmarks.at(row).uri;
}

bool BookmarkModel::load()
{
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
        return false;

    beginResetModel();
    m_bookmarks.clear();
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject())
            continue;
        const QJsonObject entry = value.toObject();
        const QString uri = entry.value(QStringLiteral("uri")).toString();
        if (uri.isEmpty())
            continue;
        QString name = entry.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            name = defaultNameFor(uri);
        m_bookmarks.append(Bookmark { name, uri });
    }
    endResetModel();

    emit countChanged();
    return true;
}

bool BookmarkModel::save() const
{
    const QFileInfo info(m_filePath);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return false;

    QJsonArray array;
    for (const Bookmark& bookmark : m_bookmarks) {
        QJsonObject entry;
        entry[QStringLiteral("name")] = bookmark.name;
        entry[QStringLiteral("uri")] = bookmark.uri;
        array.append(entry);
    }

    // Same reasoning as the session file: a crash mid-write must not cost the
    // user their saved places.
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace mole
