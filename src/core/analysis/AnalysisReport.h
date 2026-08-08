#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace mole {

/// How much of a directory one file extension accounts for.
struct ExtensionStat
{
    QString extension; ///< lowercased, no dot; empty means "no extension"
    qint64 count = 0;
    qint64 bytes = 0;
};

/// One immediate subfolder, so the biggest contributor is visible at a glance.
struct FolderStat
{
    QString name;
    QString uri;
    qint64 count = 0;
    qint64 bytes = 0;
};

struct FileStat
{
    QString name;
    QString uri;
    qint64 bytes = 0;
    QDateTime modified;
};

/// A histogram bar: files grouped by size or by age.
struct BucketStat
{
    QString label;
    qint64 count = 0;
    qint64 bytes = 0;
};

/// Everything one analysis run found.
///
/// Deliberately an aggregate rather than a per-file dump: a report has to stay
/// small enough to keep forever, because its whole point is being compared
/// with the next one.
struct AnalysisReport
{
    QString id; ///< sortable, derived from the timestamp
    QString rootUri;
    QString label;
    QDateTime createdAt;

    qint64 fileCount = 0;
    qint64 folderCount = 0;
    qint64 totalBytes = 0;
    int maxDepth = 0;
    /// Directories the walk could not read. A report that silently skipped
    /// half a tree would be worse than no report.
    int unreadableFolders = 0;
    /// True when the walk was stopped early.
    bool partial = false;

    QList<ExtensionStat> extensions; ///< by bytes, descending
    QList<FolderStat> topFolders; ///< immediate children, by bytes
    QList<FileStat> largestFiles;
    QList<BucketStat> sizeBuckets;
    QList<BucketStat> ageBuckets;

    bool isValid() const { return !rootUri.isEmpty() && createdAt.isValid(); }
    qint64 averageFileBytes() const { return fileCount > 0 ? totalBytes / fileCount : 0; }

    QJsonObject toJson() const;
    static AnalysisReport fromJson(const QJsonObject& json);

    /// The buckets every report uses, so two reports are always comparable.
    static QStringList sizeBucketLabels();
    static QStringList ageBucketLabels();
    static int sizeBucketFor(qint64 bytes);
    static int ageBucketFor(const QDateTime& modified, const QDateTime& now);
};

} // namespace mole
