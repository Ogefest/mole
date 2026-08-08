#include "plugins/builtin/AutomationFeature.h"

#include "core/automation/ScheduleStore.h"

namespace mole {
namespace {

    /// "in 4 hours", "3 days ago", "just now" -- a schedule is read as a distance
    /// from now far more often than as a date.
    QString relativeTime(const QDateTime& when, const QDateTime& now)
    {
        if (!when.isValid())
            return QStringLiteral("never");

        const qint64 seconds = now.secsTo(when);
        const qint64 magnitude = std::llabs(seconds);
        const bool future = seconds > 0;

        QString amount;
        if (magnitude < 60)
            return future ? QStringLiteral("in a moment") : QStringLiteral("just now");
        else if (magnitude < 3600)
            amount = QStringLiteral("%1 min").arg(magnitude / 60);
        else if (magnitude < 86400)
            amount = QStringLiteral("%1 h").arg(magnitude / 3600);
        else
            amount = QStringLiteral("%1 d").arg(magnitude / 86400);

        return future ? QStringLiteral("in %1").arg(amount) : QStringLiteral("%1 ago").arg(amount);
    }

    QString statusLabel(RunStatus status)
    {
        switch (status) {
        case RunStatus::Never:
            return QStringLiteral("Not run yet");
        case RunStatus::Running:
            return QStringLiteral("Running");
        case RunStatus::Succeeded:
            return QStringLiteral("OK");
        case RunStatus::Failed:
            return QStringLiteral("Failed");
        case RunStatus::Skipped:
            return QStringLiteral("Skipped");
        }
        return {};
    }

} // namespace

AutomationController::AutomationController(ScheduleStore* store, Scheduler* scheduler, QObject* parent)
    : FeatureController(QStringLiteral("Automation"), parent)
    , m_store(store)
    , m_scheduler(scheduler)
{
    if (m_store) {
        connect(m_store, &ScheduleStore::rulesChanged, this, &AutomationController::refresh);
        connect(m_store, &ScheduleStore::historyChanged, this, &AutomationController::historyChanged);
    }
    if (m_scheduler) {
        connect(m_scheduler, &Scheduler::runStarted, this, &AutomationController::refresh);
        connect(m_scheduler, &Scheduler::runFinished, this, &AutomationController::refresh);
    }

    // "Next run in 3 h" has to keep counting down while the tab is open, and
    // nothing else emits when only the clock has moved.
    m_tick.setInterval(30000);
    connect(&m_tick, &QTimer::timeout, this, &AutomationController::rulesChanged);
    m_tick.start();

    refresh();
}

void AutomationController::refresh()
{
    const int failing = failingCount();
    setSubtitle(failing > 0 ? QStringLiteral("%1 failing").arg(failing)
                            : QStringLiteral("%1 scheduled").arg(m_store ? m_store->rules().size() : 0));
    setBusy(runningCount() > 0);
    emit rulesChanged();
    emit historyChanged();
}

QVariantList AutomationController::rules() const
{
    QVariantList out;
    if (!m_store)
        return out;

    const QDateTime now = QDateTime::currentDateTime();
    const QStringList running = m_scheduler ? m_scheduler->runningRules() : QStringList {};

    QList<ScheduleRule> rules = m_store->rules();
    // Broken first: the reason to open this tab is usually that something
    // stopped working, not to admire the jobs that are fine.
    std::sort(rules.begin(), rules.end(), [](const ScheduleRule& a, const ScheduleRule& b) {
        const bool aBad = a.lastStatus == RunStatus::Failed || a.lastStatus == RunStatus::Skipped;
        const bool bBad = b.lastStatus == RunStatus::Failed || b.lastStatus == RunStatus::Skipped;
        if (aBad != bBad)
            return aBad;
        return a.label.localeAwareCompare(b.label) < 0;
    });

    for (const ScheduleRule& rule : rules) {
        const bool isRunning = running.contains(rule.id);
        out.append(QVariantMap {
            { QStringLiteral("id"), rule.id },
            { QStringLiteral("label"), rule.label },
            { QStringLiteral("jobKind"), rule.jobKind },
            { QStringLiteral("target"), rule.parameters.value(QStringLiteral("rootUri")).toString() },
            { QStringLiteral("intervalSeconds"), QVariant::fromValue(rule.intervalSeconds) },
            { QStringLiteral("intervalText"), ScheduleRule::describeInterval(rule.intervalSeconds) },
            { QStringLiteral("enabled"), rule.enabled },
            { QStringLiteral("running"), isRunning },
            { QStringLiteral("status"),
                isRunning ? QStringLiteral("running") : runStatusToString(rule.lastStatus) },
            { QStringLiteral("statusText"),
                isRunning ? QStringLiteral("Running") : statusLabel(rule.lastStatus) },
            { QStringLiteral("failing"),
                !isRunning
                    && (rule.lastStatus == RunStatus::Failed || rule.lastStatus == RunStatus::Skipped) },
            { QStringLiteral("message"), rule.lastMessage },
            { QStringLiteral("consecutiveFailures"), rule.consecutiveFailures },
            { QStringLiteral("lastRunText"), relativeTime(rule.lastRunAt, now) },
            { QStringLiteral("nextDueText"),
                !rule.enabled ? QStringLiteral("paused")
                              : (!rule.lastRunAt.isValid() ? QStringLiteral("as soon as possible")
                                                           : relativeTime(rule.dueAt(), now)) },
        });
    }
    return out;
}

QVariantList AutomationController::history() const
{
    QVariantList out;
    if (!m_store)
        return out;

    const QDateTime now = QDateTime::currentDateTime();
    const QList<RunRecord> records = m_store->history(m_historyFilter, 200);
    for (const RunRecord& record : records) {
        out.append(QVariantMap {
            { QStringLiteral("ruleId"), record.ruleId },
            { QStringLiteral("label"), record.ruleLabel },
            { QStringLiteral("status"), runStatusToString(record.status) },
            { QStringLiteral("statusText"), statusLabel(record.status) },
            { QStringLiteral("failed"),
                record.status == RunStatus::Failed || record.status == RunStatus::Skipped },
            { QStringLiteral("message"), record.message },
            { QStringLiteral("whenText"), relativeTime(record.startedAt, now) },
            { QStringLiteral("startedAt"), record.startedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) },
            { QStringLiteral("durationText"),
                record.durationMs() < 1000 ? QStringLiteral("%1 ms").arg(record.durationMs())
                                           : QStringLiteral("%1 s").arg(record.durationMs() / 1000) },
        });
    }
    return out;
}

