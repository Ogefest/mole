#include "core/rename/RenameTask.h"

#include "core/vfs/VfsManager.h"

namespace mole {
namespace {

    /// What a file wears for the moment it spends out of the way while a cycle
    /// of renames unwinds. Visibly not a name anybody meant, for the same reason
    /// a partial write is: if the process dies here, whoever finds it should be
    /// able to tell what it was.
    constexpr QLatin1String kRenameParkingSuffix(".mole-renaming");

} // namespace

RenameTask::RenameTask(VfsManager* vfs, QList<RenamePlan::Entry> entries, QObject* parent)
    : Task(QStringLiteral("Rename %1 items").arg(entries.size()), parent)
    , m_vfs(vfs)
    , m_entries(std::move(entries))
{
    for (const RenamePlan::Entry& entry : std::as_const(m_entries))
        noteTouching(entry.source);
}

void RenameTask::run()
{
    if (!m_vfs) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("No drives are available")));
        return;
    }

    // The plan allows a batch to reuse a name another row is giving up -- a swap,
    // or a chain of shifts -- because after the batch every name is free. Doing
    // them in the order they were listed does not work: a rename onto a name
    // that is still occupied is refused, so the row fails and the batch is half
    // applied. So the order is worked out here.
    //
    // Each pass renames every row whose target is not still occupied by another
    // row waiting its turn. A pass that can do nothing has met a cycle, and one
    // member of it is moved to a temporary name to break it -- which is why the
    // suffix exists at all, and why it is visibly not a name anybody meant.
    QList<int> waiting;
    for (int i = 0; i < m_entries.size(); ++i)
        waiting.append(i);

    const auto targetOf = [this](int index) {
        return m_entries.at(index).source.parent().child(m_entries.at(index).newName);
    };
    const auto occupiedByAnother = [&](int index) {
        for (int other : std::as_const(waiting)) {
            if (other != index && m_entries.at(other).source == targetOf(index))
                return true;
        }
        return false;
    };

    int done = 0;
    const int total = m_entries.size();
    const auto carryOut = [&](int index) {
        const RenamePlan::Entry& entry = m_entries.at(index);
        FileSystemPtr fs = m_vfs->resolve(entry.source);
        if (!fs) {
            m_failures.append(QStringLiteral("%1: no drive").arg(entry.originalName));
        } else if (Result<void> renamed = fs->rename(entry.source, targetOf(index), cancelToken());
                   !renamed.ok()) {
            m_failures.append(QStringLiteral("%1: %2").arg(entry.originalName, renamed.error().message));
        } else {
            ++m_renamed;
        }

        setProgress(total > 0 ? static_cast<int>(100.0 * ++done / total) : 100);
        reportCount(QStringLiteral("renamed"), QStringLiteral("Renamed"), m_renamed, 10);
        if (!m_failures.isEmpty()) {
            reportCount(QStringLiteral("failed"), QStringLiteral("Failed"),
                static_cast<double>(m_failures.size()), 20);
        }
        setStatusText(entry.newName);
    };

    while (!waiting.isEmpty()) {
        if (isCancelRequested())
            return;

        QList<int> stillWaiting;
        bool movedSomething = false;
        for (int index : std::as_const(waiting)) {
            if (occupiedByAnother(index)) {
                stillWaiting.append(index);
                continue;
            }
            carryOut(index);
            movedSomething = true;
        }
        waiting = stillWaiting;

        if (!movedSomething && !waiting.isEmpty()) {
            // Every remaining row wants a name another remaining row still has:
            // a cycle. One step out of the way turns it into a chain.
            const int index = waiting.takeFirst();
            RenamePlan::Entry& entry = m_entries[index];
            FileSystemPtr fs = m_vfs->resolve(entry.source);
            const VfsUri parked = entry.source.parent().child(entry.newName + kRenameParkingSuffix);
            if (!fs || !fs->rename(entry.source, parked, cancelToken()).ok()) {
                m_failures.append(
                    QStringLiteral("%1: could not be moved out of the way").arg(entry.originalName));
                setProgress(total > 0 ? static_cast<int>(100.0 * ++done / total) : 100);
                continue;
            }
            // It now stands somewhere nothing else wants, so the rest can go and
            // it takes its own name in a later pass.
            entry.source = parked;
            waiting.append(index);
        }
    }

    setStatusText(m_failures.isEmpty()
            ? QStringLiteral("%1 renamed").arg(m_renamed)
            : QStringLiteral("%1 renamed, %2 failed").arg(m_renamed).arg(m_failures.size()));
}

} // namespace mole
