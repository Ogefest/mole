#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"

#include <QLocale>
#include <QSignalSpy>
#include <QThread>

#include <atomic>

using namespace mole;
using namespace mole::test;

namespace {

/// A task whose body is supplied by the test.
class ScriptedTask final : public Task
{
public:
    using Body = std::function<void(ScriptedTask&)>;

    ScriptedTask(QString title, Body body)
        : Task(std::move(title))
        , m_body(std::move(body))
    {
    }

    /// Re-exposed so the test can drive them from inside the body.
    using Task::fail;
    using Task::isCancelRequested;
    using Task::reportBytes;
    using Task::reportCount;
    using Task::reportText;
    using Task::setBytesDone;
    using Task::setByteTotal;
    using Task::setProgress;
    using Task::setStatusText;

    QThread* ranOn() const { return m_ranOn; }

protected:
    void run() override
    {
        m_ranOn = QThread::currentThread();
        if (m_body)
            m_body(*this);
    }

private:
    Body m_body;
    std::atomic<QThread*> m_ranOn { nullptr };
};

} // namespace

class TestTaskManager : public QObject
{
    Q_OBJECT

private slots:
    void runsTaskOffTheCallingThread();
    void reportsSuccessState();
    void failurePropagatesErrorAndState();
    void cancellationIsCooperative();
    void progressUpdatesReachTheUiThread();
    void activeCountTracksRunningTasks();
    void clearFinishedKeepsRunningTasks();
    void destroyingManagerCancelsAndWaits();

    void finishedTasksAreStampedAndRetired();
    void runningTasksAreNeverRetired();

    void aTaskPublishesWhateverItWants();
    void byteProgressDrivesThePercentageAndTheRate();
    void aCancelledTaskIsNotStillInProgress();
    void elapsedTimeStopsWhenTheTaskDoes();

    void aCancelledTaskIsNotLoggedAsAFailure();
    void aFailedTaskIsStillLoggedAsOne();
    void durationAndRateComeFromAClockThatCannotGoBackwards();
};

void TestTaskManager::runsTaskOffTheCallingThread()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("noop"), nullptr);
    manager.submit(task);

    QVERIFY(waitForTask(task));
    QVERIFY2(task->ranOn() != QThread::currentThread(),
        "tasks must never execute on the thread that submitted them");
}

void TestTaskManager::reportsSuccessState()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("ok"), [](ScriptedTask& self) {
        self.setStatusText(QStringLiteral("done"));
        self.setProgress(100);
    });

    QSignalSpy finished(task, &Task::finished);
    manager.submit(task);

    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->progress(), 100);
    QCOMPARE(task->statusText(), QStringLiteral("done"));
    QCOMPARE(finished.count(), 1);
}

void TestTaskManager::failurePropagatesErrorAndState()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("boom"), [](ScriptedTask& self) {
        self.fail(VfsError::make(VfsError::NetworkError, QStringLiteral("nas unreachable")));
    });
    manager.submit(task);

    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Failed);
    QCOMPARE(task->error().code, VfsError::NetworkError);
    QCOMPARE(errorTextOf(*task), QStringLiteral("nas unreachable"));
}

void TestTaskManager::cancellationIsCooperative()
{
    TaskManager manager;
    std::atomic_bool started { false };

    auto* task = new ScriptedTask(QStringLiteral("long"), [&started](ScriptedTask& self) {
        started = true;
        while (!self.isCancelRequested())
            QThread::msleep(5);
    });
    manager.submit(task);

    QVERIFY(waitFor([&started] { return started.load(); }));
    task->requestCancel();

    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Cancelled);
}

void TestTaskManager::progressUpdatesReachTheUiThread()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("progress"), [](ScriptedTask& self) {
        for (int i = 0; i <= 100; i += 25)
            self.setProgress(i);
    });

    QSignalSpy progress(task, &Task::progressChanged);
    manager.submit(task);

    QVERIFY(waitForTask(task));
    QCOMPARE(task->progress(), 100);
    QVERIFY2(progress.count() > 1, "intermediate progress must be observable, not just the final value");
}

void TestTaskManager::activeCountTracksRunningTasks()
{
    TaskManager manager;
    QCOMPARE(manager.activeCount(), 0);

    std::atomic_bool release { false };
    auto* task = new ScriptedTask(QStringLiteral("held"), [&release](ScriptedTask& self) {
        while (!release.load() && !self.isCancelRequested())
            QThread::msleep(5);
    });

    manager.submit(task);
    QCOMPARE(manager.activeCount(), 1);

    release = true;
    QVERIFY(waitForTask(task));
    QVERIFY(waitFor([&manager] { return manager.activeCount() == 0; }));
}

