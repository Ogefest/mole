#pragma once

#include "core/vfs/VfsUri.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace mole {

/// A named, hand-built collection of files and folders.
///
/// Gathered from anywhere, across any number of drives, and then treated as a
/// thing in its own right: analysed, searched within, copied, renamed.
///
/// The design constraint that matters is on the *other* side. Operations already
/// take "the things to act on" as a list of uris from a pane's selection, so a
/// set has only to present itself the same way. Get that right and using a set
/// as a target costs nothing; get it wrong and every operation grows a second
/// code path for sets, then a third for whatever comes after them.
struct FileSet
{
    QString id;
    QString name;
    QString note;
    QDateTime createdAt;
    QDateTime updatedAt;
    /// In the order they were added. A set is a list somebody built, and
    /// re-sorting it would throw away the only ordering they chose.
    QList<QString> uris;

    bool isValid() const { return !id.isEmpty() && !name.isEmpty(); }
    int count() const { return static_cast<int>(uris.size()); }
    bool contains(const QString& uri) const { return uris.contains(uri); }

    /// The members as uris, which is exactly what every operation already takes.
    QList<VfsUri> targets() const;

    /// How many distinct drives it spans. A set crossing drives is normal here
    /// and worth saying, because most operations then involve a transfer.
    int driveCount() const;

    QJsonObject toJson() const;
    static FileSet fromJson(const QJsonObject& json);
};

} // namespace mole
