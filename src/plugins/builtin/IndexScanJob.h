#pragma once

#include "sdk/PluginServices.h"

#include "core/automation/Scheduler.h"

#include <QObject>

namespace mole {

/// Re-indexes a volume on a clock, without anybody remembering to.
///
/// An index is only ever as fresh as the last time somebody started a scan by
/// hand, and for the volume it matters most on -- the 4 TB tree that takes an
/// hour to walk -- that is "not very". This is the answer to staleness the
/// search deliberately does not have: nothing anywhere judges whether an index
/// is too old to trust, because a folder that changes often is one somebody
/// indexes often. That makes the schedule the control that matters.
///
/// Incremental by default, so a nightly run over an unchanged tree costs a walk
/// of what moved rather than of everything.
class IndexScanJob : public QObject, public IScheduledJob
{
    Q_OBJECT

public:
    /// The job kind these rules carry. Public because the search form makes
    /// rules for it.
    static QString kind() { return QStringLiteral("index"); }
    /// Which folder to re-index.
    static QString rootUriParameter() { return QStringLiteral("rootUri"); }
    /// False for the occasional full rescan, when somebody suspects the index.
    static QString incrementalParameter() { return QStringLiteral("incremental"); }
    /// Whether the scan also records what each file says about itself.
    static QString metadataParameter() { return QStringLiteral("metadata"); }

    explicit IndexScanJob(PluginServices services, QObject* parent = nullptr);

    QString displayName() const override { return QStringLiteral("Re-index a folder"); }
    bool start(const ScheduleRule& rule, std::function<void(bool, QString)> done) override;

signals:
    /// A scan the scheduler ran has finished, so an open search can refresh its
    /// volume list rather than showing what was there before.
    void volumeScanned(const QString& rootUri);

private:
    PluginServices m_services;
};

} // namespace mole
