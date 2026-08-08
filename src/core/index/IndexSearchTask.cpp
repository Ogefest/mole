#include "core/index/IndexSearchTask.h"

namespace mole {

IndexSearchTask::IndexSearchTask(IndexDatabase* index, IndexSearchQuery query, QObject* parent)
    : Task(QStringLiteral("Search index for \"%1\"").arg(query.text), parent)
    , m_index(index)
    , m_query(std::move(query))
{
}

void IndexSearchTask::run()
{
    if (!m_index) {
        fail(VfsError::make(VfsError::NotSupported, QStringLiteral("No index available")));
        return;
    }

    Result<QList<IndexSearchHit>> hits = m_index->search(m_query);
    if (!hits.ok()) {
        fail(hits.error());
        return;
    }

    // Index hits become ordinary FileEntry values, so the same list model and
    // the same context menu work whether results came from disk or the index.
    FileEntryList entries;
    entries.reserve(hits.value().size());
    for (const IndexSearchHit& hit : hits.value()) {
        FileEntry entry;
        entry.name = hit.name;
        entry.uri = VfsUri::fromString(hit.uri);
        entry.isDir = hit.isDir;
        entry.size = hit.size;
        if (hit.modifiedEpoch > 0)
            entry.modified = QDateTime::fromSecsSinceEpoch(hit.modifiedEpoch);
        entries.append(entry);
    }

    m_hitCount = static_cast<int>(entries.size());
    emit resultsReady(entries);

    setProgress(100);
    setStatusText(QStringLiteral("%1 matches from the index").arg(m_hitCount));
}

} // namespace mole
