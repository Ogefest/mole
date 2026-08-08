#include "core/duplicates/FindDuplicatesTask.h"

#include "core/vfs/DirectoryWalker.h"
#include "core/vfs/VfsManager.h"

#include <QHash>
#include <QLocale>

#include <algorithm>

namespace mole {

FindDuplicatesTask::FindDuplicatesTask(
    VfsManager* vfs, QList<VfsUri> roots, std::unique_ptr<IDuplicateStrategy> strategy, QObject* parent)
    : Task(QStringLiteral("Find duplicates"), parent)
    , m_vfs(vfs)
    , m_roots(std::move(roots))
    , m_strategy(std::move(strategy))
{
}

FindDuplicatesTask::~FindDuplicatesTask() = default;

void FindDuplicatesTask::run()
{
    if (!m_vfs || !m_strategy || m_roots.isEmpty()) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("Nothing to search")));
        return;
    }

    // ---- gather -------------------------------------------------------

    QList<FileEntry> candidates;
    QHash<QString, FileSystemPtr> drives;

    for (const VfsUri& root : std::as_const(m_roots)) {
        if (isCancelRequested())
            return;

        FileSystemPtr fs = m_vfs->resolve(root);
        if (!fs)
            continue;
        drives.insert(root.scheme() + QLatin1Char('/') + root.authority(), fs);

        DirectoryWalker walker(fs);
        walker.walk(root, cancelToken(), [&](const FileEntry& entry, int) {
            if (!entry.isDir && entry.size >= m_minimumSize)
                candidates.append(entry);
            setStatusText(QStringLiteral("%1 files").arg(candidates.size()));
            reportCount(QStringLiteral("scanned"), QStringLiteral("Scanned"), candidates.size(), 10);
            return DirectoryWalker::Action::Continue;
        });
    }

    if (isCancelRequested())
        return;

    const auto driveFor = [&](const FileEntry& entry) -> IFileSystem* {
        return drives.value(entry.uri.scheme() + QLatin1Char('/') + entry.uri.authority()).get();
    };

    // ---- narrow, stage by stage ---------------------------------------

    // Everything starts in one bucket; each stage splits every bucket and
    // discards the ones left with a single file. By the last stage -- the
    // expensive one -- almost nothing is left to read.
    QList<QList<FileEntry>> buckets { candidates };

    for (int stage = 0; stage < m_strategy->stageCount(); ++stage) {
        if (isCancelRequested())
            return;

        const QString stageName = m_strategy->stageNames().at(stage);
        const bool reads = m_strategy->stageReadsContent(stage);

        QList<QList<FileEntry>> next;
        int examined = 0;
        int total = 0;
        for (const QList<FileEntry>& bucket : buckets)
            total += static_cast<int>(bucket.size());

        for (const QList<FileEntry>& bucket : buckets) {
            if (isCancelRequested())
                return;

            QHash<QString, QList<FileEntry>> split;
            for (const FileEntry& entry : bucket) {
                if (isCancelRequested())
                    return;

                const QString key
                    = m_strategy->keyFor(stage, entry, reads ? driveFor(entry) : nullptr, cancelToken());
                // An empty key means the file could not be compared. Dropping it
                // beats grouping it with everything else that failed for its own
                // unrelated reason.
                if (!key.isEmpty())
                    split[key].append(entry);

                setProgress(total > 0 ? static_cast<int>(100.0 * ++examined / total) : -1);
            }

            for (auto it = split.constBegin(); it != split.constEnd(); ++it) {
                if (it.value().size() > 1)
                    next.append(it.value());
            }
        }

        buckets = next;
        int remaining = 0;
        for (const QList<FileEntry>& bucket : buckets)
            remaining += static_cast<int>(bucket.size());

        setStatusText(QStringLiteral("%1: %2 candidates").arg(stageName).arg(remaining));
        reportCount(QStringLiteral("candidates"), QStringLiteral("Candidates"), remaining, 20);

        if (buckets.isEmpty())
            break;
    }

    // ---- report -------------------------------------------------------

    for (const QList<FileEntry>& bucket : std::as_const(buckets)) {
        DuplicateGroup group;
        group.files = bucket;
        // What keeping one and removing the rest would free. The first copy is
        // not a saving -- it is the file.
        group.reclaimable = bucket.first().size * (bucket.size() - 1);
        m_reclaimable += group.reclaimable;
        m_groups.append(group);
    }

    // Largest saving first, which is the order anybody clearing space wants.
    std::sort(m_groups.begin(), m_groups.end(),
        [](const DuplicateGroup& a, const DuplicateGroup& b) { return a.reclaimable > b.reclaimable; });

    reportCount(QStringLiteral("groups"), QStringLiteral("Groups"), m_groups.size(), 30);
    if (m_reclaimable > 0) {
        reportBytes(QStringLiteral("reclaimable"), QStringLiteral("Reclaimable"), m_reclaimable, 40);
    }
    setStatusText(m_groups.isEmpty() ? QStringLiteral("no duplicates")
                                     : QStringLiteral("%1 groups · %2 could be freed")
                                           .arg(m_groups.size())
                                           .arg(QLocale().formattedDataSize(m_reclaimable)));

    emit groupsReady(m_groups);
}

} // namespace mole
