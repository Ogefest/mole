#include "core/tasks/Task.h"

#include "core/diagnostics/Diagnostics.h"
#include "core/text/SizeWords.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
#include <QMutexLocker>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <exception>

namespace mole {

Task::Task(QString title, QObject* parent)
    : QObject(parent)
    , m_id(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_title(std::move(title))
{
    // Started here rather than when the task begins running, so a queued task
    // has a clock to measure from the moment anybody can ask it anything.
    m_since.start();
}

Task::~Task() = default;

bool Task::isFinished() const
{
    return m_state == State::Succeeded || m_state == State::Failed || m_state == State::Cancelled;
}

void Task::requestCancel()
{
    m_cancel.cancel();
}

void Task::execute()
{
    // Answered here rather than by run(). A cancel that arrived while this was
    // queued used to reach a task that had already entered run() and blocked in
    // the kernel on its first list() -- an uninterruptible call on a mount that
    // has stopped answering -- so cancelling the queue freed no pool thread at
    // all. The token is cooperative and this is the first place it can be
    // co-operated with. See MOLE-362.
    if (m_cancel.isCancelled()) {
        setState(State::Cancelled);
        qCDebug(
            taskLog, "%s [%s]: cancelled before it started", qPrintable(m_title), qPrintable(m_id.left(8)));
        QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
        return;
    }

    setState(State::Running);

    // Here rather than in each task, so a scan, a copy, a rename and whatever is
    // written next all announce themselves the same way and a log can be read
    // without knowing which task wrote which line.
    //
    // At info for a job somebody would look for afterwards, at debug for anything
    // else. ADR-0012 put both at debug, on the argument that the browser cancels a
    // listing on every keystroke and a line apiece would bury the copy the log was
    // opened to find. That argument is sound and it only covers the crowd: a listing,
    // a thumbnail, a metadata read and a ranged read come by the hundred, and a copy
    // or a scan does not. So the two are told apart rather than both silenced, and a
    // session log from an ordinary run finally says what ran. See ADR-0064.
    //
    // Both questions, not just the crowd. Housekeeping is not something anybody looks
    // for either: with only isOneOfMany() consulted, a plain eight-second run wrote
    // forty-two lines of which thirty were `Check free space on …`, because
    // QuerySpaceTask runs per mount every minute for ever. That is the same burial by
    // a different door.
    const bool loud = !isOneOfMany() && !isBackground();
    if (loud)
        qCInfo(taskLog, "%s [%s]: started", qPrintable(m_title), qPrintable(m_id.left(8)));
    else
        qCDebug(taskLog, "%s [%s]: started", qPrintable(m_title), qPrintable(m_id.left(8)));

    QElapsedTimer clock;
    clock.start();
    // A task body is other people's code -- a backend, a plugin, whatever a
    // library throws on its way out. An exception reaching the thread pool ends
    // the process, and the job it was running disappears with no record of why,
    // so it becomes a failure like any other here.
    try {
        run();
    } catch (const std::exception& problem) {
        fail(VfsError::make(VfsError::Unknown,
            QStringLiteral("%1 stopped unexpectedly: %2").arg(m_title, QString::fromUtf8(problem.what()))));
    } catch (...) {
        fail(VfsError::make(VfsError::Unknown, QStringLiteral("%1 stopped unexpectedly").arg(m_title)));
    }
    const qint64 elapsed = clock.elapsed();

    // Before the state change, so anything watching finished() reads the line
    // and the counts the task ended on rather than the ones the clock happened
    // to let through.
    flushReports();

    // Cancelled means the run stopped *because* it was cancelled: either it
    // asked the token and acted on the answer, or a backend it called answered
    // Cancelled. The token alone is not enough -- a cancel landing after the
    // last poll used to turn a finished task into a cancelled one, and a move
    // that had already deleted its source reported "cancelled" over a copy that
    // was complete. See MOLE-359.
    const bool stoppedForACancel = m_cancel.wasNoticed() || m_error.code == VfsError::Cancelled;

    const char* outcome = "finished";
    if (stoppedForACancel) {
        setState(State::Cancelled);
        outcome = "cancelled";
    } else if (m_error.isError()) {
        setState(State::Failed);
        outcome = "failed";
    } else {
        setState(State::Succeeded);
    }

    if (loud) {
        qCInfo(taskLog, "%s [%s]: %s after %lld ms -- %s", qPrintable(m_title), qPrintable(m_id.left(8)),
            outcome, elapsed, qPrintable(m_lastPostedStatus));
    } else {
        qCDebug(taskLog, "%s [%s]: %s after %lld ms -- %s", qPrintable(m_title), qPrintable(m_id.left(8)),
            outcome, elapsed, qPrintable(m_lastPostedStatus));
    }

    // A task that failed is worth a line whether anyone asked for logging or
    // not: it is the thing the user will be asking about later.
    //
    // Cancellation is not that. The browser cancels a listing every time the
    // folder changes or a keystroke narrows a filter, so a warning apiece
    // buries the lines somebody opened the log to find. It is already accounted
    // for in the outcome line above, along with the rest of what the task did.
    if (m_error.isError() && m_error.code != VfsError::Cancelled) {
        qCWarning(taskLog, "%s failed: %s", qPrintable(m_title), qPrintable(m_error.message));
    }

    // Queued so the UI observes finished() after the final state change.
    QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
}

void Task::setProgress(int percent)
{
    // A bar is a promise about how much is left, and two things break it: a
    // figure past the end, and one that slides back. Both are arithmetic
    // accidents rather than intentions -- a total that turned out to be wrong, a
    // count restarted for the next stage -- and neither is worth showing.
    // Minus one is left alone: it is the indeterminate state, not a percentage.
    if (percent != kIndeterminateProgress) {
        percent = std::clamp(percent, 0, 100);
        if (m_lastPostedProgress != kIndeterminateProgress && percent < m_lastPostedProgress)
            return;
    }

    // Called once per processed item on hot paths, so drop no-op updates
    // before they ever reach the event queue. This needs no box and no clock,
    // and must not grow either: a percentage has a hundred and one distinct
    // values however many files there are, so the check below is already a bound
    // on what the drawing thread can be asked to do. The status line and the
    // metrics carry running totals, which is why those two go through the box.
    if (percent == m_lastPostedProgress)
        return;
    m_lastPostedProgress = percent;

    QMetaObject::invokeMethod(
        this,
        [this, percent] {
            if (m_progress == percent)
                return;
            m_progress = percent;
            emit progressChanged();
        },
        Qt::QueuedConnection);
}

void Task::setStatusText(const QString& text)
{
    // The only check that ever stood between a caller and the event queue, and
    // a status line with a running total in it defeats it: the text differs on
    // every entry, so every call used to post. A metadata walk answers tens of
    // thousands of entries a second, and each post cost the drawing thread a
    // dataChanged on the task list, a delegate update and a text relayout -- so
    // the queue grew for as long as the walk ran and the window never got a
    // frame in. What follows is why there is a box.
    if (text == m_lastPostedStatus)
        return;
    m_lastPostedStatus = text;

    {
        const QMutexLocker locked(&m_pendingGuard);
        m_pendingStatus = text;
    }
    scheduleDrain();
}

void Task::scheduleDrain()
{
    // At most one of these outstanding at a time. That is the bound: whatever
    // the worker's rate, the window has one event of ours to get through, and
    // it carries whatever the box holds by the time it is read rather than a
    // reading from when it was posted.
    if (!m_drainScheduled.exchange(true))
        QMetaObject::invokeMethod(this, [this] { drainReports(); }, Qt::QueuedConnection);
}

void Task::drainReports()
{
    const qint64 nowMs = elapsedNow();
    const qint64 sinceLast = m_lastDrainMs < 0 ? kDrainIntervalMs : nowMs - m_lastDrainMs;
    if (sinceLast < kDrainIntervalMs) {
        // Too soon after the last one to be worth a repaint. Come back when the
        // window opens, and leave the flag standing while we wait: a second
        // wake-up queued behind this one would buy nothing.
        QTimer::singleShot(kDrainIntervalMs - sinceLast, this, [this] { drainReports(); });
        return;
    }

    // Cleared before the box is read, never after. A reading written between the
    // two is one the worker will ask for another drain for; a reading written
    // after the clear and before the lock is one this drain still picks up. The
    // other order is how the last reading of a task that then goes quiet gets
    // left in the box for ever.
    m_drainScheduled.store(false);
    applyPending();
}

void Task::applyPending()
{
    std::optional<QString> status;
    QMap<QString, TaskMetric> metrics;
    {
        const QMutexLocker locked(&m_pendingGuard);
        status.swap(m_pendingStatus);
        metrics.swap(m_pendingMetrics);
    }
    m_lastDrainMs = elapsedNow();

    if (status && m_statusText != *status) {
        m_statusText = *status;
        emit statusTextChanged();
    }
    if (!metrics.isEmpty()) {
        for (const TaskMetric& metric : std::as_const(metrics))
            m_metrics.insert(metric.key, metric);
        // One signal for the lot: a strip that redraws once for four figures
        // that arrived together is a strip that redraws once.
        emit metricsChanged();
    }

    // Whatever the subclass has been collecting travels on the same drain, and
    // therefore under the same bound.
    drainPayload();
}

void Task::flushReports()
{
    // Queued from the worker thread before the state change is, so a reader
    // watching finished() sees the line and the counts the task ended on. The
    // scheduled drain may be most of kDrainIntervalMs away, and a row that has
    // stopped must not be left holding a figure from the middle of the run.
    QMetaObject::invokeMethod(this, [this] { applyPending(); }, Qt::QueuedConnection);
}

void Task::fail(const VfsError& error)
{
    if (m_error.isError())
        return; // first failure wins
    m_error = error;
    setStatusText(error.message);
}

void Task::setElapsedSource(std::function<qint64()> source)
{
    m_elapsedSource = std::move(source);
}

qint64 Task::elapsedNow() const
{
    if (m_elapsedSource)
        return m_elapsedSource();
    return m_since.isValid() ? m_since.elapsed() : 0;
}

qint64 Task::elapsedMs() const
{
    // From the monotonic clock rather than from the two timestamps. Subtracting
    // wall-clock readings is how a job that ran for a minute reports minus
    // fifty-nine when ntp steps the clock, and how a rate comes out negative.
    if (!m_since.isValid() && !m_elapsedSource)
        return 0;
    return m_finishedElapsedMs >= 0 ? m_finishedElapsedMs : elapsedNow();
}

QString TaskMetric::format(double value, Kind kind)
{
    switch (kind) {
    case Kind::Count:
        return QLocale().toString(static_cast<qlonglong>(value));
    case Kind::Bytes:
        return sizeInWords(static_cast<qint64>(value));
    case Kind::Rate:
        return value <= 0.0 ? QString() : rateInWords(value);
    case Kind::Duration: {
        // A negative duration is "not known" rather than a number: an estimate
        // before the rate has settled, or after there is nothing left to
        // estimate. Empty, so the view leaves no column instead of printing
        // something nobody can act on.
        if (value < 0.0)
            return {};
        const qint64 seconds = static_cast<qint64>(value) / 1000;
        if (seconds < 60)
            return QStringLiteral("%1s").arg(seconds);
        if (seconds < 3600)
            return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
        return QStringLiteral("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
    }
    case Kind::Text:
        break;
    }
    return {};
}

QList<TaskMetric> Task::metrics() const
{
    QList<TaskMetric> out = m_metrics.values();
    std::sort(out.begin(), out.end(), [](const TaskMetric& a, const TaskMetric& b) {
        if (a.order != b.order)
            return a.order < b.order;
        return a.key < b.key;
    });
    return out;
}

TaskMetric Task::metric(const QString& key) const
{
    return m_metrics.value(key);
}

qint64 Task::bytesDone() const
{
    return m_bytesDone.load(std::memory_order_relaxed);
}

qint64 Task::bytesTotal() const
{
    return m_byteTotal.load(std::memory_order_relaxed);
}

double Task::bytesPerSecond() const
{
    return m_bytesPerSecond.load(std::memory_order_relaxed);
}

namespace {

    /// Whether two readings of the same metric say the same thing to a reader.
    bool saysTheSame(const TaskMetric& a, const TaskMetric& b)
    {
        return a.text == b.text && qFuzzyCompare(a.value + 1.0, b.value + 1.0);
    }

} // namespace

void Task::report(TaskMetric metric)
{
    if (metric.key.isEmpty())
        return;
    if (metric.text.isEmpty())
        metric.text = TaskMetric::format(metric.value, metric.kind);

    // Decided here rather than inside a queued lambda, where it used to be: by
    // then the event had been posted and delivered, so the check saved a signal
    // and none of the cost of getting there -- which is what the comment on
    // report() promised and did not do. The same fault as the status line, one
    // door along: a rename publishes the number renamed per entry and a sync the
    // number applied per step, and that number differs on every call.
    const auto handed = m_lastPostedMetrics.constFind(metric.key);
    if (handed != m_lastPostedMetrics.constEnd() && saysTheSame(*handed, metric))
        return; // nothing a reader could see has changed
    m_lastPostedMetrics.insert(metric.key, metric);

    {
        // Keyed, so the box holds the latest reading of each metric rather than
        // the latest reading of one of them.
        const QMutexLocker locked(&m_pendingGuard);
        m_pendingMetrics.insert(metric.key, std::move(metric));
    }
    scheduleDrain();
}

void Task::reportCount(const QString& key, const QString& label, double value, int order)
{
    report(TaskMetric { key, label, {}, value, TaskMetric::Kind::Count, order });
}

void Task::reportBytes(const QString& key, const QString& label, qint64 bytes, int order)
{
    report(TaskMetric { key, label, {}, static_cast<double>(bytes), TaskMetric::Kind::Bytes, order });
}

void Task::reportText(const QString& key, const QString& label, const QString& text, int order)
{
    report(TaskMetric { key, label, text, 0.0, TaskMetric::Kind::Text, order });
}

void Task::setByteTotal(qint64 total)
{
    m_byteTotal = total;
    reportBytes(TaskMetrics::kBytesTotal, QStringLiteral("Total"), total, 20);
}

void Task::setBytesDone(qint64 done)
{
    // A short window, sampled no more often than a few times a second: any
    // faster and the figure jitters between chunk boundaries, any slower and a
    // stall takes too long to become visible.
    // Monotonic, for the same reason elapsedMs() is: a clock stepped backwards
    // between two samples would make the interval negative and the rate with it.
    // First, and outside every window below. What follows coalesces *reports* --
    // the queued events that reach the window -- and that throttling must not
    // reach the figure a worker asks for.
    m_bytesDone.store(done, std::memory_order_relaxed);

    const qint64 nowMs = elapsedNow();
    if (m_lastSampleMs < 0) {
        m_lastSampleMs = nowMs;
        m_lastSampleBytes = done;
    } else if (nowMs - m_lastSampleMs >= 250) {
        const double seconds = static_cast<double>(nowMs - m_lastSampleMs) / 1000.0;
        const double rate = static_cast<double>(done - m_lastSampleBytes) / seconds;
        // Smoothed, or the number is unreadable while it changes every frame.
        m_rate = m_rate > 0.0 ? m_rate * 0.6 + rate * 0.4 : rate;
        m_bytesPerSecond.store(m_rate, std::memory_order_relaxed);
        m_lastSampleMs = nowMs;
        m_lastSampleBytes = done;
        ++m_rateSamples;
        report(TaskMetric {
            TaskMetrics::kRate, QStringLiteral("Speed"), {}, m_rate, TaskMetric::Kind::Rate, 30 });
        // Beside the rate, from the same window: a task that knows its speed and
        // its size knows how long is left, and every one of them should say so.
        reportTimeLeft(done);
    }

    // Coalesced: a chunk loop calls this thousands of times, and every call
    // would otherwise be a queued event carrying a number nobody can read that
    // fast.
    const bool finalCall = m_byteTotal > 0 && done >= m_byteTotal;
    if (finalCall || m_lastReportMs < 0 || nowMs - m_lastReportMs >= 100) {
        m_lastReportMs = nowMs;
        reportBytes(TaskMetrics::kBytesDone, QStringLiteral("Copied"), done, 10);
        if (m_byteTotal > 0) {
            setProgress(
                static_cast<int>(100.0 * static_cast<double>(done) / static_cast<double>(m_byteTotal)));
        }
        // Whether or not a rate window happened to close on the last chunk: a row
        // that has stopped must not be left holding an estimate.
        if (finalCall)
            reportTimeLeft(done);
    }
}

void Task::reportTimeLeft(qint64 done)
{
    // Nothing to divide with. A task that never declared a total has no
    // denominator, and one whose rate has not settled would publish a figure
    // wrong by multiples -- see kSettledRateSamples.
    if (m_byteTotal <= 0 || m_rateSamples < kSettledRateSamples)
        return;

    const qint64 remaining = m_byteTotal - done;
    if (remaining <= 0) {
        // Finished, or as good as. "0s left" on a row that has stopped is noise,
        // and the figure it had a moment ago would be worse.
        m_timeLeftMs = -1.0;
    } else if (m_rate > 1.0) {
        m_timeLeftMs = 1000.0 * static_cast<double>(remaining) / m_rate;
    }
    // Otherwise a stall, and the last estimate stands. Dividing by a rate on its
    // way to zero gives a figure that runs off to hours and then to infinity,
    // which is less honest than the last one that meant something.

    report(TaskMetric {
        TaskMetrics::kTimeLeft, QStringLiteral("Left"), {}, m_timeLeftMs, TaskMetric::Kind::Duration, 35 });
}

void Task::setState(State state)
{
    QMetaObject::invokeMethod(
        this,
        [this, state] {
            if (m_state == state)
                return;
            m_state = state;
            if (state == State::Running && !m_startedAt.isValid())
                m_startedAt = QDateTime::currentDateTime();
            // Stamped on the UI thread with the rest of the state, so the
            // retention sweep and the list always agree about when this ended.
            if (isFinished() && !m_finishedAt.isValid())
                m_finishedAt = QDateTime::currentDateTime();
            if (isFinished() && m_finishedElapsedMs < 0 && (m_since.isValid() || m_elapsedSource))
                m_finishedElapsedMs = elapsedNow();
            emit stateChanged();
        },
        Qt::QueuedConnection);
}

} // namespace mole
