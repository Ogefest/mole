#include "core/search/LiveSearchTask.h"

#include "core/data/FileType.h"
#include "core/vfs/DirectoryWalker.h"

#include <QElapsedTimer>
#include <QIODevice>
#include <QSet>

namespace mole {

LiveSearchTask::LiveSearchTask(FileSystemPtr fileSystem, VfsUri root, SearchQuery query, QObject* parent)
    : Task(QStringLiteral("Search %1").arg(root.toString()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_root(std::move(root))
    , m_query(std::move(query))
    , m_plan(planSearch(m_query, SearchSource::Walk))
{
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

    // Batches go out on whichever comes first: enough matches, or enough time.
    // Counting alone meant a search over a big tree that matched twelve files
    // showed nothing at all until it had finished walking.
    QElapsedTimer sinceLastEmit;
    sinceLastEmit.start();

    const auto flush = [&] {
        if (batch.isEmpty())
            return;
        emit hitsFound(batch);
        batch.clear();
        sinceLastEmit.restart();
    };

    // What matched inside the directory being listed right now, so the rows an
    // index claimed for it can be checked off against what is really there.
    QSet<QString> matchedHere;

    // A criterion that needs the file itself gets a page of it, and only for
    // the entries that survived everything cheaper -- which is the whole point
    // of the plan handing them over in cost order.
    SampleReader sample;
    if (m_plan.needsSample()) {
        sample = [this](const VfsUri& uri) -> QByteArray {
            Result<std::unique_ptr<QIODevice>> stream = m_fileSystem->openRead(uri, FileType::kSampleBytes);
            if (!stream.ok() || !stream.value())
                return {};
            return stream.value()->read(FileType::kSampleBytes);
        };
    }

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

            if (m_plan.matches(entry, sample)) {
                batch.append(entry);
                matchedHere.insert(entry.uri.toString());
                ++m_hitCount;
                if (batch.size() >= kEmitBatchSize || sinceLastEmit.elapsed() >= kEmitIntervalMs)
                    flush();
            }

            setStatusText(
                QStringLiteral("%1 matches / %2 scanned").arg(m_hitCount).arg(walker.visitedCount()));

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
    setStatusText(QStringLiteral("%1 matches / %2 scanned").arg(m_hitCount).arg(walker.visitedCount()));
}

} // namespace mole
