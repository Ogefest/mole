#pragma once

#include "core/vfs/VfsTypes.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QString>

namespace mole {

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

    /// Bytes moved so far, or -1 when this task does not measure bytes. A
    /// shortcut for the well-known metric, because the progress bar needs it.
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

    /// Safe to call from any thread. Cooperative -- run() must poll.
    Q_INVOKABLE void requestCancel();

    /// Called by TaskManager on a pool thread. Not part of the public API.
    void execute();

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

    /// Publishes or updates a metric. Safe from the worker thread; coalesced,
    /// so a report per chunk does not flood the event queue.
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
    void setStatusText(const QString& text);
    /// Mark the task failed. The first failure wins.
    void fail(const VfsError& error);

private:
    void setState(State state);
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
    // Last values we bothered to post. Keeping them here lets a scan call
    // setProgress() per file without flooding the event queue.
    int m_lastPostedProgress = -2;
    QString m_lastPostedStatus;
    VfsError m_error;

    // --- throughput sampling, touched only by the worker thread ---
    qint64 m_byteTotal = -1;
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
