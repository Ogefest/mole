#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QLocale>
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QThread>

#include <atomic>
#include <cmath>
#include <stdexcept>

using namespace mole;
using namespace mole::test;

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
    void aTaskWithASpeedAndASizeSaysHowLongIsLeft();
    void withNoTotalThereIsNoEstimate();
    void aStallHoldsTheLastEstimateRatherThanShowingInfinity();
    void aCancelledTaskIsNotStillInProgress();
    void elapsedTimeStopsWhenTheTaskDoes();

    void aCancelledTaskIsNotLoggedAsAFailure();
    void aFailedTaskIsStillLoggedAsOne();
    void durationAndRateComeFromAClockThatCannotGoBackwards();

    void progressNeverGoesBackwardsAndNeverPassesTheTotal();
    void bytesDoneIsWhatTheTaskActuallyMoved();
    void aTaskWhoseBodyThrowsIsReportedFailedAndThePoolSurvives();

    void aJobSomebodyStartedIsInTheLogWithNothingSwitchedOn();
    void oneOfACrowdIsNotAndTurningTheCategoryUpBringsItBack();
    void aTaskThatSaysNothingAboutItselfIsLoud();
    void tenThousandMetricUpdatesDoNotFloodTheQueue();
    void tenThousandStatusLinesDoNotFloodTheQueue();
    void aCountReportedPerItemIsCoalescedTheSameWay();
    void theLastReadingOfATaskThatGoesQuietStillArrives();
    void aTaskOutlivesTheDriveItWasGiven();
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

// --- how long is left -------------------------------------------------------
//
// The sleeps below are the work taking time, not the test waiting on a clock: a
// rate is bytes divided by elapsed time, so a task that finishes instantly has
// no rate to publish and nothing to estimate from. Every assertion is on what
// the task published, never on when.

void TestTaskManager::aTaskWithASpeedAndASizeSaysHowLongIsLeft()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("copy"), [](ScriptedTask& self) {
        self.setByteTotal(1000000);
        // A fifth of the file per closed sampling window. Four windows, so three
        // rate samples close and the third is where an estimate first appears.
        for (int step = 1; step <= 4 && !self.isCancelRequested(); ++step) {
            QThread::msleep(260);
            self.setBytesDone(step * 200000);
        }
    });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    const TaskMetric left = task->metric(TaskMetrics::kTimeLeft);
    QVERIFY2(!left.text.isEmpty(), "a task that knows its speed and its size has to say how long is left");
    QCOMPARE(left.kind, TaskMetric::Kind::Duration);
    QCOMPARE(left.label, QStringLiteral("Left"));

    // Two hundred thousand bytes left at roughly 770 kB/s is a few hundred
    // milliseconds. Bounded generously on both sides rather than compared: the
    // claim is that the arithmetic is the right arithmetic, and a loaded machine
    // is allowed to be slow without turning this red.
    QVERIFY2(left.value > 20.0 && left.value < 20000.0,
        qPrintable(QStringLiteral("an estimate of %1 ms is not from this file").arg(left.value)));

    // Right after the rate, so the two read together.
    QVERIFY(left.order > task->metric(TaskMetrics::kRate).order);
}

void TestTaskManager::withNoTotalThereIsNoEstimate()
{
    TaskManager manager;
    // Bytes reported, and plenty of time for the rate to settle, but nobody ever
    // said how much there was to move. There is no denominator, so there is
    // nothing to say -- and an estimate invented from a total nobody declared
    // would be a number read once and believed.
    auto* task = new ScriptedTask(QStringLiteral("scan"), [](ScriptedTask& self) {
        for (int step = 1; step <= 4 && !self.isCancelRequested(); ++step) {
            QThread::msleep(260);
            self.setBytesDone(step * 200000);
        }
    });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    QVERIFY(!task->metric(TaskMetrics::kRate).text.isEmpty());
    QVERIFY2(task->metric(TaskMetrics::kTimeLeft).text.isEmpty(), "no total, no estimate");
}

