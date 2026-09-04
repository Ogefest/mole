#pragma once

#include "core/vfs/VfsTypes.h"
#include "core/vfs/VfsUri.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>

namespace mole {

class IFileSystem;

/// One named quantity a task publishes about its own work.
///
/// Tasks are going to keep arriving -- sync, duplicate detection, bulk rename --
/// and each will have something different worth watching: bytes for a copy,
/// files compared for a duplicate scan, names rewritten for a rename. Rather
/// than the interface learning about each one, a task publishes whatever it has
/// and the strip renders it.
///
/// `kind` exists so the interface can format sensibly without parsing the text:
/// a rate gets "/s", a duration gets a clock, bytes get scaled units.
struct TaskMetric
{
    enum class Kind {
        Count, ///< a plain number of things
        Bytes, ///< scaled to KiB/MiB/GiB
        Rate, ///< bytes per second
        Duration, ///< milliseconds
        Text ///< anything else, already formatted
    };

    QString key; ///< stable identifier, e.g. "bytes"; how updates are matched
    QString label; ///< what to call it, e.g. "Copied"
    QString text; ///< display form, filled in by the task or by format()
    double value = 0.0; ///< raw value, for a bar or a graph
    Kind kind = Kind::Text;
    /// Lower sorts first. Lets a task put the number that matters most in front.
    int order = 100;

    /// The display form for a value of this kind. Used when `text` is empty.
    static QString format(double value, Kind kind);
};

/// Well-known keys. A task is free to publish anything, but using these lets
/// the interface do something specific -- drive the progress bar, show a rate --
/// rather than only listing them.
namespace TaskMetrics {
    inline constexpr QLatin1String kBytesDone("bytes.done");
    inline constexpr QLatin1String kBytesTotal("bytes.total");
    inline constexpr QLatin1String kRate("bytes.rate");
    /// How long is left, in milliseconds. Published by Task itself for anything
    /// that measures bytes, so no task grows its own arithmetic.
    inline constexpr QLatin1String kTimeLeft("time.left");
    inline constexpr QLatin1String kFiles("files.done");
} // namespace TaskMetrics

/// Base class for anything that runs in the background: directory scans,
/// copies, duplicate hunts, index rebuilds.
///
/// THREADING CONTRACT
/// ------------------
/// The Task object itself lives on the UI thread. run() executes on a pool
/// thread. Subclasses must touch nothing but their own private data inside
/// run(), and report outward only through setProgress()/setStatusText() (both
/// marshal back to the UI thread for you) or through a queued signal carrying
/// a copyable payload.
class Task : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    /// A task that cannot say how far along it is. Distinct from nought per
    /// cent, which is a claim about a total that is known.
    static constexpr int kIndeterminateProgress = -1;

    enum class State { Pending, Running, Succeeded, Failed, Cancelled };
    Q_ENUM(State)

    explicit Task(QString title, QObject* parent = nullptr);
    ~Task() override;

    const QString& id() const { return m_id; }
    const QString& title() const { return m_title; }
    State state() const { return m_state; }
    /// 0..100, or -1 when the total amount of work is not known yet.
    int progress() const { return m_progress; }
    QString statusText() const { return m_statusText; }
    const VfsError& error() const { return m_error; }

    bool isFinished() const;
    /// When it reached a terminal state, so a finished task can be retired
    /// after a while instead of sitting in the list for the whole session.
    const QDateTime& finishedAt() const { return m_finishedAt; }
    /// When it began running, which is not when it was submitted -- a task can
    /// sit in the queue behind others.
    const QDateTime& startedAt() const { return m_startedAt; }
    /// How long it has been running, or how long it took. 0 before it starts.
    qint64 elapsedMs() const;

    // ---- what this task is reporting -------------------------------------

    /// Everything this task has published, in display order. UI thread only.
    QList<TaskMetric> metrics() const;
    /// One metric by key, or a default-constructed one when absent.
    TaskMetric metric(const QString& key) const;

