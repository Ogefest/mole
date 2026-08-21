#pragma once

#include "core/index/IndexDatabase.h"
#include "core/tasks/Task.h"

namespace mole {

/// Throws away one indexed volume and everything recorded under it.
///
/// A task rather than a direct call for the reason every other index write is
/// already one: writers are serialised (ADR-0065), so `removeVolume()` waits for
/// whatever transaction a scan has open, and `DELETE FROM files WHERE volume_id`
/// over a volume with hundreds of thousands of rows is not instant either. Doing
/// it on the thread that draws the window is the same fault ADR-0066 fixed for
/// reads, and it was the last one left. See MOLE-274.
///
/// Not background work: forgetting an index is something somebody asked for and
/// is entitled to see on the task strip.
class ForgetVolumeTask final : public Task
{
    Q_OBJECT

public:
    ForgetVolumeTask(IndexDatabase* index, qint64 volumeId, QString label, QObject* parent = nullptr);

signals:
    /// Emitted on the UI thread when the volume has actually gone, or has not.
    ///
    /// `reason` is empty on success. The row stays until this arrives, because a
    /// row that disappears when the removal was *asked for* tells the user the
    /// index is gone before anybody knows whether it is.
    void removed(bool ok, const QString& reason);

protected:
    void run() override;

private:
    IndexDatabase* m_index = nullptr;
    qint64 m_volumeId = -1;
};

} // namespace mole