void TestTaskManager::aStallHoldsTheLastEstimateRatherThanShowingInfinity()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("copy"), [](ScriptedTask& self) {
        self.setByteTotal(1000000);
        for (int step = 1; step <= 4 && !self.isCancelRequested(); ++step) {
            QThread::msleep(260);
            self.setBytesDone(step * 200000);
        }
        // And then it stops moving, while still being asked about. The smoothed
        // rate decays towards nothing, and dividing by that gives a figure that
        // runs off to hours and then to infinity.
        for (int idle = 0; idle < 6 && !self.isCancelRequested(); ++idle) {
            QThread::msleep(260);
            self.setBytesDone(800000);
        }
    });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    const TaskMetric left = task->metric(TaskMetrics::kTimeLeft);
    QVERIFY2(!left.text.isEmpty(), "a stalled task keeps the last figure that meant something");
    QVERIFY2(std::isfinite(left.value), "and it is a number");
    QVERIFY2(!left.text.contains(QStringLiteral("inf")), qPrintable(left.text));
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
/// A session log from an ordinary run has to say what ran.
///
/// ADR-0012 put a task's start and end at debug, so neither reached the file unless
/// somebody had set MOLE_LOG beforehand -- which is the one thing nobody has done
/// before the run that goes wrong. The lines existed and were switched off. See
/// MOLE-262 and ADR-0064.
void TestTaskManager::aJobSomebodyStartedIsInTheLogWithNothingSwitchedOn()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("Copy 4 files"), [](ScriptedTask&) {});

    // From info, because that is the level the claim is about. Nothing in the
    // environment is touched: this is what a plain run writes.
    CapturedWarnings log(QtInfoMsg);
    manager.submit(task);
    QVERIFY(waitForTask(task));

    QVERIFY2(log.contains(QStringLiteral("Copy 4 files")) && log.contains(QStringLiteral("started")),
        qPrintable(QStringLiteral("no start line: %1").arg(log.joined())));
    QVERIFY2(log.contains(QStringLiteral("finished")),
        qPrintable(QStringLiteral("no outcome line: %1").arg(log.joined())));
}

/// And the crowd stays out of it, which is what ADR-0012 was right about.
void TestTaskManager::oneOfACrowdIsNotAndTurningTheCategoryUpBringsItBack()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("List somewhere"), [](ScriptedTask&) {});
    task->setOneOfMany(true);

    {
        CapturedWarnings log(QtInfoMsg);
        manager.submit(task);
        QVERIFY(waitForTask(task));
        QVERIFY2(!log.contains(QStringLiteral("List somewhere")),
            qPrintable(QStringLiteral("a listing wrote to the log: %1").arg(log.joined())));
    }

    // Nothing became unseeable: the category still turns everything on. Captured
    // from debug, which is where a crowd's lines are.
    auto* again = new ScriptedTask(QStringLiteral("List somewhere else"), [](ScriptedTask&) {});
    again->setOneOfMany(true);
    QLoggingCategory::setFilterRules(QStringLiteral("mole.task.debug=true"));
    {
        CapturedWarnings log(QtDebugMsg);
        manager.submit(again);
        QVERIFY(waitForTask(again));
        QVERIFY2(log.contains(QStringLiteral("List somewhere else")),
            qPrintable(QStringLiteral("MOLE_LOG=task stopped working: %1").arg(log.joined())));
    }
    QLoggingCategory::setFilterRules(QString());
}

/// The default is the loud one, asserted rather than assumed.
///
/// A task type written next year should be in the log because nobody had to
/// remember to put it there. `ScriptedTask` says nothing about itself here -- no
/// setOneOfMany, no setBackground -- which is exactly the shape of that new type.
void TestTaskManager::aTaskThatSaysNothingAboutItselfIsLoud()
{
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("Whatever gets written next"), [](ScriptedTask&) {});
    QVERIFY2(!task->isOneOfMany(), "the default has to be the loud answer");

    CapturedWarnings log(QtInfoMsg);
    manager.submit(task);
    QVERIFY(waitForTask(task));
    // Both lines, not just the title: the outcome line alone would let a revert of
    // the start line through, which is what happened the first time this was run
    // against a deliberately broken build.
    QVERIFY2(log.contains(QStringLiteral("Whatever gets written next: started"))
            || log.contains(QStringLiteral("]: started")),
        qPrintable(QStringLiteral("no start line: %1").arg(log.joined())));
    QVERIFY2(log.contains(QStringLiteral("Whatever gets written next")),
        qPrintable(QStringLiteral("a task that said nothing was silent: %1").arg(log.joined())));
}

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

