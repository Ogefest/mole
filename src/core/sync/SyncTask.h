#pragma once

#include "core/sync/SyncPlan.h"
#include "core/tasks/Task.h"

namespace mole {

/// Works out what a sync would do and, unless it is a dry run, does it.
///
/// One task for both, because a dry run that took a different path would not be
/// telling you about the real one. The plan is built identically either way and
/// the last step is simply withheld.
class SyncTask final : public Task
{
    Q_OBJECT

public:
    SyncTask(FileSystemPtr sourceFs, VfsUri source, FileSystemPtr targetFs, VfsUri target,
        SyncOptions options, QObject* parent = nullptr);

    /// Valid once finished.
    SyncPlan plan() const { return m_plan; }
    int appliedCount() const { return m_applied; }
    QStringList failures() const { return m_failures; }

signals:
    /// Emitted once the comparison is done, before anything is written -- so a
    /// dry run reports through the same signal a real one does.
    void planReady(const mole::SyncPlan& plan);

protected:
    void run() override;

private:
    bool copyOne(const SyncPlan::Step& step);

    FileSystemPtr m_sourceFs;
    VfsUri m_source;
    FileSystemPtr m_targetFs;
    VfsUri m_target;
    SyncOptions m_options;

    /// Bytes copied by the steps that are already finished.
    ///
    /// Its own counter, the way TransferTask keeps one. It used to read the
    /// figure back with `bytesDone()` and add the chunk it had just written,
    /// which was a number the drawing thread refreshes at most every
    /// `Task::kDrainIntervalMs` -- so between two of those, every iteration
    /// added its chunk to the same stale total and the count stopped advancing
    /// while the copy itself was perfectly correct.
    qint64 m_bytesCopied = 0;

    SyncPlan m_plan;
    int m_applied = 0;
    QStringList m_failures;
};

} // namespace mole

Q_DECLARE_METATYPE(mole::SyncPlan)
