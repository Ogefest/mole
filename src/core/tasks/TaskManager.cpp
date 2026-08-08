#include "core/tasks/TaskManager.h"

#include <QDateTime>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>

namespace mole {
namespace {

    class TaskRunnable final : public QRunnable
    {
    public:
        explicit TaskRunnable(Task* task)
            : m_task(task)
        {
            setAutoDelete(true);
        }

        void run() override
        {
            if (m_task)
                m_task->execute();
        }

    private:
        QPointer<Task> m_task;
    };

} // namespace

TaskManager::TaskManager(QObject* parent)
    : QObject(parent)
    , m_pool(new QThreadPool(this))
{
    // Leave headroom so a burst of scans cannot starve the machine.
    const int cores = QThread::idealThreadCount();
    m_pool->setMaxThreadCount(qBound(2, cores - 2, 8));

    // Swept rather than cleared on demand: a list nobody prunes grows for the
    // whole session, and by the end it is long enough that the one failure
    // worth seeing is buried in it.
    m_retirementTimer = new QTimer(this);
    m_retirementTimer->setInterval(60000);
    connect(m_retirementTimer, &QTimer::timeout, this, &TaskManager::retireOldTasks);
    m_retirementTimer->start();
    m_pool->setExpiryTimeout(30'000);
}

TaskManager::~TaskManager()
{
    cancelAll();
    // Tasks are children of this object, so they must outlive the pool threads
    // that are still touching them.
    m_pool->waitForDone();
}

void TaskManager::submit(Task* task)
{
    if (!task)
        return;

    task->setParent(this);
    m_tasks.append(task);

    connect(task, &Task::finished, this, [this, task] { onTaskFinished(task); });

    ++m_activeCount;
    emit taskAppended(task);
    emit activeCountChanged();

    m_pool->start(new TaskRunnable(task));
}

void TaskManager::onTaskFinished(Task* task)
{
    Q_UNUSED(task)
    if (m_activeCount > 0) {
        --m_activeCount;
        emit activeCountChanged();
    }
}

void TaskManager::cancelAll()
{
    for (Task* task : m_tasks) {
        if (task && !task->isFinished())
            task->requestCancel();
    }
}

int TaskManager::retireOldTasks()
{
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-m_retention.count());

    QList<Task*> remaining;
    QList<Task*> doomed;
    remaining.reserve(m_tasks.size());
    for (Task* task : std::as_const(m_tasks)) {
        // A finished task with no timestamp predates this behaviour or was
        // never stamped; keeping it is safer than guessing it is old.
        const bool expired
            = task && task->isFinished() && task->finishedAt().isValid() && task->finishedAt() < cutoff;
        if (expired)
            doomed.append(task);
        else
            remaining.append(task);
    }

    if (doomed.isEmpty())
        return 0;

    emit tasksAboutToBeReset();
    m_tasks = remaining;
    emit tasksReset();
    for (Task* task : std::as_const(doomed))
        task->deleteLater();
    return static_cast<int>(doomed.size());
}

void TaskManager::clearFinished()
{
    QList<Task*> remaining;
    QList<Task*> doomed;
    remaining.reserve(m_tasks.size());
    for (Task* task : m_tasks) {
        if (task && task->isFinished())
            doomed.append(task);
        else
            remaining.append(task);
    }

    if (doomed.isEmpty())
        return;

    emit tasksAboutToBeReset();
    m_tasks = remaining;
    emit tasksReset();

    for (Task* task : doomed)
        task->deleteLater();
}

void TaskManager::setMaxThreadCount(int count)
{
    m_pool->setMaxThreadCount(qMax(1, count));
}

} // namespace mole