void TestTaskManager::progressNeverGoesBackwardsAndNeverPassesTheTotal()
{
    // The bar is a promise about how much is left. One that slides back, or sits
    // at 103%, is worse than no bar: it turns "nearly finished" into a guess.
    TaskManager manager;
    QList<int> seen;

    auto* task = new ScriptedTask(QStringLiteral("copy"), [](ScriptedTask& self) {
        self.setByteTotal(1000);
        // Deliberately hostile: out of order, past the end, and negative.
        self.setBytesDone(400);
        self.setBytesDone(200);
        self.setBytesDone(1500);
        self.setBytesDone(-50);
        self.setBytesDone(1000);
        self.setProgress(50);
        self.setProgress(20);
        self.setProgress(140);
    });
    connect(task, &Task::progressChanged, task, [task, &seen] { seen.append(task->progress()); });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    QVERIFY2(!seen.isEmpty(), "something has to be reported");
    int highest = -1;
    for (int value : seen) {
        QVERIFY2(value >= 0 && value <= 100, qPrintable(QStringLiteral("progress reached %1").arg(value)));
        QVERIFY2(value >= highest,
            qPrintable(QStringLiteral("progress went from %1 back to %2").arg(highest).arg(value)));
        highest = value;
    }
}

void TestTaskManager::bytesDoneIsWhatTheTaskActuallyMoved()
{
    // The number the arrival check compares against. If it counts anything other
    // than bytes that moved -- a chunk counted twice, a skipped file added in --
    // then every check built on it is comparing against fiction.
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("copy"), [](ScriptedTask& self) {
        self.setByteTotal(4096);
        qint64 moved = 0;
        for (int i = 0; i < 4; ++i) {
            moved += 1024;
            self.setBytesDone(moved);
        }
    });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    QCOMPARE(task->bytesDone(), 4096);
    QCOMPARE(task->progress(), 100);
}

void TestTaskManager::aTaskWhoseBodyThrowsIsReportedFailedAndThePoolSurvives()
{
    // A backend throwing std::bad_alloc, or a plugin doing something a plugin
    // should not. An exception escaping into the pool takes the process with it,
    // and the job it was running disappears with no record of why.
    TaskManager manager;
    auto* thrower = new ScriptedTask(
        QStringLiteral("throws"), [](ScriptedTask&) { throw std::runtime_error("something gave way"); });
    manager.submit(thrower);
    QVERIFY2(waitForTask(thrower), "a task that threw still has to reach an end");
    QCOMPARE(thrower->state(), Task::State::Failed);
    QVERIFY2(!errorTextOf(*thrower).isEmpty(), "and it has to say what happened");

    // And the pool is still usable afterwards, which is the half that would
    // otherwise be discovered by everything after it silently never running.
    auto* after
        = new ScriptedTask(QStringLiteral("after"), [](ScriptedTask& self) { self.setProgress(100); });
    manager.submit(after);
    QVERIFY(waitForTask(after));
    QCOMPARE(after->state(), Task::State::Succeeded);
}

void TestTaskManager::tenThousandMetricUpdatesDoNotFloodTheQueue()
{
    // A chunk loop reports every chunk, and a fast local copy runs thousands a
    // second. Every one of them is a queued event carrying a number nobody can
    // read that fast, and an interface that spends its time draining them is an
    // interface that stops answering.
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("busy"), [](ScriptedTask& self) {
        self.setByteTotal(10000);
        for (int i = 1; i <= 10000; ++i)
            self.setBytesDone(i);
    });

    int notifications = 0;
    connect(task, &Task::progressChanged, task, [&notifications] { ++notifications; });
    manager.submit(task);
    QVERIFY(waitForTask(task));
    drainEvents();

    QCOMPARE(task->bytesDone(), 10000);
    QVERIFY2(notifications < 500,
        qPrintable(
            QStringLiteral("%1 notifications for 10000 updates is not coalescing").arg(notifications)));
}

void TestTaskManager::tenThousandStatusLinesDoNotFloodTheQueue()
{
    // Analysing a folder wrote a status line per entry it walked. The text
    // carries a running total, so it differs on every call and the value check
    // never fires; a metadata walk opens no file, so it produces those lines
    // tens of thousands a second. Each one became a queued event, and each event
    // cost the drawing thread a row change, a delegate update and a relayout, so
    // the queue grew for the length of the walk and the window never got a frame.
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("analyse"), [](ScriptedTask& self) {
        for (int i = 1; i <= 10000; ++i)
            self.setStatusText(QStringLiteral("%1 files, %2 folders").arg(i).arg(i / 10));
    });

    QSignalSpy status(task, &Task::statusTextChanged);
    manager.submit(task);

    QVERIFY(waitForTask(task));
    drainEvents();

    // Whatever the clock let through, the line the task ended on is the one the
    // row is left holding. A finished task showing a figure from the middle of
    // its run reads as one that stopped early.
    QTRY_COMPARE(task->statusText(), QStringLiteral("10000 files, 1000 folders"));
    QVERIFY2(status.count() < 100,
        qPrintable(
            QStringLiteral("%1 notifications for 10000 status lines is not coalescing").arg(status.count())));
}