void TestTaskManager::clearFinishedKeepsRunningTasks()
{
    TaskManager manager;

    auto* done = new ScriptedTask(QStringLiteral("done"), nullptr);
    manager.submit(done);
    QVERIFY(waitForTask(done));

    std::atomic_bool release { false };
    auto* running = new ScriptedTask(QStringLiteral("running"), [&release](ScriptedTask& self) {
        while (!release.load() && !self.isCancelRequested())
            QThread::msleep(5);
    });
    manager.submit(running);

    QCOMPARE(manager.tasks().size(), 2);
    manager.clearFinished();
    QCOMPARE(manager.tasks().size(), 1);
    QCOMPARE(manager.tasks().first(), static_cast<Task*>(running));

    release = true;
    QVERIFY(waitForTask(running));
}

void TestTaskManager::destroyingManagerCancelsAndWaits()
{
    std::atomic_bool observedCancel { false };
    std::atomic_bool exited { false };

    {
        TaskManager manager;
        auto* task
            = new ScriptedTask(QStringLiteral("spinner"), [&observedCancel, &exited](ScriptedTask& self) {
                  while (!self.isCancelRequested())
                      QThread::msleep(5);
                  observedCancel = true;
                  exited = true;
              });
        manager.submit(task);
        QVERIFY(waitFor([task] { return task->state() == Task::State::Running; }));
    } // ~TaskManager must cancel the task and join the pool before returning.

    QVERIFY(observedCancel.load());
    QVERIFY(exited.load());
}

void TestTaskManager::finishedTasksAreStampedAndRetired()
{
    TaskManager manager;
    // Zero rather than the real hour: the behaviour is identical and the test
    // does not have to wait for it.
    manager.setRetention(std::chrono::seconds { 0 });

    auto* task = new ScriptedTask(QStringLiteral("quick"), [](ScriptedTask&) {});
    manager.submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }));
    QCOMPARE(manager.tasks().size(), 1);

    // Stamped when it ended, which is what the sweep compares against.
    QVERIFY(task->finishedAt().isValid());

    QCOMPARE(manager.retireOldTasks(), 1);
    QVERIFY(manager.tasks().isEmpty());

    // Nothing left to do on a second sweep.
    QCOMPARE(manager.retireOldTasks(), 0);
}

void TestTaskManager::runningTasksAreNeverRetired()
{
    TaskManager manager;
    manager.setRetention(std::chrono::seconds { 0 });

    std::atomic_bool release { false };
    auto* slow = new ScriptedTask(QStringLiteral("slow"), [&release](ScriptedTask& task) {
        while (!release.load() && !task.isCancelRequested())
            QThread::msleep(5);
    });
    manager.submit(slow);
    QVERIFY(waitFor([slow] { return slow->state() == Task::State::Running; }));

    // A sweep must not take work that is still going, however stale the list is.
    QCOMPARE(manager.retireOldTasks(), 0);
    QCOMPARE(manager.tasks().size(), 1);

    release.store(true);
    QVERIFY(waitFor([slow] { return slow->isFinished(); }));
    QCOMPARE(manager.retireOldTasks(), 1);
}

void TestTaskManager::aTaskPublishesWhateverItWants()
{
    // The point of the mechanism: a task reports what its own work is about,
    // and nothing above it needs to know the vocabulary in advance.
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("compare"), [](ScriptedTask& self) {
        self.reportCount(QStringLiteral("compared"), QStringLiteral("Compared"), 1200, 10);
        self.reportBytes(QStringLiteral("hashed"), QStringLiteral("Hashed"), 5 * 1024 * 1024, 20);
        self.reportText(
            QStringLiteral("strategy"), QStringLiteral("Strategy"), QStringLiteral("size then hash"), 30);
    });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    const QList<TaskMetric> metrics = task->metrics();
    QCOMPARE(metrics.size(), 3);

    // Ordered as the task asked, not as they happened to arrive.
    QCOMPARE(metrics.at(0).key, QStringLiteral("compared"));
    QCOMPARE(metrics.at(2).key, QStringLiteral("strategy"));

    // Formatted by kind, so the task never has to build the display string.
    QCOMPARE(metrics.at(0).text, QLocale().toString(1200));
    QVERIFY(metrics.at(1).text.contains(QStringLiteral("MiB")));
    QCOMPARE(metrics.at(2).text, QStringLiteral("size then hash"));
}

void TestTaskManager::byteProgressDrivesThePercentageAndTheRate()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("copy"), [](ScriptedTask& self) {
        self.setByteTotal(1000);
        self.setBytesDone(500);
        QThread::msleep(300); // long enough for the rate window to close
        self.setBytesDone(1000);
    });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    // Counted in bytes, not files: one large file otherwise sits at 0% and then
    // jumps to 100%, which is exactly the case a progress bar exists for.
    QCOMPARE(task->bytesTotal(), 1000);
    QCOMPARE(task->bytesDone(), 1000);
    QCOMPARE(task->progress(), 100);
    QVERIFY2(task->bytesPerSecond() > 0.0, "a throughput figure has to appear");
    QVERIFY(!task->metric(TaskMetrics::kRate).text.isEmpty());
}

