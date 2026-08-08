#include "core/search/LiveSearchTask.h"

#include "core/vfs/DirectoryWalker.h"

namespace mole {

LiveSearchTask::LiveSearchTask(FileSystemPtr fileSystem, VfsUri root, Criteria criteria, QObject* parent)
    : Task(QStringLiteral("Search %1").arg(root.toString()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_root(std::move(root))
    , m_criteria(std::move(criteria))
    , m_foldedText(m_criteria.text.toLower())
{
}

bool LiveSearchTask::matches(const FileEntry& entry) const
{
    if (entry.isDir && !m_criteria.includeDirs)
        return false;
    if (!entry.isDir && !m_criteria.includeFiles)
        return false;

    if (!m_criteria.text.isEmpty()) {
        const bool hit = m_criteria.caseSensitive ? entry.name.contains(m_criteria.text, Qt::CaseSensitive)
                                                  : entry.name.toLower().contains(m_foldedText);
        if (!hit)
            return false;
    }

    if (!m_criteria.extension.isEmpty() && entry.uri.suffix() != m_criteria.extension.toLower())
        return false;
    if (m_criteria.minSize >= 0 && entry.size < m_criteria.minSize)
        return false;
    if (m_criteria.maxSize >= 0 && entry.size > m_criteria.maxSize)
        return false;

    return true;
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

    const auto flush = [&] {
        if (batch.isEmpty())
            return;
        emit hitsFound(batch);
        batch.clear();
    };

    DirectoryWalker walker(m_fileSystem);
    Result<void> walked = walker.walk(m_root, cancelToken(), [&](const FileEntry& entry, int) {
        if (matches(entry)) {
            batch.append(entry);
            ++m_hitCount;
            if (batch.size() >= kEmitBatchSize)
                flush();
        }

        setStatusText(QStringLiteral("%1 matches / %2 scanned").arg(m_hitCount).arg(walker.visitedCount()));

        if (m_hitCount >= m_criteria.maxResults) {
            m_truncated = true;
            return DirectoryWalker::Action::Stop;
        }
        return DirectoryWalker::Action::Continue;
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
