#pragma once

#include "core/search/SearchQuery.h"
#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Searches the filesystem as it is right now by walking it, as opposed to
/// IndexDatabase::search() which answers from a previous scan. Slower but
/// always current -- the two are deliberately separate features.
class LiveSearchTask final : public Task
{
    Q_OBJECT

public:
    LiveSearchTask(FileSystemPtr fileSystem, VfsUri root, SearchQuery query, QObject* parent = nullptr);

    /// Rows an index already reported for this same search, so the walk can say
    /// which of them are no longer true.
    ///
    /// Keyed by the parent directory's uri, because that is the unit the walk
    /// can be sure about: once it has listed a directory it knows everything in
    /// it, and an indexed row there that it did not match is a row that has been
    /// deleted or has stopped matching since the scan. Either way it goes.
    void supersede(QHash<QString, QStringList> indexedByParent);

    qint64 hitCount() const { return m_hitCount; }
    bool truncated() const { return m_truncated; }

signals:
    /// Emitted in batches on the UI thread while the walk is still running, so
    /// results stream in instead of appearing all at once at the end.
    void hitsFound(const mole::FileEntryList& batch);
    /// Rows the index reported that the walk has now disproved: it listed the
    /// directory they are in and they were not among the matches.
    void hitsGone(const QStringList& uris);

protected:
    void run() override;

private:
    /// A full batch is sent as soon as it fills, so a flood of matches costs one
    /// signal per two hundred rather than one per file.
    static constexpr int kEmitBatchSize = 200;
    /// ...and a partial one is sent anyway after this long, which is the half that
    /// was missing: a search finding a dozen matches used to show none of them
    /// until the whole walk finished, because the batch never reached two hundred.
    static constexpr int kEmitIntervalMs = 120;

    FileSystemPtr m_fileSystem;
    VfsUri m_root;
    SearchQuery m_query;
    /// Only ever read on the pool thread, and only after run() starts.
    QHash<QString, QStringList> m_indexedByParent;
    /// A walk pushes nothing down -- it lists a directory and looks at what
    /// came back -- so every criterion is in here, cheapest first.
    SearchPlan m_plan;
    qint64 m_hitCount = 0;
    bool m_truncated = false;
};

} // namespace mole