void TestTaskManager::aCancelledTaskIsNotStillInProgress()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("scan"), [](ScriptedTask& self) {
        while (!self.isCancelRequested())
            QThread::msleep(5);
    });
    manager.submit(task);
    QVERIFY(waitFor([task] { return task->state() == Task::State::Running; }));

    // It never knew its total, so progress stays -1 -- which the interface
    // renders as an indeterminate bar. Once cancelled it must read as finished,
    // or the bar sweeps for ever as though the work were still going.
    QCOMPARE(task->progress(), -1);
    task->requestCancel();
    QVERIFY(waitFor([task] { return task->isFinished(); }));

    QCOMPARE(task->state(), Task::State::Cancelled);
    QVERIFY2(task->isFinished(), "the strip decides on this, not on the percentage");
    QVERIFY(task->finishedAt().isValid());
}

void TestTaskManager::elapsedTimeStopsWhenTheTaskDoes()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("brief"), [](ScriptedTask&) { QThread::msleep(60); });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    QVERIFY(task->startedAt().isValid());
    const qint64 took = task->elapsedMs();
    QVERIFY2(took >= 50, "the measurement has to cover the work");

    // Frozen once it ends: an elapsed time that keeps counting after the task
    // finished is not a measurement of anything.
    QThread::msleep(80);
    QCOMPARE(task->elapsedMs(), took);
}

/// The browser cancels a listing every time the folder changes or a filter
/// narrows, and each one used to leave a warning in the session log -- four in
/// fifteen milliseconds from an ordinary startup. The log is the one file
/// somebody opens when something has gone wrong, so a decision the application
/// took on purpose must not read like a fault there.
void TestTaskManager::aCancelledTaskIsNotLoggedAsAFailure()
{
    TaskManager manager;
    std::atomic_bool started { false };

    auto* task = new ScriptedTask(QStringLiteral("List somewhere"), [&started](ScriptedTask& self) {
        started = true;
        while (!self.isCancelRequested())
            QThread::msleep(1);
        // What every backend does when the token it was handed goes: it stops
        // where it is and reports Cancelled, which the task then carries.
        self.fail(VfsError::make(VfsError::Cancelled, QStringLiteral("Listing cancelled")));
    });

    CapturedWarnings warnings;
    manager.submit(task);
    QVERIFY(waitFor([&started] { return started.load(); }));
    task->requestCancel();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Cancelled);
    QVERIFY2(warnings.messages().isEmpty(),
        qPrintable(QStringLiteral("cancelling logged: %1").arg(warnings.joined())));
}

/// The other half of the same claim, and the reason the fix above cannot be
/// "stop logging": a task that failed because something went wrong still has to
/// leave the line that somebody will be asking about later.
void TestTaskManager::aFailedTaskIsStillLoggedAsOne()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("Copy"), [](ScriptedTask& self) {
        self.fail(VfsError::make(VfsError::NetworkError, QStringLiteral("the server stopped answering")));
    });

    CapturedWarnings warnings;
    manager.submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Failed);
    QVERIFY2(warnings.contains(QStringLiteral("the server stopped answering")),
        qPrintable(QStringLiteral("a real failure left no warning; captured: %1").arg(warnings.joined())));
}

void TestTaskManager::durationAndRateComeFromAClockThatCannotGoBackwards()
{
    // The machine's clock is stepped by ntp, or by somebody in another time
    // zone, while a job is running. Both numbers a task shows are differences
    // between two readings, and a wall clock read twice can go backwards -- a
    // job that has run for a minute reporting minus fifty-nine, and a rate to
    // match. Nothing here can step the system clock, and nothing needs to: what
    // the assertion holds is that neither figure is taken from it.
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("copy"), [](ScriptedTask& self) {
        self.setByteTotal(2000);
        self.setBytesDone(1000);
        QThread::msleep(300);
        self.setBytesDone(2000);
    });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    QVERIFY2(task->elapsedMs() >= 0, "a duration is never negative");
    QVERIFY2(task->bytesPerSecond() >= 0.0, "a rate is never negative");
    QVERIFY(qIsFinite(task->bytesPerSecond()));

    // And it stops when the task does, rather than growing for ever against a
    // clock nobody is winding.
    const qint64 first = task->elapsedMs();
    QThread::msleep(20);
    QCOMPARE(task->elapsedMs(), first);
}

MOLE_TEST_MAIN(TestTaskManager)
#include "tst_TaskManager.moc"
