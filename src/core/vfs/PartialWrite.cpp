#include "core/vfs/PartialWrite.h"

#include <QRandomGenerator>

namespace mole {

VfsUri partialWriteOf(const VfsUri& target)
{
    QString name = target.fileName();

    // **A token, so two writers cannot share a staging name.** The name is cut
    // to fit below, deterministically, so two long names agreeing in their first
    // 242 bytes -- which is what a common prefix and a differing tail look like
    // -- produced *one* `.mole-partial` name. Two concurrent writes then
    // truncated over each other: two copy tasks, or two Moles, or one of each.
    // Eight hex characters is enough that a collision needs 2^32 tries, and it
    // keeps isPartialWrite() a suffix test, which is what the sweep and every
    // listing rely on. See MOLE-359.
    const QString token
        = QString::number(QRandomGenerator::global()->generate(), 16).rightJustified(8, QLatin1Char('0'));
    const QString tail = QLatin1Char('.') + token + kPartialWriteSuffix;

    // Almost every filesystem stops a name at 255 bytes, so a file already at
    // the limit cannot wear the suffix as well -- and a copy of it would fail
    // with "file name too long" against a name the user never typed. The base
    // gives way instead, on whole characters so the result is still text, and
    // the suffix stays whole because the suffix is what the name is *for*.
    constexpr int kNameLimit = 255;
    const int room = kNameLimit - int(tail.toUtf8().size());
    while (name.toUtf8().size() > room && !name.isEmpty())
        name.chop(1);

    return target.parent().child(name + tail);
}

bool isPartialWrite(const QString& name)
{
    return name.endsWith(kPartialWriteSuffix);
}

VfsError commitPartialWrite(IFileSystem& fs, const VfsUri& staging, const VfsUri& target, bool mayReplace)
{
    // What is at the destination now, against what was there when this started.
    // Something that appeared in between is data this write was never asked to
    // touch, and a rename that quietly replaced it would destroy it.
    const Result<FileEntry> occupied = fs.stat(target);
    if (occupied.ok()) {
        // A folder standing where the file was meant to land. openWrite() refuses
        // this before a byte is written, so getting here means one arrived during
        // the write -- and replace() would take it away, with everything in it if
        // the drive's remove is recursive. Nothing this write was asked to do
        // includes that.
        if (occupied.value().isDir && !occupied.value().isSymlink) {
            fs.remove(staging, false);
            return VfsError::make(VfsError::IsADirectory,
                QStringLiteral("%1 is a folder, and a file cannot be put in its place").arg(target.path()));
        }
        if (!mayReplace) {
            fs.remove(staging, false);
            return VfsError::make(VfsError::AlreadyExists,
                QStringLiteral("%1 appeared while it was being written, so the result was not put in place")
                    .arg(target.path()));
        }
        // An overwrite, which is what was asked for -- and the drive is asked to
        // do it rather than told how. A rename onto an existing name is refused
        // outright by SFTP and by WebDAV's MOVE and does the wrong thing on one
        // or two FTP servers, so those get the clear-then-rename that is all a
        // protocol can offer; a local disk does it in one step, and the instant
        // between the two calls -- in which the file being replaced is already
        // gone and the replacement is not there yet -- does not exist for it.
        // See ADR-0087.
        const Result<void> replaced = fs.replace(staging, target);
        if (!replaced.ok()) {
            fs.remove(staging, false);
            return replaced.error();
        }
        return VfsError::ok();
    }
    if (occupied.error().code != VfsError::NotFound) {
        // Not there is one answer; could not find out is another, and only the
        // first of them makes a rename safe. Guessing here is guessing about
        // whether somebody else's file is about to be replaced.
        fs.remove(staging, false);
        return occupied.error();
    }

    const Result<void> renamed = fs.rename(staging, target);
    if (!renamed.ok()) {
        fs.remove(staging, false);
        return renamed.error();
    }
    return VfsError::ok();
}

VfsError refuseWritingOntoAFolder(IFileSystem& fs, const VfsUri& target, bool* replacing)
{
    const Result<FileEntry> standing = fs.stat(target);
    if (replacing)
        *replacing = standing.ok();
    if (standing.ok() && standing.value().isDir && !standing.value().isSymlink) {
        return VfsError::make(
            VfsError::IsADirectory, QStringLiteral("%1 is a folder, not a file").arg(target.path()));
    }
    return VfsError::ok();
}

} // namespace mole
