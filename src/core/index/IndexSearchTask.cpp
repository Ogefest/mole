#include "core/index/IndexSearchTask.h"

namespace mole {
namespace {

    /// Names the search after what was typed, when anything was.
    QString titleFor(const SearchQuery& query)
    {
        for (const SearchPredicate& predicate : query.predicates) {
            if (predicate.field == SearchPredicate::Field::Name && !predicate.text.isEmpty())
                return QStringLiteral("Search index for \"%1\"").arg(predicate.text);
        }
        return QStringLiteral("Search the index");
    }

} // namespace

IndexSearchTask::IndexSearchTask(IndexDatabase* index, SearchQuery query, QObject* parent)
    : Task(titleFor(query), parent)
    , m_index(index)
    , m_query(std::move(query))
{
}

void IndexSearchTask::setSearchIo(SearchIo io)
{
    m_io = std::move(io);
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
    //
    // And everything SQL could not state is applied here, to the entry, by the
    // evaluator the walk uses -- so a criterion the database cannot express
    // narrows the answer rather than being lost on the way out of it.
    const SearchPlan plan = planSearch(m_query, SearchSource::Index);
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
        if (!plan.matches(entry, m_io))
            continue;
        entries.append(entry);
    }

    // As many rows as the query was allowed means the volume had more to say,
    // whatever the evaluator then rejected: the rows the database chose are the
    // alphabetically first, and the rest were never looked at. Read from the
    // rows and not from the entries, because the criteria applied above are
    // exactly what makes the two counts differ.
    const int allowed = m_query.effectiveLimit();
    m_truncated = hits.value().size() >= allowed;

    m_hitCount = static_cast<int>(entries.size());
    emit resultsReady(entries);

    setProgress(100);
    setStatusText(m_truncated
            ? QStringLiteral("Stopped at %1 rows from the index (limit reached)").arg(allowed)
            : QStringLiteral("%1 matches from the index").arg(m_hitCount));
}

} // namespace mole
