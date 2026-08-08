#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Reads one directory off the UI thread. Every navigation goes through this,
/// which is why a hung network mount shows a spinner instead of freezing.
class ListDirectoryTask final : public Task
{
    Q_OBJECT

public:
    ListDirectoryTask(FileSystemPtr fileSystem, VfsUri directory, QObject* parent = nullptr);

    const VfsUri& directory() const { return m_directory; }

signals:
    /// Delivered on the UI thread before finished().
    void listed(const mole::VfsUri& directory, const mole::FileEntryList& entries);

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_directory;
};

} // namespace mole
