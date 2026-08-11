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

    int hitCount() const { return m_hitCount; }

signals:
    /// Delivered on the UI thread before finished().
    void resultsReady(const mole::FileEntryList& entries);

protected:
    void run() override;

private:
    IndexDatabase* m_index = nullptr;
    SearchQuery m_query;
    int m_hitCount = 0;
};

} // namespace mole
