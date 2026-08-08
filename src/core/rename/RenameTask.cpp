#include "core/rename/RenameTask.h"

#include "core/vfs/VfsManager.h"

namespace mole {

RenameTask::RenameTask(VfsManager* vfs, QList<RenamePlan::Entry> entries, QObject* parent)
    : Task(QStringLiteral("Rename %1 items").arg(entries.size()), parent)
    , m_vfs(vfs)
    , m_entries(std::move(entries))
{
}

void RenameTask::run()
{
    if (!m_vfs) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("No drives are available")));
        return;
    }

    int done = 0;
    for (const RenamePlan::Entry& entry : std::as_const(m_entries)) {
        if (isCancelRequested())
            return;

        // Unchanged and blocked rows never reach here; the plan filters them.
        FileSystemPtr fs = m_vfs->resolve(entry.source);
        if (!fs) {
            m_failures.append(QStringLiteral("%1: no drive").arg(entry.originalName));
        } else {
            const VfsUri target = entry.source.parent().child(entry.newName);
            if (Result<void> renamed = fs->rename(entry.source, target); !renamed.ok())
                m_failures.append(QStringLiteral("%1: %2").arg(entry.originalName, renamed.error().message));
            else
                ++m_renamed;
        }

        setProgress(static_cast<int>(100.0 * ++done / m_entries.size()));
        reportCount(QStringLiteral("renamed"), QStringLiteral("Renamed"), m_renamed, 10);
        if (!m_failures.isEmpty()) {
            reportCount(QStringLiteral("failed"), QStringLiteral("Failed"),
                static_cast<double>(m_failures.size()), 20);
        }
        setStatusText(entry.newName);
    }

    setStatusText(m_failures.isEmpty()
            ? QStringLiteral("%1 renamed").arg(m_renamed)
            : QStringLiteral("%1 renamed, %2 failed").arg(m_renamed).arg(m_failures.size()));
}

} // namespace mole
