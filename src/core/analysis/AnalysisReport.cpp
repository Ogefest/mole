#include "core/analysis/AnalysisReport.h"

#include <QJsonArray>

namespace mole {
namespace {

    QJsonArray extensionsToJson(const QList<ExtensionStat>& stats)
    {
        QJsonArray array;
        for (const ExtensionStat& stat : stats) {
            array.append(QJsonObject { { QStringLiteral("ext"), stat.extension },
                { QStringLiteral("count"), stat.count }, { QStringLiteral("bytes"), stat.bytes } });
        }
        return array;
    }

    QJsonArray foldersToJson(const QList<FolderStat>& stats)
    {
        QJsonArray array;
        for (const FolderStat& stat : stats) {
            array.append(
                QJsonObject { { QStringLiteral("name"), stat.name }, { QStringLiteral("uri"), stat.uri },
                    { QStringLiteral("count"), stat.count }, { QStringLiteral("bytes"), stat.bytes } });
        }
        return array;
    }

    QJsonArray filesToJson(const QList<FileStat>& stats)
    {
        QJsonArray array;
        for (const FileStat& stat : stats) {
            array.append(QJsonObject { { QStringLiteral("name"), stat.name },
                { QStringLiteral("uri"), stat.uri }, { QStringLiteral("bytes"), stat.bytes },
                { QStringLiteral("mtime"),
                    stat.modified.isValid() ? stat.modified.toSecsSinceEpoch() : 0 } });
        }
        return array;
    }

    QJsonArray bucketsToJson(const QList<BucketStat>& stats)
    {
        QJsonArray array;
        for (const BucketStat& stat : stats) {
            array.append(QJsonObject { { QStringLiteral("label"), stat.label },
                { QStringLiteral("count"), stat.count }, { QStringLiteral("bytes"), stat.bytes } });
        }
        return array;
    }

} // namespace

QStringList AnalysisReport::sizeBucketLabels()
{
    return { QStringLiteral("0 – 1 kB"), QStringLiteral("1 – 10 kB"), QStringLiteral("10 – 100 kB"),
        QStringLiteral("100 kB – 1 MB"), QStringLiteral("1 – 10 MB"), QStringLiteral("10 – 100 MB"),
        QStringLiteral("100 MB – 1 GB"), QStringLiteral("over 1 GB") };
}

QStringList AnalysisReport::ageBucketLabels()
{
    return { QStringLiteral("today"), QStringLiteral("this week"), QStringLiteral("this month"),
        QStringLiteral("this year"), QStringLiteral("1 – 5 years"), QStringLiteral("older"),
        QStringLiteral("unknown") };
}

int AnalysisReport::sizeBucketFor(qint64 bytes)
{
    static const qint64 limits[] = { 1024LL, 10LL * 1024, 100LL * 1024, 1024LL * 1024, 10LL * 1024 * 1024,
        100LL * 1024 * 1024, 1024LL * 1024 * 1024 };
    for (int i = 0; i < 7; ++i) {
        if (bytes < limits[i])
            return i;
    }
    return 7;
}

int AnalysisReport::ageBucketFor(const QDateTime& modified, const QDateTime& now)
{
    if (!modified.isValid())
        return 6;

    const qint64 days = modified.daysTo(now);
    if (days < 1)
        return 0;
    if (days < 7)
        return 1;
    if (days < 31)
        return 2;
    if (days < 365)
        return 3;
    if (days < 365 * 5)
        return 4;
    return 5;
}

QJsonObject AnalysisReport::toJson() const
{
    return QJsonObject {
        { QStringLiteral("id"), id },
        { QStringLiteral("rootUri"), rootUri },
        { QStringLiteral("label"), label },
        { QStringLiteral("createdAt"), createdAt.toSecsSinceEpoch() },
        { QStringLiteral("fileCount"), fileCount },
        { QStringLiteral("folderCount"), folderCount },
        { QStringLiteral("totalBytes"), totalBytes },
        { QStringLiteral("maxDepth"), maxDepth },
        { QStringLiteral("unreadableFolders"), unreadableFolders },
        { QStringLiteral("partial"), partial },
        { QStringLiteral("extensions"), extensionsToJson(extensions) },
        { QStringLiteral("topFolders"), foldersToJson(topFolders) },
        { QStringLiteral("largestFiles"), filesToJson(largestFiles) },
        { QStringLiteral("sizeBuckets"), bucketsToJson(sizeBuckets) },
        { QStringLiteral("ageBuckets"), bucketsToJson(ageBuckets) },
    };
}

AnalysisReport AnalysisReport::fromJson(const QJsonObject& json)
{
    AnalysisReport report;
    report.id = json.value(QStringLiteral("id")).toString();
    report.rootUri = json.value(QStringLiteral("rootUri")).toString();
    report.label = json.value(QStringLiteral("label")).toString();

    const qint64 created = json.value(QStringLiteral("createdAt")).toInteger();
    if (created > 0)
        report.createdAt = QDateTime::fromSecsSinceEpoch(created);

    report.fileCount = json.value(QStringLiteral("fileCount")).toInteger();
    report.folderCount = json.value(QStringLiteral("folderCount")).toInteger();
    report.totalBytes = json.value(QStringLiteral("totalBytes")).toInteger();
    report.maxDepth = json.value(QStringLiteral("maxDepth")).toInt();
    report.unreadableFolders = json.value(QStringLiteral("unreadableFolders")).toInt();
    report.partial = json.value(QStringLiteral("partial")).toBool();

    for (const QJsonValue& value : json.value(QStringLiteral("extensions")).toArray()) {
        const QJsonObject entry = value.toObject();
        report.extensions.append(ExtensionStat { entry.value(QStringLiteral("ext")).toString(),
            entry.value(QStringLiteral("count")).toInteger(),
            entry.value(QStringLiteral("bytes")).toInteger() });
    }
    for (const QJsonValue& value : json.value(QStringLiteral("topFolders")).toArray()) {
        const QJsonObject entry = value.toObject();
        report.topFolders.append(FolderStat { entry.value(QStringLiteral("name")).toString(),
            entry.value(QStringLiteral("uri")).toString(), entry.value(QStringLiteral("count")).toInteger(),
            entry.value(QStringLiteral("bytes")).toInteger() });
    }
    for (const QJsonValue& value : json.value(QStringLiteral("largestFiles")).toArray()) {
        const QJsonObject entry = value.toObject();
        FileStat stat;
        stat.name = entry.value(QStringLiteral("name")).toString();
        stat.uri = entry.value(QStringLiteral("uri")).toString();
        stat.bytes = entry.value(QStringLiteral("bytes")).toInteger();
        const qint64 mtime = entry.value(QStringLiteral("mtime")).toInteger();
        if (mtime > 0)
            stat.modified = QDateTime::fromSecsSinceEpoch(mtime);
        report.largestFiles.append(stat);
    }
    for (const QString& key : { QStringLiteral("sizeBuckets"), QStringLiteral("ageBuckets") }) {
        QList<BucketStat> buckets;
        for (const QJsonValue& value : json.value(key).toArray()) {
            const QJsonObject entry = value.toObject();
            buckets.append(BucketStat { entry.value(QStringLiteral("label")).toString(),
                entry.value(QStringLiteral("count")).toInteger(),
                entry.value(QStringLiteral("bytes")).toInteger() });
        }
        if (key == QLatin1String("sizeBuckets"))
            report.sizeBuckets = buckets;
        else
            report.ageBuckets = buckets;
    }

    return report;
}

} // namespace mole
