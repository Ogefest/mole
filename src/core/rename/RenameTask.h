#pragma once

#include "core/rename/RenamePlan.h"
#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

class VfsManager;

/// Carries out a rename plan.
///
/// Only ever given a plan that has already been checked: the collision rules
/// live in RenamePlan, so this cannot start a batch that would half-succeed.
/// Anything that still fails -- a file removed between the preview and the
/// commit -- is recorded and the rest continues, because stopping halfway would
/// leave the batch in the state the checking exists to avoid.
class RenameTask final : public Task
{
    Q_OBJECT

public:
    RenameTask(VfsManager* vfs, QList<RenamePlan::Entry> entries, QObject* parent = nullptr);

    int renamedCount() const { return m_renamed; }
    QStringList failures() const { return m_failures; }

protected:
    void run() override;

private:
    VfsManager* m_vfs = nullptr;
    QList<RenamePlan::Entry> m_entries;
    int m_renamed = 0;
    QStringList m_failures;
};

} // namespace mole
