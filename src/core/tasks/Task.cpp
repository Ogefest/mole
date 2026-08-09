#include "core/tasks/Task.h"

#include "core/diagnostics/Diagnostics.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
#include <QUuid>

#include <algorithm>

namespace mole {

Task::Task(QString title, QObject* parent)
    : QObject(parent)
    , m_id(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_title(std::move(title))
{
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
    run();
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
    if (m_error.isError()) {
        qCWarning(taskLog, "%s failed: %s", qPrintable(m_title), qPrintable(m_error.message));
    }

    // Queued so the UI observes finished() after the final state change.
    QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
}

void Task::setProgress(int percent)
{
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
    if (!m_startedAt.isValid())
        return 0;
    const QDateTime end = m_finishedAt.isValid() ? m_finishedAt : QDateTime::currentDateTime();
    return m_startedAt.msecsTo(end);
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
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastSampleMs == 0) {
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
    if (finalCall || m_lastReportMs == 0 || nowMs - m_lastReportMs >= 100) {
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
            emit stateChanged();
        },
        Qt::QueuedConnection);
}

} // namespace mole
