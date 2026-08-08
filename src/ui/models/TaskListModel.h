#pragma once

#include "core/tasks/TaskManager.h"

#include <QAbstractListModel>
#include <QTimer>

namespace mole {

/// Background work, visible. Every scan, listing and search shows up here so
/// nothing the application does behind the user's back is actually hidden.
class TaskListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)

public:
    /// What is happening right now, for the collapsed strip. Without it the
    /// only sign of background work was a count, which reads as decoration
    /// rather than as something in progress.
    Q_PROPERTY(QString activeTitle READ activeTitle NOTIFY activeSummaryChanged)
    Q_PROPERTY(QString activeStatus READ activeStatus NOTIFY activeSummaryChanged)
    Q_PROPERTY(int activeProgress READ activeProgress NOTIFY activeSummaryChanged)
    /// Finished tasks still in the list, i.e. what "Clear finished" would drop.
    Q_PROPERTY(int finishedCount READ finishedCount NOTIFY activeSummaryChanged)
    Q_PROPERTY(QString activeElapsedText READ activeElapsedText NOTIFY activeSummaryChanged)
    Q_PROPERTY(QString activeRateText READ activeRateText NOTIFY activeSummaryChanged)

    enum Role {
        TitleRole = Qt::UserRole + 1,
        StateRole,
        StateTextRole,
        ProgressRole,
        StatusTextRole,
        IsFinishedRole,
        CanCancelRole,
        /// When it started and how long it has been going, so a long copy can
        /// be watched rather than guessed at.
        StartedAtTextRole,
        ElapsedTextRole,
        /// Everything the task chose to publish, as {label, text} for the view
        /// to lay out. The strip knows nothing about what any of them mean.
        MetricsRole,
    };

    explicit TaskListModel(TaskManager* tasks, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int activeCount() const;

    Q_INVOKABLE void cancel(int row);
    Q_INVOKABLE void cancelAll();
    Q_INVOKABLE void clearFinished();

    /// Ticks while anything is running, so elapsed times count up on their own.
    QString activeTitle() const;
    QString activeStatus() const;
    int activeProgress() const;
    int finishedCount() const;
    QString activeElapsedText() const;
    QString activeRateText() const;

signals:
    void countChanged();
    void activeCountChanged();
    void activeSummaryChanged();

private:
    /// The task the strip should be talking about: the first one still running.
    Task* firstRunning() const;

    void onTaskAppended(Task* task);
    void reload();
    void emitRowChanged(const Task* task);

    TaskManager* m_tasks = nullptr;
    QList<Task*> m_rows;
    /// Elapsed time changes with the clock and nothing else, so something has
    /// to say so. Runs only while there is work in flight.
    QTimer* m_clock = nullptr;
};

} // namespace mole