QVariantList AutomationController::intervalPresets() const
{
    QVariantList out;
    const auto presets = ScheduleRule::presets();
    for (const auto& preset : presets) {
        out.append(QVariantMap { { QStringLiteral("label"), preset.first },
            { QStringLiteral("seconds"), QVariant::fromValue(preset.second) } });
    }
    return out;
}

int AutomationController::failingCount() const
{
    if (!m_store)
        return 0;
    int failing = 0;
    const QList<ScheduleRule> rules = m_store->rules();
    for (const ScheduleRule& rule : rules) {
        if (rule.lastStatus == RunStatus::Failed || rule.lastStatus == RunStatus::Skipped)
            ++failing;
    }
    return failing;
}

int AutomationController::runningCount() const
{
    return m_scheduler ? static_cast<int>(m_scheduler->runningRules().size()) : 0;
}

void AutomationController::setHistoryFilter(const QString& filter)
{
    if (m_historyFilter == filter)
        return;
    m_historyFilter = filter;
    emit historyFilterChanged();
    emit historyChanged();
}

bool AutomationController::runNow(const QString& ruleId)
{
    return m_scheduler && m_scheduler->runNow(ruleId);
}

bool AutomationController::setEnabled(const QString& ruleId, bool enabled)
{
    if (!m_store)
        return false;
    ScheduleRule rule = m_store->rule(ruleId);
    if (!rule.isValid())
        return false;
    rule.enabled = enabled;
    return m_store->put(rule);
}

bool AutomationController::setInterval(const QString& ruleId, qint64 seconds)
{
    if (!m_store || seconds < 60)
        return false;
    ScheduleRule rule = m_store->rule(ruleId);
    if (!rule.isValid())
        return false;
    rule.intervalSeconds = seconds;
    return m_store->put(rule);
}

bool AutomationController::rename(const QString& ruleId, const QString& label)
{
    if (!m_store || label.trimmed().isEmpty())
        return false;
    ScheduleRule rule = m_store->rule(ruleId);
    if (!rule.isValid())
        return false;
    rule.label = label.trimmed();
    return m_store->put(rule);
}

bool AutomationController::removeRule(const QString& ruleId)
{
    return m_store && m_store->remove(ruleId);
}

void AutomationController::clearHistory()
{
    if (m_store)
        m_store->clearHistory();
}

QVariantMap AutomationController::saveState() const
{
    return { { QStringLiteral("historyFilter"), m_historyFilter } };
}

void AutomationController::restoreState(const QVariantMap& state)
{
    setHistoryFilter(state.value(QStringLiteral("historyFilter")).toString());
}

AutomationFeature::AutomationFeature(ScheduleStore* store, Scheduler* scheduler)
    : m_store(store)
    , m_scheduler(scheduler)
{
}

QUrl AutomationFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/AutomationView.qml"));
}

FeatureController* AutomationFeature::createController(QObject* parent)
{
    return new AutomationController(m_store, m_scheduler, parent);
}

} // namespace mole
