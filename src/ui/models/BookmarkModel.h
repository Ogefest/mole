#pragma once

#include <QAbstractListModel>
#include <QString>

namespace mole {

struct Bookmark
{
    QString name;
    QString uri;
};

/// The user's saved places.
///
/// Separate from the drive list on purpose: drives are what the machine has,
/// bookmarks are what the person cares about. A bookmark can point anywhere a
/// uri can -- a folder on disk, a path inside a mounted archive, a directory
/// on a network share.
class BookmarkModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        UriRole,
    };

    /// `MOLE_BOOKMARKS_PATH` wins, so tests never touch the user's real list.
    static QString defaultFilePath();

    explicit BookmarkModel(QString filePath, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Ignores a uri that is already bookmarked, so adding twice is harmless
    /// rather than producing a duplicate row.
    Q_INVOKABLE bool add(const QString& uri, const QString& name = {});
    Q_INVOKABLE bool removeUri(const QString& uri);
    Q_INVOKABLE void removeAt(int row);
    Q_INVOKABLE bool rename(int row, const QString& name);
    Q_INVOKABLE bool contains(const QString& uri) const;
    Q_INVOKABLE QString uriAt(int row) const;

    QList<Bookmark> bookmarks() const { return m_bookmarks; }

    bool load();
    bool save() const;

signals:
    void countChanged();

private:
    /// "photos" out of "file:///home/user/photos"; the scheme for a root.
    static QString defaultNameFor(const QString& uri);

    QString m_filePath;
    QList<Bookmark> m_bookmarks;
};

} // namespace mole
