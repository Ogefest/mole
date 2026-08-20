#include "ui/models/BookmarkModel.h"

#include "core/sets/FileSetStore.h"
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
namespace {

    // The on-disk vocabulary. A folder row carries no `kind` at all, so a file
    // written before ADR-0061 loads unchanged -- and a set's id goes under a key an
    // older Mole does not read, so it skips the row rather than treating an id as a
    // path.
    constexpr auto kKindKey = "kind";
    constexpr auto kSetKind = "set";
    constexpr auto kFolderKind = "folder";
    constexpr auto kUriKey = "uri";
    constexpr auto kSetIdKey = "setId";
    constexpr auto kNameKey = "name";

} // namespace

QString BookmarkModel::defaultFilePath()
{
    const QByteArray override = qgetenv("MOLE_BOOKMARKS_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("bookmarks.json"));
}

BookmarkModel::BookmarkModel(QString filePath, FileSetStore* sets, QObject* parent)
    : QAbstractListModel(parent)
    , m_filePath(std::move(filePath))
    , m_sets(sets)
{
    load();
    if (m_sets)
        connect(m_sets, &FileSetStore::setsChanged, this, &BookmarkModel::setsChanged);
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

QString BookmarkModel::displayName(const Bookmark& bookmark) const
{
    if (bookmark.kind != Bookmark::Kind::Set || !m_sets)
        return bookmark.name;

    // The store is the authority while the set exists, so a rename in the Sets
    // tab shows up here with nothing copied and nothing to keep in step.
    const FileSet set = m_sets->set(bookmark.target);
    return set.isValid() ? set.name : bookmark.name;
}

bool BookmarkModel::isDead(const Bookmark& bookmark) const
{
    if (bookmark.kind != Bookmark::Kind::Set)
        return false;
    return !m_sets || !m_sets->set(bookmark.target).isValid();
}

void BookmarkModel::setsChanged()
{
    bool renamed = false;
    for (int row = 0; row < m_bookmarks.size(); ++row) {
        Bookmark& bookmark = m_bookmarks[row];
        if (bookmark.kind != Bookmark::Kind::Set)
            continue;

        // The name in the file is the last one seen, so a set that has since been
        // deleted still has something to be called.
        const FileSet set = m_sets ? m_sets->set(bookmark.target) : FileSet {};
        if (set.isValid() && set.name != bookmark.name) {
            bookmark.name = set.name;
            renamed = true;
        }
        const QModelIndex at = index(row, 0);
        emit dataChanged(at, at, { NameRole, Qt::DisplayRole, DeadRole });
    }
    if (renamed)
        save();
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
        return displayName(bookmark);
    case UriRole:
        return bookmark.kind == Bookmark::Kind::Folder ? bookmark.target : QString();
    case KindRole:
        return bookmark.kind == Bookmark::Kind::Set ? QLatin1String(kSetKind) : QLatin1String(kFolderKind);
    case TargetRole:
        return bookmark.target;
    case DeadRole:
        return isDead(bookmark);
    default:
        return {};
    }
}

QHash<int, QByteArray> BookmarkModel::roleNames() const
{
    return { { NameRole, "name" }, { UriRole, "uri" }, { KindRole, "kind" }, { TargetRole, "target" },
        { DeadRole, "dead" } };
}

bool BookmarkModel::contains(Bookmark::Kind kind, const QString& target) const
{
    return std::any_of(m_bookmarks.begin(), m_bookmarks.end(), [kind, &target](const Bookmark& bookmark) {
        return bookmark.kind == kind && bookmark.target == target;
    });
}

bool BookmarkModel::contains(const QString& uri) const
{
    return contains(Bookmark::Kind::Folder, uri);
}

bool BookmarkModel::containsSet(const QString& setId) const
{
    return contains(Bookmark::Kind::Set, setId);
}

bool BookmarkModel::add(Bookmark bookmark)
{
    if (bookmark.target.isEmpty() || contains(bookmark.kind, bookmark.target))
        return false;

    const int row = static_cast<int>(m_bookmarks.size());
    beginInsertRows({}, row, row);
    m_bookmarks.append(std::move(bookmark));
    endInsertRows();

    emit countChanged();
    save();
    return true;
}

bool BookmarkModel::add(const QString& uri, const QString& name)
{
    return add(Bookmark { Bookmark::Kind::Folder, name.isEmpty() ? defaultNameFor(uri) : name, uri });
}

bool BookmarkModel::addSet(const QString& setId)
{
    // The name is the set's, not the caller's: there is one place a set is
    // named, and a copy taken here would be the second.
    const FileSet set = m_sets ? m_sets->set(setId) : FileSet {};
    return add(Bookmark { Bookmark::Kind::Set, set.isValid() ? set.name : setId, setId });
}

bool BookmarkModel::remove(Bookmark::Kind kind, const QString& target)
{
    for (int row = 0; row < m_bookmarks.size(); ++row) {
        const Bookmark& bookmark = m_bookmarks.at(row);
        if (bookmark.kind == kind && bookmark.target == target) {
            removeAt(row);
            return true;
        }
    }
    return false;
}

bool BookmarkModel::removeUri(const QString& uri)
{
    return remove(Bookmark::Kind::Folder, uri);
}

bool BookmarkModel::removeSet(const QString& setId)
{
    return remove(Bookmark::Kind::Set, setId);
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
    const Bookmark& bookmark = m_bookmarks.at(row);
    return bookmark.kind == Bookmark::Kind::Folder ? bookmark.target : QString();
}

QString BookmarkModel::targetAt(int row) const
{
    if (row < 0 || row >= m_bookmarks.size())
        return {};
    return m_bookmarks.at(row).target;
}

QString BookmarkModel::kindAt(int row) const
{
    if (row < 0 || row >= m_bookmarks.size())
        return {};
    return m_bookmarks.at(row).kind == Bookmark::Kind::Set ? QLatin1String(kSetKind)
                                                           : QLatin1String(kFolderKind);
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
        const QString kind = entry.value(QLatin1String(kKindKey)).toString();

        Bookmark bookmark;
        if (kind.isEmpty() || kind == QLatin1String(kFolderKind)) {
            bookmark.kind = Bookmark::Kind::Folder;
            bookmark.target = entry.value(QLatin1String(kUriKey)).toString();
        } else if (kind == QLatin1String(kSetKind)) {
            bookmark.kind = Bookmark::Kind::Set;
            bookmark.target = entry.value(QLatin1String(kSetIdKey)).toString();
        } else {
            // Written by a newer Mole. Skipped rather than guessed at, which is
            // the whole reason the kind is on disk. See ADR-0061.
            continue;
        }
        if (bookmark.target.isEmpty())
            continue;

        bookmark.name = entry.value(QLatin1String(kNameKey)).toString();
        if (bookmark.name.isEmpty()) {
            bookmark.name
                = bookmark.kind == Bookmark::Kind::Folder ? defaultNameFor(bookmark.target) : bookmark.target;
        }
        m_bookmarks.append(std::move(bookmark));
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
        // A folder row is written exactly as it always was: no `kind`, and the
        // uri under `uri`. A set row carries its kind and puts the id under a key
        // that is not `uri`, so nothing can read it as a place.
        if (bookmark.kind == Bookmark::Kind::Set) {
            entry[QLatin1String(kKindKey)] = QLatin1String(kSetKind);
            entry[QLatin1String(kNameKey)] = bookmark.name;
            entry[QLatin1String(kSetIdKey)] = bookmark.target;
        } else {
            entry[QLatin1String(kNameKey)] = bookmark.name;
            entry[QLatin1String(kUriKey)] = bookmark.target;
        }
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
