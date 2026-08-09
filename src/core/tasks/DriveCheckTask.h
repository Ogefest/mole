#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Finds out whether a configured drive can actually be reached.
///
/// Building a backend proves nothing. A factory only constructs an object; the
/// first real request is what discovers a wrong endpoint, a refused password, a
/// bucket that is not there or a certificate that does not match the host. Until
/// something asks, a drive that cannot work looks exactly like one that can.
///
/// Without this the news arrives at the end of a long road — save the drive,
/// connect it, open it, walk into a folder, and only then read something about a
/// certificate subject name. This does the first request at the point the
/// configuration was entered, while the person who typed it is still looking at
/// it, and says plainly what happened.
///
/// A Task rather than a plain call, because IFileSystem is synchronous and must
/// never be touched from the UI thread: a check against a host that is not
/// answering would freeze the window for the whole timeout.
class DriveCheckTask final : public Task
{
    Q_OBJECT

public:
    DriveCheckTask(
        const QString& driveName, FileSystemPtr fileSystem, VfsUri root, QObject* parent = nullptr);

signals:
    /// Emitted on the UI thread once the check is done, with a message ready to
    /// show: what was found, or why it could not be.
    void checked(bool reachable, const QString& message);

protected:
    void run() override;

private:
    QString m_driveName;
    FileSystemPtr m_fileSystem;
    VfsUri m_root;
};

} // namespace mole
