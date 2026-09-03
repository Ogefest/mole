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
    noteTouching(m_root);
}

void ScanTask::setContainerReader(std::function<QList<IndexedFile>(const FileEntry&, bool*)> reader)
{
    m_containers = std::move(reader);
}

void ScanTask::setFactReader(std::function<QList<SearchFact>(const FileEntry&)> reader)
{
    m_facts = std::move(reader);
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
    // files disappear from search results instead of lingering forever. It
    // builds them beside the old ones and swaps at the end: a search running
    // while a 4 TB tree is re-walked is answered from the previous scan, in
    // full, rather than from the part that has been reached so far.
    Result<qint64> opened = m_index->beginScan(volumeId);
    if (!opened.ok()) {
        fail(opened.error());
        return;
    }
    const qint64 generation = opened.value();

    // Every way out of here except a finished walk. What this scan wrote was
    // never visible and never will be, so it goes -- and if that fails too, it
    // stays invisible and the next scan of this volume sweeps it out.
    const auto giveUp = [&](const VfsError& error) {
        (void)m_index->abandonScan(volumeId, generation);
        fail(error);
    };

    // What the last finished scan recorded about the folders, which is the only
    // thing an incremental scan has to go on.
    QHash<QString, qint64> knownFolders;
    if (m_options.incremental) {
        if (Result<QHash<QString, qint64>> previous = m_index->directoryTimes(volumeId); previous.ok())
            knownFolders = previous.value();
    }

    QList<IndexedFile> batch;
    batch.reserve(kBatchSize);
    VfsError writeError;

    const auto flush = [&]() -> bool {
        if (batch.isEmpty())
            return true;
        Result<void> written = m_index->insertBatch(volumeId, generation, batch);
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
        // A file whose readers find nothing, or fail, is indexed without them:
        // a reader costs its own rows and never the scan.
        if (m_facts && !entry.isDir) {
            row.facts = m_facts(entry);
            ++m_filesRead;
        }
        // Unchanged since the last scan, so everything under it is what it
        // was. Carried across rather than re-walked -- and only because this
        // walk just saw the folder in its parent's listing, which is what makes
        // a deleted subtree disappear rather than linger.
        if (entry.isDir && m_options.incremental && entry.modified.isValid()) {
            m_datesFolders = true;
            const auto known = knownFolders.constFind(entry.uri.path());
            if (known != knownFolders.constEnd() && *known == entry.modified.toSecsSinceEpoch()) {
                batch.append(row);
                ++m_filesIndexed;
                if (!flush()) // the carry has to land after the folder's own row
                    return DirectoryWalker::Action::Stop;
                const Result<qint64> carried = m_index->carryForward(volumeId, generation, entry.uri.path());
                if (!carried.ok()) {
                    // The subtree is neither carried nor walked, so committing
                    // now would swap in a generation it is missing from -- and
                    // the task used to report that as a success. A carry is a
                    // write, and a write that failed stops the scan like any
                    // other. See MOLE-340.
                    writeError = carried.error();
                    return DirectoryWalker::Action::Stop;
                }
                m_carried += carried.value();
                m_filesIndexed += carried.value();
                return DirectoryWalker::Action::SkipSubtree;
            }
        }

        // What is inside it, when it is a container and somebody can open one.
        // A member is an ordinary row with its own uri, so everything that
        // works on a row -- the preview, the operations, a file set -- works on
        // it without learning what an archive is.
        QList<IndexedFile> members;
        if (m_containers && !entry.isDir) {
            bool truncated = false;
            members = m_containers(entry, &truncated);
            // Said on the container's own row: a listing trimmed in silence
            // reads as a complete one.
            if (truncated) {
                row.facts.append(SearchFact { QStringLiteral("archive.truncated"),
                    QStringLiteral("more entries than the index will take"), 0, false });
            }
        }

        batch.append(row);
        ++m_filesIndexed;

        for (IndexedFile& member : members) {
            batch.append(std::move(member));
            ++m_filesIndexed;
            ++m_containedEntries;
        }

        if (batch.size() >= kBatchSize && !flush())
            return DirectoryWalker::Action::Stop;

        // Total size is unknown up front, so report throughput instead of a
        // percentage and leave the bar indeterminate.
        setStatusText(m_facts
                ? QStringLiteral("%1 entries indexed, %2 read").arg(m_filesIndexed).arg(m_filesRead)
                : QStringLiteral("%1 entries indexed").arg(m_filesIndexed));
        return DirectoryWalker::Action::Continue;
    });

    if (!writeError.isError())
        flush();

    if (writeError.isError()) {
        giveUp(writeError);
        return;
    }
    if (!walked.ok()) {
        giveUp(walked.error());
        return;
    }

    m_skippedDirectories = walker.errors().size();

    // The one moment the volume changes: the new contents become the answer
    // and the old ones go, in a single transaction.
    if (Result<void> committed
        = m_index->commitScan(volumeId, generation, QDateTime::currentDateTime(), m_options);
        !committed.ok()) {
        giveUp(committed.error());
        return;
    }

    setProgress(100);
    QStringList extras;
    if (m_options.incremental && m_carried > 0)
        extras.append(QStringLiteral("%1 unchanged and kept").arg(m_carried));
    if (m_options.incremental && !m_datesFolders) {
        // A drive that does not date its folders gives an incremental scan
        // nothing to work from. Saying so beats looking like a slow one.
        extras.append(QStringLiteral("this drive does not date its folders, so all of it was walked"));
    }
    if (m_facts)
        extras.append(QStringLiteral("%1 read for what they say about themselves").arg(m_filesRead));
    if (m_containedEntries > 0)
        extras.append(QStringLiteral("%1 from inside containers").arg(m_containedEntries));
    if (!extras.isEmpty()) {
        setStatusText(QStringLiteral("%1 entries indexed, %2")
                          .arg(m_filesIndexed)
                          .arg(extras.join(QStringLiteral(", "))));
        return;
    }
    setStatusText(m_skippedDirectories > 0 ? QStringLiteral("%1 entries indexed, %2 directories unreadable")
                                                 .arg(m_filesIndexed)
                                                 .arg(m_skippedDirectories)
                                           : QStringLiteral("%1 entries indexed").arg(m_filesIndexed));
}

} // namespace mole
