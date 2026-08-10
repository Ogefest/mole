#include "ui/models/TaskListModel.h"

namespace mole {
namespace {

    QString stateText(Task::State state)
    {
        switch (state) {
        case Task::State::Pending:
            return QStringLiteral("Queued");
        case Task::State::Running:
            return QStringLiteral("Running");
        case Task::State::Succeeded:
            return QStringLiteral("Done");
        case Task::State::Failed:
            return QStringLiteral("Failed");
        case Task::State::Cancelled:
            return QStringLiteral("Cancelled");
        }
        return {};
    }

} // namespace

namespace {

    /// Housekeeping the user never asked for is dropped from the strip. A free
    /// space check every minute would otherwise push their actual copy off the
    /// list within the hour.
    QList<Task*> visibleOnly(const QList<Task*>& tasks)
    {
        QList<Task*> out;
        out.reserve(tasks.size());
        for (Task* task : tasks) {
            if (task && !task->isBackground())
                out.append(task);
        }
        return out;
    }

} // namespace

TaskListModel::TaskListModel(TaskManager* tasks, QObject* parent)
    : QAbstractListModel(parent)
    , m_tasks(tasks)
{
    if (!m_tasks)
        return;

    connect(m_tasks, &TaskManager::taskAppended, this, &TaskListModel::onTaskAppended);
    connect(m_tasks, &TaskManager::tasksAboutToBeReset, this, [this] { beginResetModel(); });
    connect(m_tasks, &TaskManager::tasksReset, this, [this] {
        m_rows = visibleOnly(m_tasks->tasks());
        endResetModel();
        emit countChanged();
        emit activeSummaryChanged();
    });
    connect(m_tasks, &TaskManager::activeCountChanged, this, &TaskListModel::activeCountChanged);

    // A clock, because elapsed time is the one figure that changes without
    // anything happening. Stopped when idle so an empty strip costs nothing.
    m_clock = new QTimer(this);
    m_clock->setInterval(500);
    connect(m_clock, &QTimer::timeout, this, [this] {
        emit activeSummaryChanged();
        if (!m_rows.isEmpty())
            emit dataChanged(index(0, 0), index(rowCount() - 1, 0), { ElapsedTextRole });
    });
    connect(this, &TaskListModel::activeCountChanged, this, [this] {
        if (activeCount() > 0)
            m_clock->start();
        else
            m_clock->stop();
    });

    reload();
}

Task* TaskListModel::firstRunning() const
{
    for (Task* task : m_rows) {
        if (task && !task->isFinished())
            return task;
    }
    return nullptr;
}

QString TaskListModel::activeTitle() const
{
    Task* task = firstRunning();
    return task ? task->title() : QString();
}

QString TaskListModel::activeElapsedText() const
{
    Task* task = firstRunning();
    return task && task->startedAt().isValid()
        ? TaskMetric::format(static_cast<double>(task->elapsedMs()), TaskMetric::Kind::Duration)
        : QString();
}

QString TaskListModel::activeRateText() const
{
    Task* task = firstRunning();
    return task ? task->metric(TaskMetrics::kRate).text : QString();
}

QString TaskListModel::activeTimeLeftText() const
{
    Task* task = firstRunning();
    return task ? task->metric(TaskMetrics::kTimeLeft).text : QString();
}

QString TaskListModel::activeStatus() const
{
    Task* task = firstRunning();
    if (!task)
        return {};
    return task->statusText().isEmpty() ? stateText(task->state()) : task->statusText();
}

int TaskListModel::activeProgress() const
{
    Task* task = firstRunning();
    // -1 means "unknown", which the interface renders as an indeterminate bar
    // rather than as zero -- zero looks stuck.
    return task ? task->progress() : -1;
}

int TaskListModel::finishedCount() const
{
    int finished = 0;
    for (Task* task : m_rows) {
        if (task && task->isFinished())
            ++finished;
    }
    return finished;
}

void TaskListModel::reload()
{
    beginResetModel();
    m_rows = m_tasks ? visibleOnly(m_tasks->tasks()) : QList<Task*> {};
    endResetModel();
    emit countChanged();
    emit activeSummaryChanged();

    for (Task* task : std::as_const(m_rows))
        onTaskAppended(task);
}

void TaskListModel::onTaskAppended(Task* task)
{
    if (!task || task->isBackground())
        return;

    if (!m_rows.contains(task)) {
        const int row = static_cast<int>(m_rows.size());
        beginInsertRows({}, row, row);
        m_rows.append(task);
        endInsertRows();
        emit countChanged();
        emit activeSummaryChanged();
    }

    // Per-task signals keep the row live without polling.
    connect(task, &Task::progressChanged, this, [this, task] { emitRowChanged(task); });
    connect(task, &Task::stateChanged, this, [this, task] { emitRowChanged(task); });
    connect(task, &Task::statusTextChanged, this, [this, task] { emitRowChanged(task); });
    connect(task, &Task::metricsChanged, this, [this, task] { emitRowChanged(task); });
}

void TaskListModel::emitRowChanged(const Task* task)
{
    // Any row moving can change which task is the one being reported, so the
    // summary is refreshed alongside the row rather than only on insertion.
    emit activeSummaryChanged();

    const int row = static_cast<int>(m_rows.indexOf(const_cast<Task*>(task)));
    if (row < 0)
        return;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx);
}

int TaskListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant TaskListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Task* task = m_rows.at(index.row());
    if (!task)
        return {};

    switch (role) {
    case TitleRole:
    case Qt::DisplayRole:
        return task->title();
    case StateRole:
        return QVariant::fromValue(task->state());
    case StateTextRole:
        return stateText(task->state());
    case ProgressRole:
        return task->progress();
    case StatusTextRole:
        return task->statusText();
    case IsFinishedRole:
        return task->isFinished();
    case StartedAtTextRole:
        return task->startedAt().isValid() ? task->startedAt().toString(QStringLiteral("HH:mm:ss"))
                                           : QString();
    case ElapsedTextRole:
        return task->startedAt().isValid()
            ? TaskMetric::format(static_cast<double>(task->elapsedMs()), TaskMetric::Kind::Duration)
            : QString();
    case MetricsRole: {
        QVariantList out;
        const QList<TaskMetric> metrics = task->metrics();
        for (const TaskMetric& metric : metrics) {
            if (metric.text.isEmpty())
                continue; // nothing worth a column
            out.append(QVariantMap { { QStringLiteral("key"), metric.key },
                { QStringLiteral("label"), metric.label }, { QStringLiteral("text"), metric.text } });
        }
        return out;
    }
    case CanCancelRole:
        return !task->isFinished();
    default:
        return {};
    }
}

QHash<int, QByteArray> TaskListModel::roleNames() const
{
    return {
        { TitleRole, "title" },
        { StateRole, "state" },
        { StateTextRole, "stateText" },
        { ProgressRole, "progress" },
        { StatusTextRole, "statusText" },
        { IsFinishedRole, "isFinished" },
        { CanCancelRole, "canCancel" },
        { StartedAtTextRole, "startedAtText" },
        { ElapsedTextRole, "elapsedText" },
        { MetricsRole, "metrics" },
    };
}

int TaskListModel::activeCount() const
{
    // Counted from the visible rows rather than taken from the manager, which
    // includes housekeeping. The strip hides a free-space or access check from
    // its list; reporting it in "1 running" would be the same information
    // leaking back in by another door.
    int active = 0;
    for (Task* task : m_rows) {
        if (task && !task->isFinished())
            ++active;
    }
    return active;
}

void TaskListModel::cancel(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    if (Task* task = m_rows.at(row))
        task->requestCancel();
}

void TaskListModel::cancelAll()
{
    if (m_tasks)
        m_tasks->cancelAll();
}

void TaskListModel::clearFinished()
{
    if (m_tasks)
        m_tasks->clearFinished();
}

} // namespace mole
