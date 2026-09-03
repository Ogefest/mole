#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Creates one directory on a drive.
///
/// A task for one call, and for the same reason QueryAccessTask is one: the call
/// goes to storage. `makeDirectory()` on an SMB share that has stopped answering
/// blocks for as long as the mount takes to give up, and F7 used to make it from
/// the thread that draws the window -- so the whole interface stopped, with a
/// dialog still open on it. See ARCHITECTURE.md's first rule and MOLE-360.
///
/// Nothing about it is background: somebody pressed a key and is waiting for the
/// folder to appear, so it belongs in the strip like any other thing they asked
/// for.
class MakeDirectoryTask final : public Task
{
    Q_OBJECT

public:
    MakeDirectoryTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent = nullptr);

    const VfsUri& target() const { return m_target; }
    /// Meaningful once the task has finished: empty when the directory was made.
    QString failure() const { return m_failure; }

signals:
    /// Emitted on the UI thread when the directory is there. Whoever is showing
    /// the folder announces it -- this task does not touch the event bus, which
    /// belongs to the shell rather than to core.
    void created(const mole::VfsUri& target);
    /// Emitted on the UI thread with a reason somebody can read.
    void refused(const QString& reason);

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
    QString m_failure;
};

} // namespace mole
