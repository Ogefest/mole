#pragma once

#include "core/sets/FileSet.h"

#include <QObject>

namespace mole {

/// Where the sets live. One file, written whole.
class FileSetStore : public QObject
{
    Q_OBJECT

public:
    explicit FileSetStore(QString path, QObject* parent = nullptr);

    /// Honours MOLE_SETS_PATH so tests never touch the user's own sets.
    static QString defaultPath();

    bool load();
    bool save() const;

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
    QString m_path;
    QList<FileSet> m_sets;
};

} // namespace mole