    /// Bytes moved so far, or -1 when this task does not measure bytes.
    ///
    /// **Safe from any thread, and current rather than drawn.** These three come
    /// from what the worker last reported, not from the metric map above -- that
    /// map is filled on the drawing thread when the report box is drained, so
    /// reading it from a worker was both a data race and an answer up to
    /// `kDrainIntervalMs` out of date. A task that built its running total by
    /// reading one of these back therefore added every chunk to the same stale
    /// number until the next drain; see `SyncTask::copyOne`, which did.
    qint64 bytesDone() const;
    qint64 bytesTotal() const;
    /// Recent throughput in bytes per second, or 0 before there is enough to
    /// say. Measured over a short window rather than over the whole run, so a
    /// stall shows up instead of being averaged away by a fast start.
    double bytesPerSecond() const;

    /// Housekeeping the user did not ask for -- a periodic free-space check,
    /// say. Kept out of the task strip so it cannot bury the work they did
    /// ask for. Still cancelled and awaited like anything else.
    bool isBackground() const { return m_background; }

    /// One of a crowd: a job the user's gesture produced by the hundred, where
    /// this particular one is of no interest on its own.
    ///
    /// A different question from isBackground(), and about different jobs. A
    /// thumbnail is *not* background -- opening a folder of photographs is
    /// exactly asking for it -- but it is one of three hundred, and neither the
    /// log nor the task strip is improved by naming each. A copy, a scan, a sync
    /// or a duplicate hunt is one job somebody remembers starting and will ask
    /// about afterwards; that is the other answer.
    ///
    /// **False by default, which is the loud answer.** A task type written next
    /// year is in the log because nobody had to remember to put it there; the
    /// four bulk types say so for themselves. See ADR-0064.
    bool isOneOfMany() const { return m_oneOfMany; }

    /// Safe to call from any thread. Cooperative -- run() must poll.
    Q_INVOKABLE void requestCancel();

    /// The locations this task reads or writes, so the interface can say which
    /// drives are in use. Empty for a task that has not said.
    ///
    /// **Declared by the task, because only the task knows** -- a copy touches
    /// two drives and usually does, a scan touches one, and nothing outside can
    /// work that out from a title. Nothing is derived and nothing is polled: the
    /// list is fixed when the task is built and read when it starts and ends.
    ///
    /// Silence lights nothing, which is the right default. What the sidebar shows
    /// is work somebody asked for; a task that reads a file to say what it is, or
    /// asks a mount how full it is, has no business making a drive look busy. See
    /// docs/adr/0052-a-drives-dot-says-what-it-is-doing.md.
    QList<VfsUri> touching() const { return m_touching; }

    /// The drive this task's work runs on, as a key, or null for a task that
    /// touches no drive at all.
    ///
    /// **This is about scheduling and touching() is about the sidebar**, which is
    /// why they are two questions. A listing, a space query, a ranged read and a
    /// thumbnail deliberately light no dot -- they arrive by the hundred -- and
    /// those are exactly the tasks whose lane matters: eight of them against a
    /// mount that has stopped answering are what used to take the whole pool.
    /// TaskManager lets no single lane hold more than half of it. See ADR-0095 and
    /// MOLE-362.
    const void* lane() const { return m_lane; }

    /// Called by TaskManager on a pool thread. Not part of the public API.
    void execute();

    /// The shortest gap between two updates handed to the drawing thread, in
    /// milliseconds. Ten a second is more than anybody reads and few enough that
    /// the thread has time left to draw; the figure matches the one
    /// setBytesDone() has always used for the same reason.
    static constexpr qint64 kDrainIntervalMs = 100;

signals:
    void stateChanged();
    void progressChanged();
    void statusTextChanged();
    /// Emitted on the UI thread once the task reaches a terminal state.
    void finished();
    /// A published quantity changed. Coalesced, so this is safe to bind to.
    void metricsChanged();

protected:
    /// The actual work. Runs on a pool thread.
    virtual void run() = 0;

