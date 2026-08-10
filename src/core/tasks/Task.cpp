#include "core/tasks/Task.h"

#include "core/diagnostics/Diagnostics.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
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
    setState(State::Running);

    // Here rather than in each task, so a scan, a copy, a rename and whatever is
    // written next all announce themselves the same way and a log can be read
    // without knowing which task wrote which line.
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

    const char* outcome = "finished";
    if (m_cancel.isCancelled()) {
        setState(State::Cancelled);
        outcome = "cancelled";
    } else if (m_error.isError()) {
        setState(State::Failed);
        outcome = "failed";
    } else {
        setState(State::Succeeded);
    }

    qCDebug(taskLog, "%s [%s]: %s after %lld ms -- %s", qPrintable(m_title), qPrintable(m_id.left(8)),
        outcome, elapsed, qPrintable(m_lastPostedStatus));

    // A task that failed is worth a line whether anyone asked for logging or
    // not: it is the thing the user will be asking about later.
    //
    // Cancellation is not that. The browser cancels a listing every time the
    // folder changes or a keystroke narrows a filter, so a warning apiece
    // buries the lines somebody opened the log to find. It is already accounted
    // for in the debug line above, along with the rest of what the task did.
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
    // before they ever reach the event queue.
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
    if (text == m_lastPostedStatus)
        return;
    m_lastPostedStatus = text;

    QMetaObject::invokeMethod(
        this,
        [this, text] {
            if (m_statusText == text)
                return;
            m_statusText = text;
            emit statusTextChanged();
        },
        Qt::QueuedConnection);
}

void Task::fail(const VfsError& error)
{
    if (m_error.isError())
        return; // first failure wins
    m_error = error;
    setStatusText(error.message);
}

qint64 Task::elapsedMs() const
{
    // From the monotonic clock rather than from the two timestamps. Subtracting
    // wall-clock readings is how a job that ran for a minute reports minus
    // fifty-nine when ntp steps the clock, and how a rate comes out negative.
    if (!m_since.isValid())
        return 0;
    return m_finishedElapsedMs >= 0 ? m_finishedElapsedMs : m_since.elapsed();
}

QString TaskMetric::format(double value, Kind kind)
{
    switch (kind) {
    case Kind::Count:
        return QLocale().toString(static_cast<qlonglong>(value));
    case Kind::Bytes:
        return QLocale().formattedDataSize(static_cast<qint64>(value));
    case Kind::Rate:
        return value <= 0.0
            ? QString()
            : QStringLiteral("%1/s").arg(QLocale().formattedDataSize(static_cast<qint64>(value)));
    case Kind::Duration: {
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
    const auto position = m_metrics.constFind(TaskMetrics::kBytesDone);
    return position == m_metrics.constEnd() ? -1 : static_cast<qint64>(position->value);
}

qint64 Task::bytesTotal() const
{
    const auto position = m_metrics.constFind(TaskMetrics::kBytesTotal);
    return position == m_metrics.constEnd() ? -1 : static_cast<qint64>(position->value);
}

double Task::bytesPerSecond() const
{
    const auto position = m_metrics.constFind(TaskMetrics::kRate);
    return position == m_metrics.constEnd() ? 0.0 : position->value;
}

void Task::report(TaskMetric metric)
{
    if (metric.key.isEmpty())
        return;
    if (metric.text.isEmpty())
        metric.text = TaskMetric::format(metric.value, metric.kind);

    // Posted to the UI thread like progress and status, for the same reason:
    // the metric map belongs to whoever reads it, and that is not this thread.
    QMetaObject::invokeMethod(
        this,
        [this, metric = std::move(metric)] {
            const auto existing = m_metrics.constFind(metric.key);
            if (existing != m_metrics.constEnd() && existing->text == metric.text
                && qFuzzyCompare(existing->value + 1.0, metric.value + 1.0)) {
                return; // nothing a reader could see has changed
            }
            m_metrics.insert(metric.key, metric);
            emit metricsChanged();
        },
        Qt::QueuedConnection);
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
    const qint64 nowMs = m_since.isValid() ? m_since.elapsed() : 0;
    if (m_lastSampleMs < 0) {
        m_lastSampleMs = nowMs;
        m_lastSampleBytes = done;
    } else if (nowMs - m_lastSampleMs >= 250) {
        const double seconds = static_cast<double>(nowMs - m_lastSampleMs) / 1000.0;
        const double rate = static_cast<double>(done - m_lastSampleBytes) / seconds;
        // Smoothed, or the number is unreadable while it changes every frame.
        m_rate = m_rate > 0.0 ? m_rate * 0.6 + rate * 0.4 : rate;
        m_lastSampleMs = nowMs;
        m_lastSampleBytes = done;
        report(TaskMetric {
            TaskMetrics::kRate, QStringLiteral("Speed"), {}, m_rate, TaskMetric::Kind::Rate, 30 });
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
    }
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
            if (isFinished() && m_finishedElapsedMs < 0 && m_since.isValid())
                m_finishedElapsedMs = m_since.elapsed();
            emit stateChanged();
        },
        Qt::QueuedConnection);
}

} // namespace mole
