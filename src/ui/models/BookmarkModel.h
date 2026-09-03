#pragma once

#include "core/data/JsonFileStore.h"

#include <QAbstractListModel>
#include <QString>

namespace mole {

class FileSetStore;

/// One saved place.
///
/// `kind` says how to read `target`: a uri for a folder, a set's id for a set.
/// Two fields rather than one uri with a `set:` scheme in it -- a drive's scheme
/// is derived from its name and could collide, and VfsUri would have to accept
/// something that is not a location. See ADR-0061.
struct Bookmark
{
    enum class Kind {
        /// Anywhere a uri can point: a folder on disk, a path inside a mounted
        /// archive, a directory on a network share.
        Folder,
        /// A named list of files, by id. Its name and whether it still exists
        /// are read from the store rather than copied here.
        Set,
    };

    Kind kind = Kind::Folder;
    /// For a set, the last name the store was seen to give it. Shown only once
    /// the set has gone: a gravestone, not a cache.
    QString name;
    QString target;
};

/// The user's saved places.
///
/// Separate from the drive list on purpose: drives are what the machine has,
/// bookmarks are what the person cares about.
class BookmarkModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        /// The uri for a folder row, and **empty for a set row**. A consumer
        /// that only understands places gets nothing rather than an id it would
        /// try to parse as a path.
        UriRole,
        /// "folder" or "set".
        KindRole,
        /// Whatever the row points at: a uri, or a set's id.
        TargetRole,
        /// A set bookmark whose set has been deleted. Kept and marked rather
        /// than dropped -- deciding it has stopped being useful belongs to the
        /// person who made it.
        DeadRole,
    };

    /// `MOLE_BOOKMARKS_PATH` wins, so tests never touch the user's real list.
    static QString defaultFilePath();

    /// `sets` may be null, in which case every set bookmark reads as dead --
    /// which is what a caller that only deals in folders wants.
    explicit BookmarkModel(QString filePath, FileSetStore* sets = nullptr, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Bookmarks a folder. Ignores a uri that is already bookmarked, so adding
    /// twice is harmless rather than producing a duplicate row.
    Q_INVOKABLE bool add(const QString& uri, const QString& name = {});
    /// Bookmarks a set, by id. Its name comes from the store and follows it.
    Q_INVOKABLE bool addSet(const QString& setId);
    Q_INVOKABLE bool removeUri(const QString& uri);
    Q_INVOKABLE bool removeSet(const QString& setId);
    Q_INVOKABLE void removeAt(int row);
    Q_INVOKABLE bool rename(int row, const QString& name);
    Q_INVOKABLE bool contains(const QString& uri) const;
    Q_INVOKABLE bool containsSet(const QString& setId) const;
    Q_INVOKABLE QString uriAt(int row) const;
    Q_INVOKABLE QString targetAt(int row) const;
    /// "folder" or "set", or empty for a row that is not there.
    Q_INVOKABLE QString kindAt(int row) const;

    QList<Bookmark> bookmarks() const { return m_bookmarks; }

    /// False when the file is there and could not be read: it has been kept
    /// beside itself and this model will not write over it.
    bool load();
    [[nodiscard]] bool save();

    /// Where a file that could not be read was kept, or empty.
    QString damagedCopyPath() const { return m_file.damagedCopyPath(); }

signals:
    void countChanged();
    /// A write that did not land, with the reason in words and no path in it.
    /// The same signal JsonFileStore has: the bookmarks are a list model and
    /// cannot derive from it, so they hold the file instead. See ADR-0089.
    void saveFailed(const QString& reason);

private:
    /// "photos" out of "file:///home/user/photos"; the scheme for a root.
    static QString defaultNameFor(const QString& uri);
    /// The name to show: the set's current name while it exists, otherwise
    /// whatever was written down.
    QString displayName(const Bookmark& bookmark) const;
    bool isDead(const Bookmark& bookmark) const;
    bool add(Bookmark bookmark);
    bool contains(Bookmark::Kind kind, const QString& target) const;
    bool remove(Bookmark::Kind kind, const QString& target);
    /// A set was renamed, removed or created. Re-reads the name of every set row
    /// and tells the views, so the sidebar follows with nothing polling.
    void setsChanged();

    JsonFile m_file;
    FileSetStore* m_sets = nullptr;
    QList<Bookmark> m_bookmarks;
};

} // namespace mole
