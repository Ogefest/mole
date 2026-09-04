#include "core/tasks/TaskManager.h"

#include "core/diagnostics/Diagnostics.h"

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
    // that are still touching them -- which is why this waits at all.
    if (m_pool->waitForDone(kQuitGraceMs))
        return;

    // Past the grace, something is in a call the token cannot reach. Waiting for
    // ever is what hung the process with the window already gone, so the pool and
    // whatever it is still running are cut loose and leaked on purpose: the
    // process is ending. Named first, because "Mole took a while to quit" is
    // otherwise a report with nothing in it.
    QStringList stuck;
    for (Task* task : std::as_const(m_tasks)) {
        if (!task || task->isFinished())
            continue;
        stuck.append(task->title());
        // Out of the child list, so ~QObject does not delete it under the thread
        // that is inside its run().
        task->setParent(nullptr);
    }
    qCWarning(taskLog, "quitting without waiting for %lld task(s) still in a call: %s",
        static_cast<long long>(stuck.size()), qPrintable(stuck.join(QStringLiteral(", "))));
    m_pool->setParent(nullptr);
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

    dispatch(task);
}

void TaskManager::dispatch(Task* task)
{
    const void* lane = task->lane();
    // A task that names no drive has no lane to be bounded by, and nothing to
    // bound it against: it is not a call into a backend.
    if (!lane) {
        m_pool->start(new TaskRunnable(task));
        return;
    }

    Lane& queue = m_lanes[lane];
    if (queue.running >= perDriveLimit()) {
        queue.waiting.append(task);
        return;
    }
    ++queue.running;
    m_pool->start(new TaskRunnable(task));
}

void TaskManager::drainLane(const void* lane)
{
    const auto found = m_lanes.find(lane);
    if (found == m_lanes.end())
        return;

    while (found->running < perDriveLimit() && !found->waiting.isEmpty()) {
        Task* next = found->waiting.takeFirst();
        if (!next)
            continue; // retired or deleted while it waited
        ++found->running;
        m_pool->start(new TaskRunnable(next));
    }
    if (found->running == 0 && found->waiting.isEmpty())
        m_lanes.erase(found);
}

void TaskManager::onTaskFinished(Task* task)
{
    if (task && task->lane()) {
        const auto found = m_lanes.find(task->lane());
        if (found != m_lanes.end() && found->running > 0)
            --found->running;
        drainLane(task->lane());
    }
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
    // A smaller pool means a smaller per-drive limit, and whatever was waiting
    // for a lane may now be over it -- or under it, if the pool grew.
    const QList<const void*> lanes = m_lanes.keys();
    for (const void* lane : lanes)
        drainLane(lane);
}

int TaskManager::maxThreadCount() const
{
    return m_pool->maxThreadCount();
}

int TaskManager::perDriveLimit() const
{
    return qMax(1, m_pool->maxThreadCount() / 2);
}

int TaskManager::queuedForADriveCount() const
{
    int waiting = 0;
    for (const Lane& lane : m_lanes)
        waiting += static_cast<int>(lane.waiting.size());
    return waiting;
}

} // namespace mole
