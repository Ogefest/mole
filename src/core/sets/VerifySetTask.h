#pragma once

#include "core/tasks/Task.h"

#include <QHash>
#include <QStringList>

namespace mole {

class VfsManager;

/// Checks whether each member of a set is still there.
///
/// A set outlives the files in it: things get moved, renamed and deleted from
/// under it. Pretending otherwise is how an operation half-fails partway
/// through, having already touched some of its targets.
///
/// One task for the whole set rather than one per member -- five hundred files
/// would otherwise flood the queue with work too small to schedule.
class VerifySetTask final : public Task
{
    Q_OBJECT

public:
    VerifySetTask(VfsManager* vfs, QStringList uris, QObject* parent = nullptr);

signals:
    /// Delivered on the UI thread. Keyed by uri; a member absent from `sizes`
    /// is one whose drive could not say.
    void verified(const QHash<QString, bool>& present, const QHash<QString, qint64>& sizes);

protected:
    void run() override;

private:
    VfsManager* m_vfs = nullptr;
    QStringList m_uris;
};

} // namespace mole
