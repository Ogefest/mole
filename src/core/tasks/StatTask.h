#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Asks a drive about one file.
///
/// One call, and a task for it, because the call goes to storage: `stat()` on a
/// share that has stopped answering blocks for as long as the mount takes to
/// give up. The preview did it on the thread that draws for every F3 and every
/// arrow step, with a comment saying the entry was "usually already known" --
/// which is true of a local disk and of nothing else. See MOLE-360.
///
/// Background, because nobody is looking at a task strip for this: it is one
/// step of opening something the user asked for, and a row of its own would be
/// noise. `entryReady` carries the failure as well as the answer, so a caller
/// that has a name and a uri to fall back on can go on with those.
class StatTask final : public Task
{
    Q_OBJECT

public:
    StatTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent = nullptr);

    const VfsUri& target() const { return m_target; }

signals:
    /// Emitted on the UI thread, once, whatever happened. `found` is false when
    /// the drive could not say -- which is not a failure worth a red task: a file
    /// that has gone is an ordinary answer.
    void entryReady(const mole::VfsUri& target, bool found, mole::FileEntry entry);

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
};

} // namespace mole
