#pragma once

#include "core/alerts/AlertRule.h"
#include "core/tasks/Task.h"

#include <QList>

namespace mole {

class VfsManager;
class AnalysisStore;

/// Checks alerts on a pool thread.
///
/// Measuring can mean walking a whole tree, so this never happens on the UI
/// thread. Results come back as a list the caller writes into the store, which
/// keeps every mutation of the store on one thread.
class CheckAlertsTask final : public Task
{
    Q_OBJECT

public:
    CheckAlertsTask(
        VfsManager* vfs, AnalysisStore* analysis, QList<AlertRule> rules, QObject* parent = nullptr);

    /// The rules with their new state. Valid once finished() has arrived.
    QList<AlertRule> results() const { return m_results; }

protected:
    void run() override;

private:
    VfsManager* m_vfs = nullptr;
    AnalysisStore* m_analysis = nullptr;
    QList<AlertRule> m_rules;
    QList<AlertRule> m_results;
};

} // namespace mole
