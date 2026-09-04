#include "sdk/ScanReaders.h"

#include "sdk/IMetadataReader.h"

#include "core/data/FileType.h"
#include "core/diagnostics/Diagnostics.h"
#include "core/index/ScanTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/DirectoryWalker.h"
#include "core/vfs/IFileSystemFactory.h"
#include "core/vfs/VfsManager.h"

#include <QIODevice>
#include <QUrl>

namespace mole {

namespace {
    /// Twenty thousand rather than everything, because one file holding a
    /// million entries would put a million rows in the index and make every
    /// search over that volume answer for it. A container that gives up more
    /// says so on its own row rather than being trimmed in silence.
    constexpr int kMaxContainerEntries = 20000;
    /// And how big a container is worth opening on a drive where opening it
    /// means fetching it whole.
    constexpr qint64 kRemoteContainerCeiling = 32 * 1024 * 1024;
}

std::function<QList<SearchFact>(const FileEntry&, const CancelToken&)> factReaderFor(
    const PluginServices& services, const FileSystemPtr& fileSystem)
{
    if (!fileSystem || !services.metadata)
        return {};

    return [fileSystem, services](const FileEntry& entry, const CancelToken& cancel) -> QList<SearchFact> {
        const QList<IMetadataReader*> readers = services.metadata->readersFor(entry);
        if (readers.isEmpty())
            return {};

        // The page the type sniff would have read, read once and handed to
        // every reader -- which is the contract IMetadataReader already states.
        QByteArray head;
        if (Result<std::unique_ptr<QIODevice>> stream
            = fileSystem->openRead(entry.uri, FileType::kSampleBytes);
            stream.ok() && stream.value()) {
            head = stream.value()->read(FileType::kSampleBytes);
        }

        QList<SearchFact> out;
        for (IMetadataReader* reader : readers) {
            if (cancel.isCancelled())
                break;
            if (!reader)
                continue;

            // A reader that throws its hands up costs its own rows and nobody
            // else's, which is what the extension point promises -- and the call
            // was unguarded, so it cost rather more than that. The throw
            // travelled out of the walker's callback and ScanTask::run() into
            // Task::run()'s generic net: the scan ended Failed with "stopped
            // unexpectedly", the volume was left half-written, and nothing named
            // the reader. The panel has caught this all along; the same two
            // clauses, and the same log line.
            try {
                for (const FileFact& fact : reader->read(entry, head, services, cancel)) {
                    if (!fact.isAskable())
                        continue;
                    out.append(SearchFact {
                        fact.key, fact.value, fact.hasNumber() ? fact.number : 0, fact.hasNumber() });
                }
            } catch (const std::exception& error) {
                qCWarning(taskLog, "metadata reader %s failed: %s", qPrintable(reader->id()), error.what());
            } catch (...) {
                qCWarning(taskLog, "metadata reader %s failed", qPrintable(reader->id()));
            }
        }
        return out;
    };
}

std::function<QList<IndexedFile>(const FileEntry&, bool*)> containerReaderFor(
    const PluginServices& services, const VfsUri& root)
{
    if (!services.vfs)
        return {};

    // Whichever backend claims this kind of file, which is a plugin's business
    // and not this one's. A build without one simply has no factory that does.
    QList<IFileSystemFactory*> openers;
    for (IFileSystemFactory* factory : services.vfs->factories()) {
        if (!factory->mountableFileSuffixes().isEmpty())
            openers.append(factory);
    }
    if (openers.isEmpty())
        return {};

    const bool remote = root.scheme() != QLatin1String("file");
    return [openers, remote](const FileEntry& entry, bool* truncatedOut) -> QList<IndexedFile> {
        // Nested containers are rows and nothing more. Following one is an
        // unbounded recursion with a bad failure mode, and a container inside a
        // container is a member like any other.
        if (entry.uri.scheme() == QLatin1String("archive"))
            return {};
        // Listing one on a remote drive means fetching it.
        if (remote && entry.size > kRemoteContainerCeiling)
            return {};

        const QString suffix = entry.uri.suffix();
        IFileSystemFactory* opener = nullptr;
        for (IFileSystemFactory* factory : openers) {
            if (factory->mountableFileSuffixes().contains(suffix)) {
                opener = factory;
                break;
            }
        }
        const QString localPath = entry.uri.toLocalPath();
        if (!opener || localPath.isEmpty())
            return {};

        QString error;
        FileSystemPtr inside = opener->create(opener->configForFile(localPath), &error);
        if (!inside)
            return {}; // corrupt, encrypted, or nothing this build can open

        // Where a member sits *on the volume*, which is inside the container's
        // own path. The row used to carry the path within the archive --
        // /2019/img.jpg -- and every prefix question in the index is asked on
        // that column: carrying an unchanged folder forward copies rows whose
        // path is under it, and a member's was not, so an incremental scan
        // dropped the contents of every archive in a folder it did not re-walk.
        // The other way round is worse: a member whose inner path happens to
        // begin with a real folder's was carried under *that* folder. See
        // MOLE-340.
        //
        // The separator says which half is which. A '/' would make a member
        // indistinguishable from a file in a directory of the same name, and
        // that ambiguity is what a prefix query would then act on.
        const QString containerPath = entry.uri.path();
        const auto insideContainer = [&containerPath](const QString& innerPath) {
            return innerPath == QLatin1String("/") ? containerPath
                                                   : containerPath + QLatin1Char('!') + innerPath;
        };

        QList<IndexedFile> rows;
        bool cut = false;
        DirectoryWalker walker(inside);
        const Result<void> walked = walker.walk(
            opener->rootUriForFile(localPath), CancelToken {}, [&](const FileEntry& member, int) {
                if (rows.size() >= kMaxContainerEntries) {
                    cut = true;
                    return DirectoryWalker::Action::Stop;
                }
                IndexedFile row;
                row.name = member.name;
                row.path = insideContainer(member.uri.path());
                row.parentPath = insideContainer(member.uri.parent().path());
                row.extension = member.uri.suffix();
                row.isDir = member.isDir;
                row.size = member.size;
                row.modifiedEpoch = member.modified.isValid() ? member.modified.toSecsSinceEpoch() : 0;
                // Addressed as it really is, not as the volume is: a member
                // lives on the archive's own authority, and rebuilding its uri
                // from the volume's scheme would put it loose on the disk.
                row.uri = member.uri.toString();
                rows.append(row);
                return DirectoryWalker::Action::Continue;
            });
        // A container that could not be read costs its own rows and nothing
        // else; the scan carries on either way.
        if (!walked.ok() && rows.isEmpty())
            return {};

        if (truncatedOut)
            *truncatedOut = cut;
        return rows;
    };
}

Task* scanRunningOn(const PluginServices& services, const VfsUri& root)
{
    if (!services.tasks)
        return nullptr;

    const QList<Task*> running = services.tasks->tasks();
    for (Task* task : running) {
        if (!qobject_cast<ScanTask*>(task) || task->isFinished())
            continue;
        if (task->touching().contains(root))
            return task;
    }
    return nullptr;
}

void applyScanOptions(ScanTask& task, const ScanOptions& options, const PluginServices& services,
    const FileSystemPtr& fileSystem, const VfsUri& root)
{
    task.setOptions(options);
    // Asked for per scan rather than assumed: reading every file in a tree is
    // bounded per file and unbounded in aggregate. See ADR-0039.
    if (options.metadata)
        task.setFactReader(factReaderFor(services, fileSystem));
    // On for a local drive, where a zip is one read; off for a remote one,
    // where listing an archive means fetching it whole.
    if (options.archives)
        task.setContainerReader(containerReaderFor(services, root));
}

} // namespace mole
