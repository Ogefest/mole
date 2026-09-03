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

    /// Carries out a plan that has already been built and shown.
    ///
    /// The confirmed plan and the executed plan have to be the same plan. Every
    /// run used to walk both trees afresh, so what the confirmation listed --
    /// the deletions of the dry run -- was not what apply() then carried out: a
    /// file that left the source in between became a mirror deletion nobody was
    /// shown, and this is the one operation in the application that deletes
    /// things nobody asked it to touch. See MOLE-337.
    ///
    /// The plan is still checked against the destination as it goes, step by
    /// step, because agreeing to a plan is not the same as freezing the disk.
    SyncTask(FileSystemPtr sourceFs, VfsUri source, FileSystemPtr targetFs, VfsUri target,
        SyncOptions options, SyncPlan plan, QObject* parent = nullptr);

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
    /// Puts a link at the far end pointing where the source one points, or fails
    /// the step by name on a drive that cannot hold one. See ADR-0092.
    bool linkOne(const SyncPlan::Step& step);

    /// Whether what is about to be removed is still what the plan recorded.
    ///
    /// A deletion is the only step here that cannot be undone, so it is the only
    /// one that asks again: between the plan being agreed to and this moment the
    /// destination may have gained the very file that is going, or gained one
    /// inside the folder that is about to go with everything in it.
    bool stillWhatThePlanSaw(const SyncPlan::Step& step, QString* changed) const;

    /// Weighs every copied file on the destination and fails the ones that do
    /// not match what was sent. One listing per directory, not one stat per
    /// file -- the same check TransferTask has had since ADR-0016, which a sync
    /// went without: a destination that acknowledges bytes and stores fewer
    /// failed a transfer and passed a sync.
    void verifyArrivals();

    /// One file that was written, and how many bytes went into it.
    struct Arrival
    {
        VfsUri target;
        qint64 bytes = 0;
    };

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
    /// True when the plan came from the caller rather than from this run. The
    /// comparison is then not done again, because doing it again is the fault.
    bool m_planWasGiven = false;
    QList<Arrival> m_arrivals;
    int m_applied = 0;
    QStringList m_failures;
};

} // namespace mole

Q_DECLARE_METATYPE(mole::SyncPlan)
