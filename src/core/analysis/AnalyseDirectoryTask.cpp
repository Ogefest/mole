#include "core/analysis/AnalyseDirectoryTask.h"

#include "core/vfs/DirectoryWalker.h"

#include <QHash>

#include <algorithm>

namespace mole {

AnalyseDirectoryTask::AnalyseDirectoryTask(
    FileSystemPtr fileSystem, VfsUri root, QString label, QObject* parent)
    : Task(QStringLiteral("Analyse %1").arg(root.fileName().isEmpty() ? root.toString() : root.fileName()),
          parent)
    , m_fileSystem(std::move(fileSystem))
    , m_root(std::move(root))
    , m_label(std::move(label))
{
}

void AnalyseDirectoryTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(
            VfsError::NotFound, QStringLiteral("Nothing is mounted for %1").arg(m_root.toString())));
        return;
    }

    const QDateTime startedAt = QDateTime::currentDateTime();

    QHash<QString, ExtensionStat> byExtension;
    QHash<QString, FolderStat> byTopFolder;
    QList<FileStat> largest;
    QList<qint64> sizeCounts(AnalysisReport::sizeBucketLabels().size(), 0);
    QList<qint64> sizeBytes(AnalysisReport::sizeBucketLabels().size(), 0);
    QList<qint64> ageCounts(AnalysisReport::ageBucketLabels().size(), 0);
    QList<qint64> ageBytes(AnalysisReport::ageBucketLabels().size(), 0);

    const QString rootPath = m_root.path();
    /// The immediate child of the root that an entry lives under, so the
    /// report can say which subfolder is responsible for the bulk.
    const auto topFolderOf = [&rootPath](const VfsUri& uri) -> QString {
        QString relative = uri.path().mid(rootPath.size());
        if (relative.startsWith(QLatin1Char('/')))
            relative.remove(0, 1);
        const int slash = relative.indexOf(QLatin1Char('/'));
        return slash < 0 ? QString() : relative.left(slash);
    };

    DirectoryWalker walker(m_fileSystem);
    Result<void> walked = walker.walk(m_root, cancelToken(), [&](const FileEntry& entry, int depth) {
        m_report.maxDepth = std::max(m_report.maxDepth, depth + 1);

        if (entry.isDir) {
            ++m_report.folderCount;
        } else {
            ++m_report.fileCount;
            m_report.totalBytes += entry.size;

            ExtensionStat& extension = byExtension[entry.uri.suffix()];
            extension.extension = entry.uri.suffix();
            ++extension.count;
            extension.bytes += entry.size;

            const int sizeBucket = AnalysisReport::sizeBucketFor(entry.size);
            ++sizeCounts[sizeBucket];
            sizeBytes[sizeBucket] += entry.size;

            const int ageBucket = AnalysisReport::ageBucketFor(entry.modified, startedAt);
            ++ageCounts[ageBucket];
            ageBytes[ageBucket] += entry.size;

            // Kept as a small sorted list rather than sorting everything at
            // the end: the whole point is not holding every file in memory.
            if (largest.size() < kLargestFilesKept || entry.size > largest.last().bytes) {
                FileStat stat;
                stat.name = entry.name;
                stat.uri = entry.uri.toString();
                stat.bytes = entry.size;
                stat.modified = entry.modified;

                const auto position = std::lower_bound(largest.begin(), largest.end(), stat,
                    [](const FileStat& a, const FileStat& b) { return a.bytes > b.bytes; });
                largest.insert(position, stat);
                if (largest.size() > kLargestFilesKept)
                    largest.removeLast();
            }
        }

        const QString top = topFolderOf(entry.uri);
        if (!top.isEmpty()) {
            FolderStat& folder = byTopFolder[top];
            if (folder.name.isEmpty()) {
                folder.name = top;
                folder.uri = m_root.child(top).toString();
            }
            if (!entry.isDir) {
                ++folder.count;
                folder.bytes += entry.size;
            }
        }

        setStatusText(
            QStringLiteral("%1 files, %2 folders").arg(m_report.fileCount).arg(m_report.folderCount));
        return DirectoryWalker::Action::Continue;
    });

    if (!walked.ok() && walked.error().code == VfsError::Cancelled)
        return;

    // Milliseconds, not seconds: two runs of the same folder within one second
    // would otherwise share an id, and the second would overwrite the first --
    // silently costing the history the entry it was meant to compare against.
    m_report.id = startedAt.toUTC().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    m_report.rootUri = m_root.toString();
    m_report.label = m_label.isEmpty() ? m_root.fileName() : m_label;
    m_report.createdAt = startedAt;
    m_report.unreadableFolders = static_cast<int>(walker.errors().size());
    // An unreadable subtree is reported, not hidden: a total that quietly
    // excludes half a disk is worse than no total.
    m_report.partial = m_report.unreadableFolders > 0 || !walked.ok();

    m_report.extensions = byExtension.values();
    std::sort(m_report.extensions.begin(), m_report.extensions.end(),
        [](const ExtensionStat& a, const ExtensionStat& b) { return a.bytes > b.bytes; });

    m_report.topFolders = byTopFolder.values();
    std::sort(m_report.topFolders.begin(), m_report.topFolders.end(),
        [](const FolderStat& a, const FolderStat& b) { return a.bytes > b.bytes; });
    if (m_report.topFolders.size() > kTopFoldersKept)
        m_report.topFolders = m_report.topFolders.mid(0, kTopFoldersKept);

    m_report.largestFiles = largest;

    const QStringList sizeLabels = AnalysisReport::sizeBucketLabels();
    for (int i = 0; i < sizeLabels.size(); ++i)
        m_report.sizeBuckets.append(BucketStat { sizeLabels.at(i), sizeCounts.at(i), sizeBytes.at(i) });

    const QStringList ageLabels = AnalysisReport::ageBucketLabels();
    for (int i = 0; i < ageLabels.size(); ++i)
        m_report.ageBuckets.append(BucketStat { ageLabels.at(i), ageCounts.at(i), ageBytes.at(i) });

    emit reportReady(m_report);

    setProgress(100);
    setStatusText(QStringLiteral("%1 files in %2 folders%3")
                      .arg(m_report.fileCount)
                      .arg(m_report.folderCount)
                      .arg(m_report.unreadableFolders > 0
                              ? QStringLiteral(", %1 unreadable").arg(m_report.unreadableFolders)
                              : QString()));
}

} // namespace mole
