#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <chrono>

namespace mole {

/// Finds what a drive is still holding that no listing shows, and can throw it
/// away.
///
/// The case it exists for is an upload interrupted by the process being killed:
/// S3 keeps the parts and charges for them until the upload is completed or
/// abandoned, and they are not objects, so nothing that lists a bucket will ever
/// mention them. Somebody whose machine lost power during a large copy is paying
/// for storage they cannot see.
///
/// A Task rather than a plain call, for the same reason DriveCheckTask is one:
/// IFileSystem is synchronous, and a bucket that is not answering would freeze
/// the window for the whole timeout.
class SweepLeftoversTask final : public Task
{
    Q_OBJECT

public:
    /// `olderThan` is the age below which a leftover is left alone -- see
    /// IFileSystem::leftovers(), where it is a correctness rule and not
    /// politeness. `discard` decides whether this reports or acts.
    SweepLeftoversTask(const QString& driveName, FileSystemPtr fileSystem, std::chrono::seconds olderThan,
        bool discard, QObject* parent = nullptr);

    /// What was found, whether or not it was then thrown away.
    QList<DriveLeftover> found() const { return m_found; }
    int discardedCount() const { return m_discarded; }
    /// One sentence, ready to show. Says what was found and what was done.
    QString summary() const { return m_summary; }

protected:
    void run() override;

private:
    QString m_driveName;
    FileSystemPtr m_fileSystem;
    std::chrono::seconds m_olderThan;
    bool m_discard = false;
    QList<DriveLeftover> m_found;
    int m_discarded = 0;
    QString m_summary;
};

} // namespace mole
