#pragma once

#include "sdk/PluginServices.h"

#include "core/analysis/AnalysisStore.h"
#include "core/automation/Scheduler.h"

#include <QObject>

namespace mole {

/// Runs a directory report on a schedule, without a tab being open.
///
/// It deliberately shares the store with the analysis tab rather than keeping
/// its own: an automatic run must land in the same history the user compares
/// against by hand, or the two would drift apart and the diffs would lie.
class AnalysisJob : public QObject, public IScheduledJob
{
    Q_OBJECT

public:
    /// The job kind these rules carry. Public because the analysis tab creates
    /// rules for it.
    static QString kind() { return QStringLiteral("analysis"); }
    /// The parameter a rule must carry: which folder to walk.
    static QString rootUriParameter() { return QStringLiteral("rootUri"); }

    AnalysisJob(PluginServices services, AnalysisStore* store, QObject* parent = nullptr);

    QString displayName() const override { return QStringLiteral("Directory report"); }
    StartOutcome start(const ScheduleRule& rule, std::function<void(bool, QString)> done) override;

    /// How many reports a scheduled run keeps per folder.
    ///
    /// Defaults to AnalysisStore::kHistoryKept, which is the analysis tab's
    /// figure too. It used to default to 30 while the tab kept 50 and nothing
    /// ever called this, so a folder with 45 manual runs lost runs 31 to 45 the
    /// first night its schedule fired -- and the header below says the job
    /// "deliberately shares the store with the analysis tab … or the two would
    /// drift apart and the diffs would lie". See MOLE-380.
    void setHistoryKept(int kept) { m_historyKept = kept; }

signals:
    /// A report was filed by the scheduler, so an open tab on the same folder
    /// can pick it up instead of showing yesterday's numbers.
    void reportStored(const QString& rootUri);

private:
    PluginServices m_services;
    AnalysisStore* m_store = nullptr;
    int m_historyKept = AnalysisStore::kHistoryKept;
};

} // namespace mole
