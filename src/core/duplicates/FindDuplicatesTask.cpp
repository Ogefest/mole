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

namespace {

    /// Whether the file is no longer the one the listing described.
    ///
    /// Size and modification time, which is all a listing gives and enough for the
    /// case that matters: something wrote to the file between the walk and the read.
    bool changedUnderTheScan(const FileEntry& entry, IFileSystem* fs)
    {
        if (!fs)
            return false;
        const Result<FileEntry> now = fs->stat(entry.uri);
        if (!now.ok())
            return true; // gone, or unreachable: either way not a candidate
        if (now.value().size != entry.size)
            return true;
        return entry.modified.isValid() && now.value().modified.isValid()
            && entry.modified != now.value().modified;
    }

} // namespace

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
    //
    // The last stage is run bucket by bucket rather than over everything at once,
    // and that is the whole of what makes results arrive early: a bucket that
    // survives it is a group nothing later can change, so it goes out then and
    // there instead of waiting for the walk to end. The cheap stages stay
    // breadth-first, because until one of them has run there are no buckets to
    // work on -- and because running them over the lot is what tells the last
    // stage exactly how many files it has to read. See ADR-0043.
    QList<QList<FileEntry>> buckets { candidates };
    const int lastStage = m_strategy->stageCount() - 1;

    for (int stage = 0; stage < lastStage; ++stage) {
        if (isCancelRequested())
            return;

        int examined = 0;
        int total = 0;
        for (const QList<FileEntry>& bucket : buckets)
            total += static_cast<int>(bucket.size());

        QList<QList<FileEntry>> next;
        for (const QList<FileEntry>& bucket : buckets) {
            if (isCancelRequested())
                return;
            next.append(splitAtStage(bucket, stage, driveFor, examined, total));
        }

        buckets = next;
        int remaining = 0;
        for (const QList<FileEntry>& bucket : buckets)
            remaining += static_cast<int>(bucket.size());
        reportCount(QStringLiteral("candidates"), QStringLiteral("Candidates"), remaining, 20);

        if (buckets.isEmpty())
            break;
    }

    // ---- confirm, one bucket at a time ---------------------------------

    int examined = 0;
    int total = 0;
    for (const QList<FileEntry>& bucket : buckets)
        total += static_cast<int>(bucket.size());

    for (const QList<FileEntry>& bucket : std::as_const(buckets)) {
        // Cancelled part-way leaves every group already confirmed, and takes none
        // of them back. They agreed at every stage, and the scan stopping does not
        // make that less true -- it only means there may be more.
        if (isCancelRequested())
            return;

        for (const QList<FileEntry>& settled : splitAtStage(bucket, lastStage, driveFor, examined, total)) {
            confirm(settled);
        }
    }

    // ---- report -------------------------------------------------------

    reportCount(QStringLiteral("groups"), QStringLiteral("Groups"), m_groups.size(), 30);
    if (m_reclaimable > 0) {
        reportBytes(QStringLiteral("reclaimable"), QStringLiteral("Reclaimable"), m_reclaimable, 40);
    }
    setStatusText(m_groups.isEmpty() ? QStringLiteral("no duplicates")
                                     : QStringLiteral("%1 groups · %2 could be freed")
                                           .arg(m_groups.size())
                                           .arg(QLocale().formattedDataSize(m_reclaimable)));
}

QList<QList<FileEntry>> FindDuplicatesTask::splitAtStage(const QList<FileEntry>& bucket, int stage,
    const std::function<IFileSystem*(const FileEntry&)>& driveFor, int& examined, int total)
{
    const QString stageName = m_strategy->stageNames().at(stage);
    const bool reads = m_strategy->stageReadsContent(stage);

    QHash<QString, QList<FileEntry>> split;
    for (const FileEntry& entry : bucket) {
        if (isCancelRequested())
            return {};

        const QString key
            = m_strategy->keyFor(stage, entry, reads ? driveFor(entry) : nullptr, cancelToken());
        // An empty key means the file could not be compared. Dropping it beats
        // grouping it with everything else that failed for its own unrelated
        // reason.
        //
        // A stage that read the file is also asked whether the file is still the
        // one it read. A scan takes minutes and something else is writing: a key
        // taken from content that has since changed would put this file in a group
        // it does not belong to, and the next thing that happens to a group is that
        // all but one of it is deleted. Dropped rather than re-read, because
        // re-reading races the same writer again.
        if (!key.isEmpty() && reads && changedUnderTheScan(entry, driveFor(entry))) {
            ++m_movedUnderfoot;
        } else if (!key.isEmpty()) {
            split[key].append(entry);
        }

        ++examined;
        setProgress(total > 0 ? static_cast<int>(100.0 * examined / total) : -1);
        // Which stage, and over how much. "whole file: 87 of 412 files" is
        // something somebody can decide to wait for; a spinner is not.
        setStatusText(QStringLiteral("%1: %2 of %3 files").arg(stageName).arg(examined).arg(total));
    }

    QList<QList<FileEntry>> survivors;
    for (auto it = split.constBegin(); it != split.constEnd(); ++it) {
        if (it.value().size() > 1)
            survivors.append(it.value());
    }
    return survivors;
}

void FindDuplicatesTask::confirm(const QList<FileEntry>& files)
{
    if (files.size() < 2)
        return;

    DuplicateGroup group;
    group.files = files;
    // What keeping one and removing the rest would free. The first copy is not a
    // saving -- it is the file.
    group.reclaimable = files.first().size * (files.size() - 1);
    m_reclaimable += group.reclaimable;

    // Inserted in its place rather than appended and sorted at the end, so the
    // largest saving is at the top at every instant of a scan and not only after
    // it. See ADR-0043 for why that beats a list which is in arrival order for
    // the whole of a long scan and then rearranges itself.
    const auto at = std::upper_bound(m_groups.begin(), m_groups.end(), group,
        [](const DuplicateGroup& a, const DuplicateGroup& b) { return a.reclaimable > b.reclaimable; });
    const int position = static_cast<int>(at - m_groups.begin());
    m_groups.insert(at, group);

    emit groupFound(group, position);
}

} // namespace mole
