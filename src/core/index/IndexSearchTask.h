#pragma once

#include "core/index/IndexDatabase.h"
#include "core/tasks/Task.h"
#include "core/vfs/FileEntry.h"

namespace mole {

/// Runs an index query off the UI thread.
///
/// Even a local SQLite scan can take long enough to drop frames once the
/// catalogue holds millions of rows, and the rule is that the UI thread never
/// waits on storage -- index included.
class IndexSearchTask final : public Task
{
    Q_OBJECT

public:
    /// `index` is borrowed and must outlive the task.
    IndexSearchTask(IndexDatabase* index, SearchQuery query, QObject* parent = nullptr);

    /// How to read a page of a hit, for the criteria a row cannot answer.
    ///
    /// A row records a name, a size and a date; what a file *is* comes from
    /// what is in it. Without this such a criterion matches nothing, which is
    /// the honest answer for a source that cannot look.
    void setSearchIo(SearchIo io);

    int hitCount() const { return m_hitCount; }
    /// Whether the query came back with as many rows as it was allowed.
    ///
    /// The database sorts by name and stops at the limit, and everything the SQL
    /// could not state is applied to those rows afterwards -- so a full answer
    /// means the volume has more to say and this list is a slice of it. The live
    /// search has always reported its own cap; this one said "%1 matches from the
    /// index" with nothing to distinguish a complete answer from a truncated one,
    /// which SearchQuery.h's own promise forbids. See MOLE-371.
    bool truncated() const { return m_truncated; }

signals:
    /// Delivered on the UI thread before finished().
    void resultsReady(const mole::FileEntryList& entries);

protected:
    void run() override;

private:
    IndexDatabase* m_index = nullptr;
    SearchQuery m_query;
    SearchIo m_io;
    int m_hitCount = 0;
    bool m_truncated = false;
};

} // namespace mole
