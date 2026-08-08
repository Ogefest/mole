#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Asks a drive how full it is.
///
/// It exists as a task rather than a direct call because the answer comes from
/// storage: `QStorageInfo` on an unreachable NFS mount blocks until the kernel
/// gives up, and the one rule this application does not bend is that the UI
/// thread never waits on a disk.
///
/// Marked as background work, so a housekeeping refresh every minute does not
/// scroll the user's real copies and scans off the task strip.
class QuerySpaceTask final : public Task
{
    Q_OBJECT

public:
    QuerySpaceTask(FileSystemPtr fileSystem, VfsUri root, QString mountId, QObject* parent = nullptr);

    const QString& mountId() const { return m_mountId; }
    /// Only meaningful once the task has succeeded.
    const SpaceInfo& info() const { return m_info; }

signals:
    /// Emitted on the UI thread. Not emitted at all when the backend cannot
    /// answer -- "unknown" is a normal outcome, not a failure to report.
    void spaceReady(const QString& mountId, mole::SpaceInfo info);

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_root;
    QString m_mountId;
    SpaceInfo m_info;
};

} // namespace mole