    // --- helpers usable from run() ---
    bool isCancelRequested() const { return m_cancel.isCancelled(); }
    const CancelToken& cancelToken() const { return m_cancel; }
    void setBackground(bool background) { m_background = background; }
    /// Says this job is one of a crowd -- see isOneOfMany(). Called from the
    /// constructor, like setBackground().
    void setOneOfMany(bool oneOfMany) { m_oneOfMany = oneOfMany; }
    /// Names a location this task reads or writes. Called from the constructor,
    /// where the task's own arguments are, and never afterwards: the answer must
    /// not change while something is watching it.
    /// Says which drive this task works on, for scheduling. Called from the
    /// constructor, from the FileSystemPtr the task was handed.
    ///
    /// A task holding two drives names one -- whichever it is chiefly reading or
    /// writing. One slot per task is what bounds a dead mount; a task holding two
    /// lanes at once would be a lock ordering problem for no gain.
    void noteRunsOn(const std::shared_ptr<IFileSystem>& fileSystem) { m_lane = fileSystem.get(); }

    void noteTouching(const VfsUri& uri)
    {
        if (uri.isValid() && !m_touching.contains(uri))
            m_touching.append(uri);
    }
    void noteTouching(const QList<VfsUri>& uris)
    {
        for (const VfsUri& uri : uris)
            noteTouching(uri);
    }

    /// Publishes or updates a metric. Safe from the worker thread; coalesced on
    /// the worker thread, so a report per chunk does not flood the event queue.
    void report(TaskMetric metric);
    /// Shorthand for the common shapes.
    void reportCount(const QString& key, const QString& label, double value, int order = 100);
    void reportBytes(const QString& key, const QString& label, qint64 bytes, int order = 100);
    void reportText(const QString& key, const QString& label, const QString& text, int order = 100);

    /// Declares how much there is to move, before moving any of it.
    void setByteTotal(qint64 total);
    /// Reports cumulative bytes moved. Drives the progress percentage, the
    /// throughput metric and the byte metrics, so a task calling this need do
    /// none of them itself.
    void setBytesDone(qint64 done);
    void setProgress(int percent);
    /// The line under the task's title. Coalesced on the worker thread, so a
    /// caller may write one per entry of a walk; whatever it writes last is
    /// always what the row ends up showing.
    void setStatusText(const QString& text);
    /// Mark the task failed. The first failure wins.
    void fail(const VfsError& error);

    /// Asks the drawing thread to empty the box, unless it has already been
    /// asked and not yet got to it. Called from the worker thread, and available
    /// to a subclass with a box of its own -- see drainPayload().
    void scheduleDrain();
    /// Empties whatever the subclass has been collecting, on the drawing thread
    /// and on the same schedule as the status line and the metrics.
    ///
    /// A task with something of its own to announce -- a duplicate group, a
    /// match, a row -- has the same problem the status line had: one queued
    /// event per item is bounded by nothing but the speed of the worker, and the
    /// window never gets a frame in. Filling a box here and emptying it from
    /// this hook gets the bound for free: at most one event outstanding, at most
    /// one every kDrainIntervalMs, carrying however much arrived in between.
    virtual void drainPayload() { }

private:
    void setState(State state);
    /// Empties the box if the window is open, and asks to be called again when
    /// it opens if it is not. Runs on the drawing thread.
    void drainReports();
    /// Empties the box, whatever the clock says. Drawing thread.
    void applyPending();
    /// Empties the box once the task body has returned, ahead of the state
    /// change: a row that has stopped must not be left showing a line -- or a
    /// count -- from the middle of its run. Called from the worker thread.
    void flushReports();
    /// Publishes how long is left, from the smoothed rate and the total. Called
    /// from setBytesDone() so every task that measures bytes gets it at once
    /// rather than each one growing its own arithmetic.
    void reportTimeLeft(qint64 done);

    /// How many rate windows have to close before an estimate is offered. Three
    /// windows of 250 ms, so the first figure is never one taken from the opening
    /// half second -- where a copy is still spinning up and the estimate is wrong
    /// by multiples rather than by percent. A wrong estimate is worse than none:
    /// it is read once, believed, and remembered.
    static constexpr int kSettledRateSamples = 3;

