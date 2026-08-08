#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Asks a drive who may do what at a location.
///
/// A task, not a direct call, for the same reason as the free-space query: the
/// answer comes from storage, and on an unreachable mount that blocks. Marked
/// background, because it runs on every navigation and must not crowd out the
/// work the user started.
class QueryAccessTask final : public Task
{
    Q_OBJECT

public:
    QueryAccessTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent = nullptr);

    const VfsUri& target() const { return m_target; }

signals:
    /// Emitted on the UI thread. Not emitted when the drive cannot answer --
    /// "unknown" is a normal outcome, not a failure to report.
    void accessReady(const mole::VfsUri& target, mole::AccessInfo access);

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
};

} // namespace mole
