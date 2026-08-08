#pragma once

#include "core/tasks/Task.h"

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

signals:
    /// `task` has just been appended at the end of tasks().
    void taskAppended(mole::Task* task);
    void tasksAboutToBeReset();
    void tasksReset();
    void activeCountChanged();

private:
    void onTaskFinished(Task* task);

    QThreadPool* m_pool = nullptr;
    QList<Task*> m_tasks;
    int m_activeCount = 0;
    std::chrono::seconds m_retention { 3600 };
    QTimer* m_retirementTimer = nullptr;
};

} // namespace mole
