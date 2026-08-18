#include "core/duplicates/FindDuplicatesTask.h"

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
    setStatusText(m_groups.isEmpty() ? QStringLiteral("no duplicates")
                                     : QStringLiteral("%1 groups · %2 could be freed")
                                           .arg(m_groups.size())
                                           .arg(QLocale().formattedDataSize(m_reclaimable)));
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
