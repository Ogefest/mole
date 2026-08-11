#pragma once

#include "core/vfs/VfsUri.h"

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

namespace mole {

/// One directory entry, as reported by a backend. Deliberately flat and
/// copyable so it can be shipped across threads inside a QList.
struct FileEntry
{
    Q_GADGET
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(bool isDir MEMBER isDir)
    Q_PROPERTY(qint64 size MEMBER size)

public:
    QString name;
    VfsUri uri;
    bool isDir = false;
    bool isSymlink = false;
    bool isHidden = false;
    bool isReadable = true;
    bool isWritable = false;
    qint64 size = 0;
    QDateTime modified;
    /// When the drive says the file was made, and when it was last read.
    ///
    /// Invalid where the backend does not report them, which most do not: a
    /// listing over SFTP or S3 carries a modification time and nothing else.
    /// Invalid means *no answer*, never *the beginning of time* -- a search for
    /// files made this week must not sweep up everything on a drive that cannot
    /// say.
    QDateTime created;
    QDateTime accessed;

    /// "rwxr-xr--" when the backend knows it, empty when it does not. Watching
    /// this is how a permission change on a shared folder gets noticed at all;
    /// isReadable/isWritable only say what *this* process may do.
    QString permissions;

    /// Set only when the backend already knows it for free. Otherwise the
    /// preview layer sniffs the type lazily.
    QString mimeType;
};

using FileEntryList = QList<FileEntry>;

} // namespace mole

Q_DECLARE_METATYPE(mole::FileEntry)
Q_DECLARE_METATYPE(mole::FileEntryList)
