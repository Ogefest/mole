#pragma once

#include "core/vfs/VfsUri.h"

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

namespace mole {

/// What an entry is, where it is neither an ordinary file nor a directory.
///
/// A listing that leaves these out is not a shorter listing, it is a wrong one.
/// A move copies what it was shown and then removes the source tree, so an entry
/// no listing ever mentioned is one nothing tried to copy, nothing counted as
/// failed, and `removeRecursively()` took away with everything else -- gone from
/// the source and never arrived. See MOLE-333.
///
/// Named rather than reduced to one flag, because a refusal has to say why: "a
/// link to nothing" and "a named pipe" are different things to be told, and the
/// second is not a fault at all -- opening a pipe for reading waits for a writer,
/// which for a copy means for ever.
enum class SpecialKind : quint8 {
    None, ///< an ordinary file or a directory
    DanglingLink, ///< a symbolic link whose target is not there
    Pipe, ///< a named pipe -- reading one waits for whoever writes to it
    Socket, ///< a unix-domain socket
    Device, ///< a block or character device node
    Other, ///< something this platform has that this list does not name
};

/// The phrase a refusal uses, in the shape of the rest of a failure line.
inline QString describe(SpecialKind kind)
{
    switch (kind) {
    case SpecialKind::None:
        return {};
    case SpecialKind::DanglingLink:
        return QStringLiteral("a link to nothing");
    case SpecialKind::Pipe:
        return QStringLiteral("a named pipe");
    case SpecialKind::Socket:
        return QStringLiteral("a socket");
    case SpecialKind::Device:
        return QStringLiteral("a device");
    case SpecialKind::Other:
        return QStringLiteral("neither a file nor a folder");
    }
    return {};
}

/// Nothing at all is known about the size, as against a size of nought.
///
/// `openRead(expectedSize)` has spelled this -1 all along; the listing side
/// could not, so five network parsers wrote 0 for a size the server did not
/// give or gave in a form they could not read -- and 0 is a size a file can
/// have. Both of `TransferTask`'s guards on ADR-0027's short-read check are
/// `expectedSize > 0`, so exactly the files whose size was unknown were the ones
/// copied with no short-read detection at all: a WebDAV server that omits
/// `getcontentlength` for a generated resource, an S3-compatible store whose
/// `Size` is not a number. See MOLE-344.
///
/// A reader that only wants to display it needs no special case -- a negative
/// size formats as an empty cell already. A reader that is about to *decide*
/// something has to tell -1 from 0.
inline constexpr qint64 kUnknownSize = -1;

/// A size out of a listing, or `kUnknownSize` where there is not one to be had.
///
/// One reader rather than one per protocol, because the mistake is the same
/// every time: `toLongLong()` answers 0 for an absent element, an empty one and
/// "n/a" alike, and none of those is a file of nought bytes. A negative number
/// is not a size either, whatever the server meant by it.
inline qint64 sizeFromListing(const QString& text)
{
    bool ok = false;
    const qint64 size = text.trimmed().toLongLong(&ok);
    return ok && size >= 0 ? size : kUnknownSize;
}

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
    /// A name that points at another node: a POSIX symbolic link, an NTFS
    /// symbolic link, or a junction. A walker that was not asked to follow
    /// links declines to descend into one.
    bool isSymlink = false;
    /// A Windows .lnk, which is not a link at all but an ordinary file that
    /// happens to contain a target.
    ///
    /// Separate from isSymlink because QFileInfo::isSymLink() answers true for
    /// both, so a folder of shortcuts was silently skipped by every sync plan
    /// and duplicate scan on the machine that has them. A shortcut is a file and
    /// gets treated as one; what it points at is a question nothing asks yet.
    bool isShortcut = false;
    bool isHidden = false;
    /// Set where this entry is not something a copy can stream. Everything above
    /// the backend can then refuse it by name and with a reason, instead of the
    /// backend leaving it out of the listing and nothing knowing it was there.
    SpecialKind special = SpecialKind::None;
    bool isReadable = true;
    bool isWritable = false;
    /// `kUnknownSize` where the drive did not say. Zero means a file of nought
    /// bytes, which is a different thing and a real answer.
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
