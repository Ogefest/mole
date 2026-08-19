#include "core/search/LiveSearchTask.h"

#include "core/data/FileType.h"
#include "core/vfs/DirectoryWalker.h"

#include <QElapsedTimer>
#include <QIODevice>
#include <QSet>
#include <QtConcurrent/QtConcurrentMap>

namespace mole {
namespace {

    /// One file read and judged, so the answers can come back from the pool in
    /// the order the candidates went out in.
    struct Outcome
    {
        FileEntry entry;
        ContentMatch why;
        bool kept = false;
    };

} // namespace

LiveSearchTask::LiveSearchTask(FileSystemPtr fileSystem, VfsUri root, SearchQuery query, QObject* parent)
    : Task(QStringLiteral("Search %1").arg(root.toString()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_root(std::move(root))
    , m_query(std::move(query))
    , m_plan(planSearch(m_query, SearchSource::Walk))
{
    noteTouching(m_root);
}

void LiveSearchTask::supersede(QHash<QString, QStringList> indexedByParent)
{
    m_indexedByParent = std::move(indexedByParent);
}

void LiveSearchTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(
            VfsError::NotFound, QStringLiteral("Nothing is mounted for %1").arg(m_root.toString())));
        return;
    }

    FileEntryList batch;
    batch.reserve(kEmitBatchSize);
    // Why each of them is a hit, in the same order, and empty when the search
    // asked nothing about what is inside a file.
    QList<ContentMatch> reasons;
    reasons.reserve(kEmitBatchSize);
    /// Whether an entry has to be opened at all, and whether opening it means
    /// reading the whole thing. A type class is a page; a content search is the
    /// file, and only that one is worth a ceiling or a count on the line.
    const bool needsFile = m_plan.needsFile();
    const bool readsWhole = m_plan.readsWholeFiles();

    // Batches go out on whichever comes first: enough matches, or enough time.
    // Counting alone meant a search over a big tree that matched twelve files
    // showed nothing at all until it had finished walking.
    QElapsedTimer sinceLastEmit;
    sinceLastEmit.start();

    const auto flush = [&] {
        if (batch.isEmpty())
            return;
        emit hitsFound(batch, reasons);
        batch.clear();
        reasons.clear();
        sinceLastEmit.restart();
    };

    // What matched inside the directory being listed right now, so the rows an
    // index claimed for it can be checked off against what is really there.
    QSet<QString> matchedHere;

    // A criterion that needs the file itself gets one, and only for the entries
    // that survived everything cheaper -- which is the whole point of the plan
    // handing them over in cost order.
    SearchIo io;
    if (m_plan.needsFile()) {
        io.read = [this](const VfsUri& uri, qint64 offset, qint64 bytes) -> QByteArray {
            Result<std::unique_ptr<QIODevice>> stream = m_fileSystem->openRead(uri, offset + bytes);
            if (!stream.ok() || !stream.value())
                return {};
            if (offset > 0 && !stream.value()->seek(offset)) {
                // A drive that cannot seek is read from the beginning and the
                // front thrown away, which is what the transfer layer does too.
                if (stream.value()->read(offset).size() != offset)
                    return {};
            }
            return stream.value()->read(bytes);
        };
        io.cancelled = [this] { return isCancelRequested(); };
        io.ceiling = m_ceiling;
    }

    const auto keep = [&](const FileEntry& entry, const ContentMatch& why) {
        batch.append(entry);
        reasons.append(why);
        matchedHere.insert(entry.uri.toString());
        ++m_hitCount;
        if (batch.size() >= kEmitBatchSize || sinceLastEmit.elapsed() >= kEmitIntervalMs)
            flush();
    };