void TestTaskManager::aCountReportedPerItemIsCoalescedTheSameWay()
{
    // The same fault one door along. A rename publishes the number renamed per
    // entry and a sync the number applied per step, and that number differs on
    // every call, so the no-op check never fires. It used to be taken inside the
    // queued lambda -- after the event had been posted and delivered, which saved
    // the signal and none of the cost of getting there.
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("rename"), [](ScriptedTask& self) {
        for (int i = 1; i <= 10000; ++i) {
            self.reportCount(QStringLiteral("renamed"), QStringLiteral("Renamed"), i, 10);
            self.reportCount(QStringLiteral("failed"), QStringLiteral("Failed"), i / 100, 20);
        }
    });

    QSignalSpy metrics(task, &Task::metricsChanged);
    manager.submit(task);

    QVERIFY(waitForTask(task));
    drainEvents();

    // Both keys arrive, and both hold the last figure the task published: the
    // window holds one reading per metric rather than one in total.
    QTRY_COMPARE(task->metric(QStringLiteral("renamed")).value, 10000.0);
    QCOMPARE(task->metric(QStringLiteral("failed")).value, 100.0);
    QVERIFY2(metrics.count() < 100,
        qPrintable(
            QStringLiteral("%1 notifications for 20000 reports is not coalescing").arg(metrics.count())));
}

void TestTaskManager::theLastReadingOfATaskThatGoesQuietStillArrives()
{
    // A stalled transfer is the case this exists for: it publishes a rate, an
    // estimate and a byte count in one breath and then says nothing more for as
    // long as the connection is down. Coalescing that waits for the caller's
    // next update leaves those last figures in the task's hand for the rest of
    // the run, with the strip showing the reading before them -- and the reading
    // before them is the one that says the transfer is still moving.
    TaskManager manager;
    auto* task = new ScriptedTask(QStringLiteral("copy"), [](ScriptedTask& self) {
        // Two in the same breath, so the second falls inside the window the
        // first opened and cannot go out alongside it.
        self.setStatusText(QStringLiteral("connected"));
        self.setStatusText(QStringLiteral("stalled at 4 MB"));
        self.reportCount(QStringLiteral("moved"), QStringLiteral("Moved"), 4194304);
        while (!self.isCancelRequested())
            QThread::msleep(5);
    });
    manager.submit(task);

    QTRY_COMPARE(task->statusText(), QStringLiteral("stalled at 4 MB"));
    QTRY_COMPARE(task->metric(QStringLiteral("moved")).value, 4194304.0);
    QVERIFY2(!task->isFinished(), "the claim is that it arrived while the task was still running");

    task->requestCancel();
    QVERIFY(waitForTask(task));
}

void TestTaskManager::aTaskOutlivesTheDriveItWasGiven()
{
    // A drive is ejected while a job is running. The job holds a shared_ptr, so
    // the backend stays alive until the job lets go of it -- asserted here
    // rather than assumed, because the alternative is a use-after-free in a
    // worker thread, which is the hardest kind of crash to read.
    TaskManager manager;
    auto drive = std::make_shared<MemoryFileSystem>();
    drive->addFile(QStringLiteral("/data/file.bin"), QByteArray(4096, 'x'));
    std::weak_ptr<MemoryFileSystem> watch = drive;

    auto* task = new ScriptedTask(QStringLiteral("reads"), [drive](ScriptedTask& self) {
        for (int i = 0; i < 20; ++i) {
            Result<std::unique_ptr<QIODevice>> reader
                = drive->openRead(VfsUri::fromString(QStringLiteral("mem:///data/file.bin")));
            if (!reader.ok())
                self.fail(reader.error());
            self.setBytesDone(4096);
        }
    });
    manager.submit(task);

    // The mount table lets go of it while the job is running.
    drive.reset();
    QVERIFY2(!watch.expired(), "the running task is holding the drive up");

    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Succeeded);
    drainEvents();
}

MOLE_TEST_MAIN(TestTaskManager)
#include "tst_TaskManager.moc"