    // --- owned by the UI thread ---
    bool m_background = false;
    bool m_oneOfMany = false;
    /// Fixed when the task is built, so it is safe to read from any thread.
    QList<VfsUri> m_touching;
    /// The drive, as a key and never dereferenced. Fixed when the task is built.
    const void* m_lane = nullptr;
    QDateTime m_startedAt;
    QDateTime m_finishedAt;
    /// How long the task has been going, measured with a clock that cannot go
    /// backwards. The two QDateTimes above say *when* it started and finished,
    /// which is what a person wants to read; a duration taken by subtracting
    /// them is a duration that changes when the machine's clock is corrected,
    /// and a rate computed from one can come out negative or infinite.
    QElapsedTimer m_since;
    qint64 m_finishedElapsedMs = -1;
    /// Keyed by TaskMetric::key. Owned by the UI thread.
    QMap<QString, TaskMetric> m_metrics;
    QString m_id;
    QString m_title;
    State m_state = State::Pending;
    int m_progress = -1;
    QString m_statusText;

    // --- owned by the worker thread running run() ---
    // Last values we bothered to hand over. Keeping them here is what lets a
    // scan call these per file without flooding the event queue: an update that
    // says nothing new is dropped before it costs anything.
    //
    // setProgress() posts an event of its own and needs no box and no clock,
    // and must not grow either: a percentage has a hundred and one distinct
    // values however many files there are, so the value check is already a
    // bound. A status line with a running total in it has as many distinct
    // values as the walk has entries, and a metric carries a number that
    // changes on every chunk, which is why those two go through the box.
    int m_lastPostedProgress = -2;
    QString m_lastPostedStatus;
    /// What the drawing thread has been handed, by key, so a report carrying
    /// nothing new is dropped before it reaches the box.
    QMap<QString, TaskMetric> m_lastPostedMetrics;
    VfsError m_error;

    // --- the box: filled by the worker thread, emptied by the drawing one ---
    //
    // A shared box under a mutex rather than an event per update, because there
    // must be a bound on what the drawing thread has to get through, and a
    // queue of events is bounded by nothing but the speed of the walk. What is
    // in the box is always the latest of everything, so a drain that arrives
    // late costs nothing but the readings nobody could have read anyway.
    mutable QMutex m_pendingGuard;
    std::optional<QString> m_pendingStatus;
    QMap<QString, TaskMetric> m_pendingMetrics;
    /// Whether a drain is already on its way. One at a time is the mechanism:
    /// however fast the worker reports, this object never has more than a
    /// single event of its own waiting in front of the window.
    std::atomic<bool> m_drainScheduled { false };
    /// When the box was last emptied. Drawing thread only.
    qint64 m_lastDrainMs = -1;

    // --- the byte counters, written by the worker and read from anywhere ---
    //
    // Atomic because they are the one part of a task's reporting that a worker
    // reads back -- a progress bar wants the window's copy, but a copy loop
    // accumulating a total wants its own. Relaxed ordering throughout: each is a
    // single value that stands alone, and nothing is published through them.
    std::atomic<qint64> m_bytesDone { -1 };
    std::atomic<double> m_bytesPerSecond { 0.0 };

    // --- throughput sampling, touched only by the worker thread ---
    std::atomic<qint64> m_byteTotal { -1 };
    qint64 m_lastSampleBytes = 0;
    /// Minus one rather than zero, because zero is a moment the monotonic clock
    /// really can read: a task sampled in the millisecond it started would
    /// otherwise look like one that has never been sampled at all, and its rate
    /// would never be worked out.
    qint64 m_lastSampleMs = -1;
    qint64 m_lastReportMs = -1;
    double m_rate = 0.0;
    /// How many rate windows have closed. An estimate wants more than one.
    int m_rateSamples = 0;
    /// The last estimate worth showing, in milliseconds; -1 for "not known".
    /// Kept so a stall holds the last figure rather than publishing infinity.
    double m_timeLeftMs = -1.0;

    CancelToken m_cancel;
};

} // namespace mole
