#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Asks a drive what it can offer, once, while somebody is looking at a folder
/// on it.
///
/// A Task rather than a plain call for the usual reason -- `IFileSystem` is
/// synchronous and a probe is a request to storage -- and a task of its own
/// rather than a step inside `ListDirectoryTask` for a second one: a probe that
/// hangs must leave the listing alone. Sharing a task would make every slow
/// answer about a capability nobody asked for into a folder that will not open.
///
/// It is deliberately silent. Nothing waits for it, it cannot fail in a way
/// anybody should be told about, and it runs on every navigation -- so it is
/// background and one of many, and the task strip never mentions it. See
/// ADR-0076 for when a drive is asked and why not sooner.
class ProbeDriveTask final : public Task
{
    Q_OBJECT

public:
    ProbeDriveTask(FileSystemPtr fileSystem, VfsUri target, QObject* parent = nullptr);

    /// What the drive said, once this has finished. Unasked when it had already
    /// been asked, which is the common case and not a failure.
    DriveOffers offers() const;

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    VfsUri m_target;
};

} // namespace mole
