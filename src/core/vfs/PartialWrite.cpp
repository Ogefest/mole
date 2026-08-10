#include "core/vfs/PartialWrite.h"

namespace mole {

VfsUri partialWriteOf(const VfsUri& target)
{
    return target.parent().child(target.fileName() + kPartialWriteSuffix);
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
        if (!mayReplace) {
            fs.remove(staging, false);
            return VfsError::make(VfsError::AlreadyExists,
                QStringLiteral("%1 appeared while it was being written, so the result was not put in place")
                    .arg(target.path()));
        }
        // An overwrite, which is what was asked for. Cleared first because a
        // rename onto an existing name is refused outright by SFTP and by
        // WebDAV's MOVE, and silently does the wrong thing on one or two FTP
        // servers -- so the one behaviour is spelled out rather than left to
        // each protocol's opinion.
        const Result<void> cleared = fs.remove(target, false);
        if (!cleared.ok()) {
            fs.remove(staging, false);
            return cleared.error();
        }
    } else if (occupied.error().code != VfsError::NotFound) {
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

} // namespace mole
