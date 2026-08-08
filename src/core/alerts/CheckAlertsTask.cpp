#include "core/alerts/CheckAlertsTask.h"

#include "core/alerts/AlertEvaluator.h"

namespace mole {

CheckAlertsTask::CheckAlertsTask(
    VfsManager* vfs, AnalysisStore* analysis, QList<AlertRule> rules, QObject* parent)
    : Task(rules.size() == 1 ? QStringLiteral("Check alert: %1").arg(rules.first().label)
                             : QStringLiteral("Check %1 alerts").arg(rules.size()),
          parent)
    , m_vfs(vfs)
    , m_analysis(analysis)
    , m_rules(std::move(rules))
{
}

void CheckAlertsTask::run()
{
    const AlertEvaluator evaluator(m_vfs, m_analysis);
    const QDateTime at = QDateTime::currentDateTime();

    m_results.reserve(m_rules.size());
    int done = 0;

    for (const AlertRule& rule : std::as_const(m_rules)) {
        if (isCancelRequested())
            return;

        setStatusText(rule.label);
        const AlertEvaluator::Reading reading = evaluator.measure(rule, cancelToken());
        m_results.append(AlertEvaluator::apply(rule, reading, at));

        setProgress(static_cast<int>(++done * 100 / std::max<qsizetype>(1, m_rules.size())));
    }

    int triggered = 0;
    for (const AlertRule& rule : std::as_const(m_results)) {
        if (rule.state == AlertState::Triggered)
            ++triggered;
    }
    setStatusText(
        triggered == 0 ? QStringLiteral("All clear") : QStringLiteral("%1 triggered").arg(triggered));
}

} // namespace mole
