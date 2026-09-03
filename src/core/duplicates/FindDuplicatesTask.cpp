#include "core/duplicates/FindDuplicatesTask.h"

#include "core/search/SearchQuery.h"
#include "core/vfs/DirectoryWalker.h"
#include "core/vfs/VfsManager.h"

#include <QHash>
#include <QLocale>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>

namespace mole {

FindDuplicatesTask::FindDuplicatesTask(
    VfsManager* vfs, QList<VfsUri> roots, std::unique_ptr<IDuplicateStrategy> strategy, QObject* parent)
    : Task(QStringLiteral("Find duplicates"), parent)
    , m_vfs(vfs)
    , m_roots(std::move(roots))
    , m_strategy(std::move(strategy))
{
    noteTouching(m_roots);
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
    /// Every uri already taken, so nothing is compared with itself.
    QSet<QString> seen;

    for (const VfsUri& root : rootsToWalk()) {
        if (isCancelRequested())
            return;

        FileSystemPtr fs = m_vfs->resolve(root);
        if (!fs)
            continue;
        drives.insert(root.scheme() + QLatin1Char('/') + root.authority(), fs);

        DirectoryWalker walker(fs);
        walker.walk(root, cancelToken(), [&](const FileEntry& entry, int) {
            if (!entry.isDir && entry.size >= m_minimumSize)
                take(entry, candidates, seen);
            setStatusText(QStringLiteral("%1 files").arg(candidates.size()));
            reportCount(QStringLiteral("scanned"), QStringLiteral("Scanned"), candidates.size(), 10);
            return DirectoryWalker::Action::Continue;
        });
        // What could not be read, rather than nothing at all. A subtree this
        // account cannot open is absent from the answer either way; the
        // difference is whether "no duplicates" means the tree has none or means
        // most of it was never looked at. ScanTask, AnalyseDirectoryTask and
        // SyncPlan all say so -- ADR-0030 -- and the one feature whose output is
        // a deletion list did not. See MOLE-341.
        m_unreadable += static_cast<int>(walker.errors().size());
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
    //
    // A pool of its own rather than the one the task itself is running on: this
    // thread is one of that pool's, and waiting on it for work it is queueing
    // there is how a pool deadlocks. It is a local, so its destructor is what
    // guarantees no worker outlives the scan.
    QThreadPool pool;
    pool.setMaxThreadCount(workerCount());

    QList<QList<FileEntry>> buckets { candidates };
    const int lastStage = m_strategy->stageCount() - 1;

    for (int stage = 0; stage < lastStage; ++stage) {
        if (isCancelRequested())
            return;

        buckets = narrowByKey(buckets, stage, driveFor, pool);

        int remaining = 0;
        for (const QList<FileEntry>& bucket : buckets)
            remaining += static_cast<int>(bucket.size());
        reportCount(QStringLiteral("candidates"), QStringLiteral("Candidates"), remaining, 20);

        if (buckets.isEmpty())
            break;
    }

    // ---- confirm, one bucket at a time ---------------------------------

    settle(buckets, lastStage, driveFor, pool);

    // ---- report -------------------------------------------------------

    reportCount(QStringLiteral("groups"), QStringLiteral("Groups"), m_groups.size(), 30);
    if (m_reclaimable > 0) {
        reportBytes(QStringLiteral("reclaimable"), QStringLiteral("Reclaimable"), m_reclaimable, 40);
    }
    QString said = m_groups.isEmpty() ? QStringLiteral("no duplicates")
                                      : QStringLiteral("%1 groups · %2 could be freed")
                                            .arg(m_groups.size())
                                            .arg(QLocale().formattedDataSize(m_reclaimable));
    // Said in the same sentence as the answer, because it qualifies the answer.
    // "no duplicates · 3 places could not be read" is a different statement from
    // "no duplicates", and the second one is what a scan of a tree with an
    // unreadable subtree in it used to make.
    if (m_unreadable > 0) {
        said += QStringLiteral(" · %1 place(s) could not be read").arg(m_unreadable);
        reportCount(QStringLiteral("unreadable"), QStringLiteral("Unreadable"), m_unreadable, 50);
    }
    if (m_links > 0) {
        said += QStringLiteral(" · %1 link(s) left out").arg(m_links);
        reportCount(QStringLiteral("links"), QStringLiteral("Links skipped"), m_links, 60);
    }
    setStatusText(said);
}

QList<VfsUri> FindDuplicatesTask::rootsToWalk() const
{
    // A root inside another root is walked twice, and everything under it then
    // appears twice among the candidates -- where a file is identical to itself,
    // so it is confirmed as a group of one file listed twice whose "could be
    // freed" is its whole size. `/a` and `/a/b` together is not a mistake
    // anybody makes on purpose; it is what a file set holds, or two console
    // arguments. See MOLE-341.
    QList<VfsUri> kept;
    for (const VfsUri& root : m_roots) {
        bool covered = false;
        for (const VfsUri& other : m_roots) {
            if (other == root)
                continue;
            // Under, and not merely equal: two spellings of the same root are
            // the same place, and the first of them is the one to keep.
            if (root.scheme() == other.scheme() && root.authority() == other.authority()
                && isUnder(root.path(), other.path()) && root.path() != other.path()) {
                covered = true;
                break;
            }
        }
        if (covered)
            continue;
        // And the same root twice: kept once.
        if (!kept.contains(root))
            kept.append(root);
    }
    return kept;
}

void FindDuplicatesTask::take(const FileEntry& entry, QList<FileEntry>& candidates, QSet<QString>& seen)
{
    // A symbolic link has the size, the hash and the bytes of what it points at,
    // so it was confirmed as a duplicate of its own target with `reclaimable =
    // size`. Deleting "the copy" then either removes the target and leaves a
    // dangling link, or removes the link and frees nothing at all -- and which
    // of the two happened depended on which one the keep rule picked. Left out
    // and counted, because a file silently missing from a scan is the other way
    // to be wrong. See MOLE-341.
    if (entry.isSymlink || entry.special != SpecialKind::None) {
        ++m_links;
        return;
    }
    // Belt as well as braces: roots that nest are dropped above, and two
    // spellings of one uri would still arrive twice.
    const QString key = entry.uri.toString();
    if (seen.contains(key))
        return;
    seen.insert(key);
    candidates.append(entry);
}

int FindDuplicatesTask::workerCount() const
{
    if (m_workers > 0)
        return m_workers;
    // Half the machine, and never more than eight. The work is reads, so past a
    // point more threads buy nothing on storage that is already saturated and
    // cost seeks on storage that is not -- while on a drive reached over a
    // network, where every read is mostly waiting, the overlap is the whole win.
    // Two at the bottom, because one is what this used to be.
    return qBound(2, QThread::idealThreadCount() / 2, 8);
}

void FindDuplicatesTask::reportStage(int stage, int examined, int total)
{
    setProgress(total > 0 ? static_cast<int>(100.0 * examined / total) : -1);
    // Which stage, and over how much. "whole file: 87 of 412 files" is
    // something somebody can decide to wait for; a spinner is not.
    setStatusText(QStringLiteral("%1: %2 of %3 files")
                      .arg(m_strategy->stageNames().at(stage))
                      .arg(examined)
                      .arg(total));
}

QList<FindDuplicatesTask::StageKey> FindDuplicatesTask::keysFor(
    const QList<FileEntry>& files, int stage, const DriveLookup& driveFor, QThreadPool& pool)
{
    const bool reads = m_strategy->stageReadsContent(stage);

    const auto keyOf = [this, stage, reads, &driveFor](const FileEntry& entry) {
        StageKey out;
        if (isCancelRequested())
            return out;
        out.key = m_strategy->keyFor(stage, entry, reads ? driveFor(entry) : nullptr, cancelToken());
        // A stage that read the file is also asked whether the file is still the
        // one it read. A scan takes minutes and something else is writing: a key
        // taken from content that has since changed would put this file in a
        // group it does not belong to, and the next thing that happens to a group
        // is that all but one of it is deleted. Dropped rather than re-read,
        // because re-reading races the same writer again.
        if (!out.key.isEmpty() && reads && changedUnderTheScan(entry, driveFor(entry))) {
            out.movedUnderfoot = true;
            out.key.clear();
        }
        return out;
    };

    QList<StageKey> keys;
    keys.reserve(files.size());

    // A stage that only reads metadata is not worth handing to a pool: the work
    // per file is building a string, and scheduling it would cost more than it.
    if (!reads || files.size() < 2 || pool.maxThreadCount() < 2) {
        for (const FileEntry& entry : files) {
            if (isCancelRequested())
                return {};
            keys.append(keyOf(entry));
            reportStage(stage, static_cast<int>(keys.size()), static_cast<int>(files.size()));
        }
        return keys;
    }

    // Several files open at once, and the answers taken in the order they went
    // out. Waiting for result *i* rather than for all of them means the progress
    // line keeps moving, and taking them in order means the grouping below sees
    // the same sequence a scan on one thread would have seen.
    QFuture<StageKey> reading
        = QtConcurrent::mapped(&pool, files, std::function<StageKey(const FileEntry&)>(keyOf));
    for (qsizetype i = 0; i < files.size(); ++i) {
        if (isCancelRequested()) {
            reading.cancel();
            reading.waitForFinished();
            return {};
        }
        keys.append(reading.resultAt(static_cast<int>(i)));
        reportStage(stage, static_cast<int>(i + 1), static_cast<int>(files.size()));
    }
    return keys;
}

QList<QList<FileEntry>> FindDuplicatesTask::narrowByKey(
    const QList<QList<FileEntry>>& buckets, int stage, const DriveLookup& driveFor, QThreadPool& pool)
{
    // Every file still in play, in bucket order, so one pass over the pool covers
    // the whole stage. Per bucket would leave most of the threads idle: after the
    // first stage a bucket usually holds two files.
    QList<FileEntry> inPlay;
    for (const QList<FileEntry>& bucket : buckets)
        inPlay.append(bucket);

    const QList<StageKey> keys = keysFor(inPlay, stage, driveFor, pool);
    if (keys.size() != inPlay.size())
        return {}; // cancelled

    QList<QList<FileEntry>> next;
    qsizetype at = 0;
    for (const QList<FileEntry>& bucket : buckets) {
        QHash<QString, QList<FileEntry>> split;
        for (const FileEntry& entry : bucket) {
            const StageKey& key = keys.at(at++);
            if (key.movedUnderfoot)
                ++m_movedUnderfoot;
            else if (!key.key.isEmpty())
                split[key.key].append(entry);
            // An empty key means the file could not be compared. Dropping it
            // beats grouping it with everything else that failed for its own
            // unrelated reason.
        }
        for (auto it = split.constBegin(); it != split.constEnd(); ++it) {
            if (it.value().size() > 1)
                next.append(it.value());
        }
    }
    return next;
}

FindDuplicatesTask::BucketOutcome FindDuplicatesTask::settleBucket(
    const QList<FileEntry>& bucket, int stage, const DriveLookup& driveFor)
{
    BucketOutcome outcome;
    if (isCancelRequested())
        return outcome;

    QList<QList<FileEntry>> found;
    if (m_strategy->stageComparesContent(stage)) {
        found = m_strategy->compare(stage, bucket, driveFor, cancelToken());
    } else {
        QHash<QString, QList<FileEntry>> split;
        for (const FileEntry& entry : bucket) {
            if (isCancelRequested())
                return {};
            const QString key = m_strategy->keyFor(stage, entry,
                m_strategy->stageReadsContent(stage) ? driveFor(entry) : nullptr, cancelToken());
            if (!key.isEmpty())
                split[key].append(entry);
        }
        for (auto it = split.constBegin(); it != split.constEnd(); ++it)
            found.append(it.value());
    }

    // Asked of every file that read its way to a group, comparison or key alike,
    // and for the same reason: what was compared is only evidence about the file
    // as it was when it was opened.
    if (!m_strategy->stageReadsContent(stage)) {
        outcome.groups = found;
        return outcome;
    }
    for (const QList<FileEntry>& group : std::as_const(found)) {
        QList<FileEntry> unchanged;
        for (const FileEntry& entry : group) {
            if (changedUnderTheScan(entry, driveFor(entry)))
                ++outcome.movedUnderfoot;
            else
                unchanged.append(entry);
        }
        if (!unchanged.isEmpty())
            outcome.groups.append(unchanged);
    }
    return outcome;
}

void FindDuplicatesTask::settle(
    const QList<QList<FileEntry>>& buckets, int stage, const DriveLookup& driveFor, QThreadPool& pool)
{
    int total = 0;
    for (const QList<FileEntry>& bucket : buckets)
        total += static_cast<int>(bucket.size());

    const auto settleOne = [this, stage, &driveFor](const QList<FileEntry>& bucket) {
        return settleBucket(bucket, stage, driveFor);
    };

    // Cancelled part-way leaves every group already confirmed, and takes none of
    // them back. They agreed at every stage, and the scan stopping does not make
    // that less true -- it only means there may be more.
    int examined = 0;
    if (buckets.size() < 2 || pool.maxThreadCount() < 2) {
        for (const QList<FileEntry>& bucket : buckets) {
            if (isCancelRequested())
                return;
            const BucketOutcome outcome = settleOne(bucket);
            m_movedUnderfoot += outcome.movedUnderfoot;
            for (const QList<FileEntry>& group : outcome.groups)
                confirm(group);
            examined += static_cast<int>(bucket.size());
            reportStage(stage, examined, total);
        }
        return;
    }

    // Buckets are independent, so they are settled several at a time -- and taken
    // back in the order they went out, which is what keeps confirm() and the list
    // it builds on this thread and free of locks.
    QFuture<BucketOutcome> settling = QtConcurrent::mapped(
        &pool, buckets, std::function<BucketOutcome(const QList<FileEntry>&)>(settleOne));
    for (qsizetype i = 0; i < buckets.size(); ++i) {
        if (isCancelRequested()) {
            settling.cancel();
            settling.waitForFinished();
            return;
        }
        const BucketOutcome outcome = settling.resultAt(static_cast<int>(i));
        m_movedUnderfoot += outcome.movedUnderfoot;
        for (const QList<FileEntry>& group : outcome.groups)
            confirm(group);
        examined += static_cast<int>(buckets.at(i).size());
        reportStage(stage, examined, total);
    }
}

void FindDuplicatesTask::confirm(const QList<FileEntry>& files)
{
    if (files.size() < 2)
        return;

    DuplicateGroup group;
    group.files = files;
    // By path inside the group, rather than in the order the walk happened to
    // reach them. Which copy to keep is a decision somebody makes by reading the
    // paths, and a list that puts them in a different order on a second scan of
    // the same tree cannot be compared with the first. It also made two
    // regenerations of the guide's picture of this view differ with nothing
    // changed. See MOLE-255.
    std::sort(group.files.begin(), group.files.end(),
        [](const FileEntry& a, const FileEntry& b) { return a.uri.toString() < b.uri.toString(); });
    // What keeping one and removing the rest would free. The first copy is not a
    // saving -- it is the file.
    group.reclaimable = files.first().size * (files.size() - 1);
    m_reclaimable += group.reclaimable;

    // Inserted in its place rather than appended and sorted at the end, so the
    // largest saving is at the top at every instant of a scan and not only after
    // it. See ADR-0043 for why that beats a list which is in arrival order for
    // the whole of a long scan and then rearranges itself.
    // Two groups that would free the same amount used to sit in the order they
    // were confirmed in, which depends on which read finished first. Broken by
    // the first path, so the list is the same list twice running.
    const auto at = std::upper_bound(
        m_groups.begin(), m_groups.end(), group, [](const DuplicateGroup& a, const DuplicateGroup& b) {
            if (a.reclaimable != b.reclaimable)
                return a.reclaimable > b.reclaimable;
            return a.files.first().uri.toString() < b.files.first().uri.toString();
        });
    const int position = static_cast<int>(at - m_groups.begin());
    m_groups.insert(at, group);

    // Into the box for the window, rather than an event of its own. What is in the
    // box is carried out by Task's drain: at most one event outstanding, at most
    // one every kDrainIntervalMs, however fast the scan confirms.
    //
    // Before the local hook below and not after it, so an observer that blocks --
    // and one of them does, to hold the scan still while a test looks -- cannot
    // stop the window being told about a group that is already confirmed.
    {
        const QMutexLocker locked(&m_pendingGuard);
        m_pendingGroups.append(group);
        m_pendingPositions.append(position);
    }
    scheduleDrain();

    // An observer that has to act now, on this thread, before the next bucket is
    // looked at. Nothing in the application uses it.
    if (m_onConfirmed)
        m_onConfirmed(group, position);
}

void FindDuplicatesTask::drainPayload()
{
    QList<DuplicateGroup> groups;
    QList<int> positions;
    {
        const QMutexLocker locked(&m_pendingGuard);
        groups.swap(m_pendingGroups);
        positions.swap(m_pendingPositions);
    }
    if (groups.isEmpty())
        return;

    emit groupsFound(groups, positions);
}

} // namespace mole
