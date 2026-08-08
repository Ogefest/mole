#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/models/TaskListModel.h"

#include "core/tasks/TaskManager.h"

#include <QSignalSpy>
#include <QThread>

#include <atomic>

using namespace mole;
using namespace mole::test;

namespace {

/// A task the test starts and stops on command, so "while something is
/// running" is a state the assertions can actually stand in.
class HeldTask final : public Task
{
public:
    explicit HeldTask(QString title)
        : Task(std::move(title))
    {
    }

    void release() { m_release.store(true); }
    using Task::setBackground;
    using Task::setProgress;
    using Task::setStatusText;

protected:
    void run() override
    {
        setStatusText(QStringLiteral("halfway"));
        setProgress(50);
        while (!m_release.load() && !isCancelRequested())
            QThread::msleep(5);
    }

private:
    std::atomic_bool m_release { false };
};

/// Housekeeping the user never asked for.
class QuietTask final : public Task
{
public:
    QuietTask()
        : Task(QStringLiteral("free space"))
    {
        setBackground(true);
    }

protected:
    void run() override { }
};

} // namespace

/// The strip's view of the task list: what is running, what has finished, and
/// what it must not show at all.
class TestTaskListModel : public QObject
{
    Q_OBJECT

private slots:
    void reportsWhatIsRunning();
    void saysNothingWhenIdle();
    void countsWhatHasFinished();
    void hidesBackgroundHousekeeping();
    void doesNotCountHousekeepingAsRunning();
    void summaryIsNotifiedNotPolled();

private:
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<TaskListModel> m_model;

    void build()
    {
        m_tasks = std::make_unique<TaskManager>();
        m_model = std::make_unique<TaskListModel>(m_tasks.get());
    }
};

void TestTaskListModel::reportsWhatIsRunning()
{
    build();
    auto* held = new HeldTask(QStringLiteral("Copy notes.txt"));
    m_tasks->submit(held);

    // A count alone reads as decoration. The strip needs to say what is
    // happening, which means the model has to know.
    QVERIFY(waitFor([this] { return m_model->activeTitle() == QStringLiteral("Copy notes.txt"); }));
    QVERIFY(waitFor([this] { return m_model->activeProgress() == 50; }));
    QCOMPARE(m_model->activeStatus(), QStringLiteral("halfway"));

    held->release();
    QVERIFY(waitFor([held] { return held->isFinished(); }));
    QVERIFY(waitFor([this] { return m_model->activeTitle().isEmpty(); }));
}

void TestTaskListModel::saysNothingWhenIdle()
{
    build();
    QCOMPARE(m_model->activeTitle(), QString());
    QCOMPARE(m_model->activeStatus(), QString());
    // Unknown, not zero: an indeterminate bar reads as working, a bar at zero
    // reads as stuck.
    QCOMPARE(m_model->activeProgress(), -1);
    QCOMPARE(m_model->finishedCount(), 0);
}

void TestTaskListModel::countsWhatHasFinished()
{
    build();
    auto* held = new HeldTask(QStringLiteral("one"));
    m_tasks->submit(held);
    QVERIFY(waitFor([this] { return !m_model->activeTitle().isEmpty(); }));
    QCOMPARE(m_model->finishedCount(), 0);

    held->release();
    QVERIFY(waitFor([this] { return m_model->finishedCount() == 1; }));

    // "Clear finished" appears on this number, so it must not count the row
    // that is still going.
    auto* second = new HeldTask(QStringLiteral("two"));
    m_tasks->submit(second);
    QVERIFY(waitFor([this] { return !m_model->activeTitle().isEmpty(); }));
    QCOMPARE(m_model->finishedCount(), 1);
    second->release();
}

void TestTaskListModel::hidesBackgroundHousekeeping()
{
    build();
    m_tasks->submit(new QuietTask());
    drainEvents();

    // A free-space check every minute would push the user's own copy off the
    // strip within the hour.
    QCOMPARE(m_model->rowCount(), 0);
    QCOMPARE(m_model->activeTitle(), QString());
    QCOMPARE(m_model->finishedCount(), 0);
}

void TestTaskListModel::summaryIsNotifiedNotPolled()
{
    build();
    QSignalSpy summary(m_model.get(), &TaskListModel::activeSummaryChanged);

    auto* held = new HeldTask(QStringLiteral("Copy"));
    m_tasks->submit(held);
    QVERIFY(waitFor([&summary] { return summary.count() > 0; }));

    // Counted before releasing, or the signal being waited for has already
    // been emitted by the time the baseline is taken.
    const int beforeFinish = summary.count();
    held->release();

    // Without the signal the strip would only update when something else
    // happened to repaint it, which is how a progress bar comes to look frozen.
    QVERIFY(waitFor([&summary, beforeFinish] { return summary.count() > beforeFinish; }));
    QVERIFY(waitFor([this] { return m_model->activeTitle().isEmpty(); }));
}

void TestTaskListModel::doesNotCountHousekeepingAsRunning()
{
    build();

    auto* quiet = new HeldTask(QStringLiteral("free space"));
    quiet->setBackground(true);
    m_tasks->submit(quiet);
    QVERIFY(waitFor([quiet] { return quiet->state() == Task::State::Running; }));

    // The strip hides a background check from its list. Reporting it in
    // "1 running" would be the same information leaking back in by another
    // door -- and it made a transfer test see work that was never started.
    QCOMPARE(m_model->rowCount(), 0);
    QCOMPARE(m_model->activeCount(), 0);

    auto* real = new HeldTask(QStringLiteral("Copy"));
    m_tasks->submit(real);
    QVERIFY(waitFor([this] { return m_model->activeCount() == 1; }));

    quiet->release();
    real->release();
}

MOLE_TEST_MAIN(TestTaskListModel)
#include "tst_TaskListModel.moc"
