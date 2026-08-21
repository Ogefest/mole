#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Asks a drive what it can do to one file that no other drive could.
///
/// A Task because asking is a call into storage: a drive may have to ask the far
/// end, and the thread that draws must never wait on that. It runs whenever the
/// cursor lands somewhere new, so it is background and one of many -- the task
/// strip has no business listing one per keystroke.
///
/// The node is stat()ed here rather than passed in, so a caller that has only a
/// uri -- a menu, a command palette, anything not holding a listing -- can ask.
class QueryFileActionsTask final : public Task
{
    Q_OBJECT

public:
    QueryFileActionsTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent = nullptr);

    const VfsUri& target() const { return m_target; }
    /// What the drive offered. Empty for every backend that contributes nothing,
    /// which is most of them, and empty when the ask failed.
    const FileActionList& actions() const { return m_actions; }

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
    FileActionList m_actions;
};

} // namespace mole
