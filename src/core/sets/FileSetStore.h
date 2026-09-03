#pragma once

#include "core/data/JsonFileStore.h"
#include "core/sets/FileSet.h"

#include <QObject>

namespace mole {

/// Where the sets live. One file, written whole.
class FileSetStore : public JsonFileStore
{
    Q_OBJECT

public:
    explicit FileSetStore(QString path, QObject* parent = nullptr);

    /// Honours MOLE_SETS_PATH so tests never touch the user's own sets.
    static QString defaultPath();

    /// False when the file is there and could not be read: it has been kept
    /// beside itself and this store will not write over it. A set is something somebody built by hand.
    bool load();
    [[nodiscard]] bool save();

    QList<FileSet> sets() const { return m_sets; }
    FileSet set(const QString& id) const;
    /// Creates one and returns it, with an id already assigned.
    FileSet create(const QString& name, const QList<QString>& uris = {});
    bool put(const FileSet& set);
    bool remove(const QString& id);
    bool rename(const QString& id, const QString& name);

    /// Adds members, ignoring any already there. Returns how many were new --
    /// which is what the interface reports, because "added 0 of 12" is the
    /// answer that explains itself.
    int addTo(const QString& id, const QList<QString>& uris);
    int removeFrom(const QString& id, const QList<QString>& uris);

    /// Which sets a uri belongs to, for marking it in a listing.
    QStringList setsContaining(const QString& uri) const;

signals:
    void setsChanged();

private:
    QList<FileSet> m_sets;
};

} // namespace mole
