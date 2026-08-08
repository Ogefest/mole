#include "core/index/ScanTask.h"

#include "core/vfs/DirectoryWalker.h"

namespace mole {

ScanTask::ScanTask(
    FileSystemPtr fileSystem, VfsUri root, QString label, IndexDatabase* index, QObject* parent)
    : Task(QStringLiteral("Scan %1").arg(root.toString()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_root(std::move(root))
    , m_label(std::move(label))
    , m_index(index)
{
}

void ScanTask::run()
{
    if (!m_fileSystem || !m_index) {
        fail(VfsError::make(VfsError::NotSupported, QStringLiteral("Scan is missing its backend or index")));
        return;
    }

    Result<qint64> volume = m_index->upsertVolume(m_root, m_label);
    if (!volume.ok()) {
        fail(volume.error());
        return;
    }
    const qint64 volumeId = volume.value();

    // A rescan replaces the previous contents rather than merging, so deleted
    // files disappear from search results instead of lingering forever.
    if (Result<void> cleared = m_index->clearVolume(volumeId); !cleared.ok()) {
        fail(cleared.error());
        return;
    }

    QList<IndexedFile> batch;
    batch.reserve(kBatchSize);
    VfsError writeError;

    const auto flush = [&]() -> bool {
        if (batch.isEmpty())
            return true;
        Result<void> written = m_index->insertBatch(volumeId, batch);
        batch.clear();
        if (!written.ok()) {
            writeError = written.error();
            return false;
        }
        return true;
    };

    DirectoryWalker walker(m_fileSystem);
    Result<void> walked = walker.walk(m_root, cancelToken(), [&](const FileEntry& entry, int) {
        IndexedFile row;
        row.name = entry.name;
        row.path = entry.uri.path();
        row.parentPath = entry.uri.parent().path();
        row.extension = entry.uri.suffix();
        row.isDir = entry.isDir;
        row.size = entry.size;
        row.modifiedEpoch = entry.modified.isValid() ? entry.modified.toSecsSinceEpoch() : 0;
        batch.append(row);
        ++m_filesIndexed;

        if (batch.size() >= kBatchSize && !flush())
            return DirectoryWalker::Action::Stop;

        // Total size is unknown up front, so report throughput instead of a
        // percentage and leave the bar indeterminate.
        setStatusText(QStringLiteral("%1 entries indexed").arg(m_filesIndexed));
        return DirectoryWalker::Action::Continue;
    });

    if (!writeError.isError())
        flush();

    if (writeError.isError()) {
        fail(writeError);
        return;
    }
    if (!walked.ok()) {
        fail(walked.error());
        return;
    }

    m_skippedDirectories = walker.errors().size();

    if (Result<void> marked = m_index->markVolumeScanned(volumeId, QDateTime::currentDateTime());
        !marked.ok()) {
        fail(marked.error());
        return;
    }

    setProgress(100);
    setStatusText(m_skippedDirectories > 0 ? QStringLiteral("%1 entries indexed, %2 directories unreadable")
                                                 .arg(m_filesIndexed)
                                                 .arg(m_skippedDirectories)
                                           : QStringLiteral("%1 entries indexed").arg(m_filesIndexed));
}

} // namespace mole
