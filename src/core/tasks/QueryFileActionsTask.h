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
/// The node is stat()ed here when the caller has only a uri -- a menu, a command
/// palette, anything not holding a listing. **A caller that already has the row
/// hands it over**, and then this costs no I/O at all: the pane asks on every
/// cursor step, so holding Down through five thousand rows of an object store
/// was five thousand HEAD requests for an answer the listing already had.
/// See MOLE-394.
class QueryFileActionsTask final : public Task
{
    Q_OBJECT

public:
    QueryFileActionsTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent = nullptr);
    QueryFileActionsTask(FileSystemPtr fileSystem, VfsUri target, FileEntry known, QObject* parent = nullptr);

    const VfsUri& target() const { return m_target; }
    /// What the drive offered. Empty for every backend that contributes nothing,
    /// which is most of them, and empty when the ask failed.
    const FileActionList& actions() const { return m_actions; }

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
    /// What the caller already knew about the row. Invalid when it knew nothing,
    /// which is when run() has to ask the drive.
    FileEntry m_known;
    bool m_haveEntry = false;
    FileActionList m_actions;
};

} // namespace mole
