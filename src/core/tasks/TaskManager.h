#pragma once

#include "core/tasks/Task.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <chrono>

class QThreadPool;

namespace mole {

/// Runs Tasks on a bounded thread pool and keeps the list the UI displays.
///
/// This is the only place allowed to call into a backend, which is what makes
/// "the UI never blocks" an enforceable rule rather than a good intention.
///
/// **No one drive may hold more than half the pool.** A backend call on a mount
/// that has stopped answering -- an NFS server that went away, a yanked USB disk,
/// a stuck SMB share -- blocks in the kernel and cannot be interrupted by the
/// cooperative token: `QStorageInfo`, `QDirIterator`, `QFile::open` all wait for
/// as long as the kernel takes, which for a hard NFS mount is about fifteen
/// minutes and can be for ever. With one unbounded pool, a single dead mount in
/// the sidebar took every thread in under ten minutes without anybody touching
/// it, and every listing, copy, preview and search on every other drive queued
/// behind it. The window kept painting -- "the UI thread never touches storage"
/// held -- and the application had stopped, which is the freeze moved rather than
/// removed. So each task declares its drive (Task::lane()) and a lane at its
/// limit queues here instead of on the pool. See ADR-0095 and MOLE-362.
class TaskManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)

public:
    explicit TaskManager(QObject* parent = nullptr);
    ~TaskManager() override;

    /// Takes ownership of `task` and queues it. Must be called from the thread
    /// that owns the TaskManager (the UI thread).
    void submit(Task* task);

    QList<Task*> tasks() const { return m_tasks; }
    int activeCount() const { return m_activeCount; }

    void cancelAll();
    /// Drops finished tasks from the list; running ones stay.
    void clearFinished();
    /// Drops finished tasks older than the retention period. Called on a timer;
    /// exposed so a test does not have to wait an hour.
    Q_INVOKABLE int retireOldTasks();
    /// How long a finished task stays in the list. The default is an hour --
    /// long enough to notice what happened, short enough that a long session
    /// does not accumulate a thousand rows nobody will read.
    void setRetention(std::chrono::seconds retention) { m_retention = retention; }

    void setMaxThreadCount(int count);
    int maxThreadCount() const;
    /// The most of the pool one drive may hold at once: half of it, and never
    /// fewer than one. Half because the point is that *something else* can
    /// always start, and one because a pool of two still has to make progress.
    int perDriveLimit() const;
    /// How many tasks are waiting for a lane rather than for a thread. Zero on an
    /// ordinary run; a test asserts on it, and so could a diagnostic.
    int queuedForADriveCount() const;

    /// How long the destructor waits for a task to notice it has been cancelled.
    ///
    /// ~TaskManager() used to be `cancelAll(); waitForDone();` with no bound, so
    /// quitting while a task sat in an uninterruptible call hung the process with
    /// the window already gone -- which is what makes people reach for `kill -9`,
    /// the one thing ADR-0020 says leaves wreckage behind. Past the grace the pool
    /// and whatever it is still running are deliberately leaked: the process is
    /// ending, and a leak at exit costs nothing while a hang costs the session.
    static constexpr int kQuitGraceMs = 3000;

signals:
    /// `task` has just been appended at the end of tasks().
    void taskAppended(mole::Task* task);
    void tasksAboutToBeReset();
    void tasksReset();
    void activeCountChanged();

private:
    void onTaskFinished(Task* task);
    /// Hands the task to the pool, or to its lane's queue when the lane is full.
    void dispatch(Task* task);
    /// Starts whatever the lane has room for now.
    void drainLane(const void* lane);

    /// What one drive has running and what is waiting for it. Only ever touched
    /// on the thread that owns this object, which is the same rule submit() has
    /// always had -- finished() arrives there too, so no lock is needed.
    struct Lane
    {
        int running = 0;
        QList<QPointer<Task>> waiting;
    };

    QThreadPool* m_pool = nullptr;
    QList<Task*> m_tasks;
    QHash<const void*, Lane> m_lanes;
    int m_activeCount = 0;
    std::chrono::seconds m_retention { 3600 };
    QTimer* m_retirementTimer = nullptr;
};

} // namespace mole