    // The files still to be opened, and the reading of them.
    //
    // Several at a time, on the pool every other piece of parallel work in this
    // process uses, because a content search over a thousand candidates spends
    // all of its time waiting for storage and none of it deciding anything. The
    // walk itself stays on this thread: it is the reads that are worth
    // overlapping.
    FileEntryList pending;
    const auto readPending = [&] {
        if (pending.isEmpty())
            return;
        const QList<Outcome> outcomes = QtConcurrent::blockingMapped(
            pending, std::function<Outcome(const FileEntry&)>([this, &io](const FileEntry& entry) {
                Outcome outcome;
                outcome.entry = entry;
                outcome.kept = m_plan.matchesNeedingFile(entry, io, &outcome.why);
                return outcome;
            }));
        // Counted as read only when it really was: one already reported as too
        // big is refused without being opened, and a line saying it was read
        // would be counting work nobody did.
        for (const FileEntry& candidate : pending) {
            if (!readsWhole || candidate.size <= io.ceiling)
                ++m_candidates;
        }
        pending.clear();
        for (const Outcome& outcome : outcomes) {
            if (outcome.kept)
                keep(outcome.entry, outcome.why);
        }
    };
    DirectoryWalker::Options options;
    options.maxDepth = m_query.maxDepth;
    DirectoryWalker walker(m_fileSystem, options);
    Result<void> walked = walker.walk(
        m_root, cancelToken(),
        [&](const FileEntry& entry, int) {
            // Told not to go in here, so it is neither a match nor a place to
            // look. Counted as visited, because it was.
            if (entry.isDir && m_query.isExcluded(entry.name))
                return DirectoryWalker::Action::SkipSubtree;

            // A candidate is an entry that got as far as costing a read, which
            // is the number worth reporting: "read 340 of 1,200" is what makes
            // a search that takes minutes something to wait for rather than
            // give up on.
            // Over the ceiling is reported rather than filtered: the criterion
            // that refuses it is the one that would have read it, and a search
            // that quietly passed over the biggest files in a tree would be
            // answering a different question from the one it was asked.
            if (readsWhole && !entry.isDir && entry.size > io.ceiling)
                ++m_skippedTooBig;

            if (m_plan.matchesWithoutFile(entry)) {
                if (!needsFile || entry.isDir) {
                    keep(entry, {});
                } else {
                    // Held back and read with its neighbours: opening files one
                    // at a time is what makes this the search that takes
                    // minutes, and the reads are what there is to overlap.
                    pending.append(entry);
                    if (pending.size() >= kReadBatchSize)
                        readPending();
                }
            }

            setStatusText(readsWhole
                    ? QStringLiteral("%1 matches / %2 read of %3 scanned")
                          .arg(m_hitCount)
                          .arg(m_candidates)
                          .arg(walker.visitedCount())
                    : QStringLiteral("%1 matches / %2 scanned").arg(m_hitCount).arg(walker.visitedCount()));

            if (m_hitCount >= m_query.limit) {
                m_truncated = true;
                return DirectoryWalker::Action::Stop;
            }
            return DirectoryWalker::Action::Continue;
        },
        [&](const VfsUri& dir, const FileEntryList&) {
            const QStringList claimed = m_indexedByParent.value(dir.toString());
            QStringList gone;
            for (const QString& uri : claimed) {
                if (!matchedHere.contains(uri))
                    gone.append(uri);
            }
            matchedHere.clear();
            if (gone.isEmpty())
                return;
            // Sent before the batch that would otherwise arrive first, so a row
            // is never removed after the walk has just re-found it.
            flush();
            emit hitsGone(gone);
        });

    readPending();
    flush();

    // Hitting the result cap is a normal outcome, not an error -- the UI just
    // has to say so instead of pretending the list is complete.
    if (m_truncated) {
        setStatusText(QStringLiteral("Stopped at %1 matches (limit reached)").arg(m_hitCount));
        return;
    }
    if (!walked.ok()) {
        fail(walked.error());
        return;
    }

    setProgress(100);
    QString done = readsWhole
        ? QStringLiteral("%1 matches / %2 read of %3 scanned")
              .arg(m_hitCount)
              .arg(m_candidates)
              .arg(walker.visitedCount())
        : QStringLiteral("%1 matches / %2 scanned").arg(m_hitCount).arg(walker.visitedCount());
    // A file too big to read is skipped and said to be skipped: a content
    // search that quietly passed over the biggest files in the tree would be
    // answering a different question from the one it was asked.
    if (m_skippedTooBig > 0)
        done += QStringLiteral(" · %1 too big to read").arg(m_skippedTooBig);
    setStatusText(done);
}

} // namespace mole
