#include "core/alerts/AlertEvaluator.h"

#include "core/analysis/AnalysisReport.h"
#include "core/analysis/AnalysisStore.h"
#include "core/vfs/DirectoryWalker.h"
#include "core/vfs/VfsManager.h"

#include <QLocale>

#include <cmath>

namespace mole {
namespace {

    /// What a tree walk gathers in one pass. Walking once and answering several
    /// metrics from it costs the same as answering one.
    struct TreeFacts
    {
        qint64 totalBytes = 0;
        qint64 fileCount = 0;
        qint64 folderCount = 0;
        qint64 largestFile = 0;
        QDateTime newestModified;
        int unreadableFolders = 0;
    };

    QString formatNumber(AlertMetric metric, double value)
    {
        const QString unit = alertMetricUnit(metric);
        if (unit == QLatin1String("bytes"))
            return QLocale().formattedDataSize(static_cast<qint64>(value));
        if (unit == QLatin1String("%"))
            return QStringLiteral("%1%").arg(QLocale().toString(value, 'f', 1));
        if (unit.isEmpty())
            return QLocale().toString(value, 'g', 6);
        return QStringLiteral("%1 %2").arg(QLocale().toString(value, 'g', 6), unit);
    }

} // namespace

AlertEvaluator::AlertEvaluator(VfsManager* vfs, AnalysisStore* analysis)
    : m_vfs(vfs)
    , m_analysis(analysis)
{
}

AlertEvaluator::Reading AlertEvaluator::measure(const AlertRule& rule, const CancelToken& cancel) const
{
    if (rule.source == AlertSource::LatestReport)
        return measureFromReport(rule);
    return measureLive(rule, cancel);
}

AlertEvaluator::Reading AlertEvaluator::measureFromReport(const AlertRule& rule) const
{
    Reading reading;
    if (!m_analysis) {
        reading.error = QStringLiteral("Reports are not available");
        return reading;
    }

    const AnalysisReport report = m_analysis->latest(rule.targetUri);
    if (!report.isValid()) {
        // Deliberately not falling back to a live walk: an alert set against a
        // report must say when the report is missing, or it would quietly
        // become a different alert with different timing.
        reading.error = QStringLiteral("No report has been produced for this folder yet");
        return reading;
    }

    reading.measured = true;
    switch (rule.metric) {
    case AlertMetric::TotalSize:
        reading.number = static_cast<double>(report.totalBytes);
        break;
    case AlertMetric::FileCount:
        reading.number = static_cast<double>(report.fileCount);
        break;
    case AlertMetric::FolderCount:
        reading.number = static_cast<double>(report.folderCount);
        break;
    case AlertMetric::LargestFile:
        reading.number
            = report.largestFiles.isEmpty() ? 0.0 : static_cast<double>(report.largestFiles.first().bytes);
        break;
    case AlertMetric::UnreadableFolders:
        reading.number = report.unreadableFolders;
        break;
    case AlertMetric::NewestFileAgeHours: {
        QDateTime newest;
        for (const FileStat& file : report.largestFiles) {
            if (!newest.isValid() || file.modified > newest)
                newest = file.modified;
        }
        if (!newest.isValid()) {
            reading.measured = false;
            reading.error = QStringLiteral("The report records no timestamps");
            return reading;
        }
        reading.number = newest.secsTo(report.createdAt) / 3600.0;
        break;
    }
    default:
        reading.measured = false;
        reading.error = QStringLiteral("%1 cannot be read from a report").arg(alertMetricLabel(rule.metric));
        return reading;
    }

    reading.text = formatNumber(rule.metric, reading.number);
    return reading;
}

AlertEvaluator::Reading AlertEvaluator::measureLive(const AlertRule& rule, const CancelToken& cancel) const
{
    Reading reading;
    if (!m_vfs) {
        reading.error = QStringLiteral("No drives are available");
        return reading;
    }

    const VfsUri target = VfsUri::fromString(rule.targetUri);
    FileSystemPtr fs = m_vfs->resolve(target);
    if (!fs) {
        reading.error = QStringLiteral("No drive is mounted for %1").arg(rule.targetUri);
        return reading;
    }

    Result<FileEntry> stat = fs->stat(target);

    if (rule.metric == AlertMetric::Exists) {
        reading.measured = true;
        reading.number = stat.ok() ? 1.0 : 0.0;
        reading.text = stat.ok() ? QStringLiteral("yes") : QStringLiteral("no");
        return reading;
    }

    if (rule.metric == AlertMetric::FreeSpace || rule.metric == AlertMetric::FreeSpacePercent) {
        if (!fs->capabilities().testFlag(VfsCapability::ReportsSpace)) {
            reading.error = QStringLiteral("This drive does not report a capacity");
            return reading;
        }
        Result<SpaceInfo> space = fs->space(target);
        if (!space.ok()) {
            reading.error = space.error().message;
            return reading;
        }
        reading.measured = true;
        reading.number = rule.metric == AlertMetric::FreeSpace ? static_cast<double>(space.value().freeBytes)
                                                               : (1.0 - space.value().usedFraction()) * 100.0;
        reading.text = formatNumber(rule.metric, reading.number);
        return reading;
    }

    if (!stat.ok()) {
        reading.error = stat.error().message;
        return reading;
    }
    const FileEntry entry = stat.value();

    if (rule.metric == AlertMetric::Permissions) {
        if (entry.permissions.isEmpty()) {
            reading.error = QStringLiteral("This drive does not report permissions");
            return reading;
        }
        reading.measured = true;
        reading.text = entry.permissions;
        return reading;
    }

    if (rule.metric == AlertMetric::ModifiedTime) {
        reading.measured = true;
        reading.number = static_cast<double>(entry.modified.toSecsSinceEpoch());
        reading.text = entry.modified.toString(Qt::ISODate);
        return reading;
    }

    // A single file answers the tree metrics without a walk.
    if (!entry.isDir) {
        reading.measured = true;
        switch (rule.metric) {
        case AlertMetric::TotalSize:
        case AlertMetric::LargestFile:
            reading.number = static_cast<double>(entry.size);
            break;
        case AlertMetric::FileCount:
            reading.number = 1.0;
            break;
        case AlertMetric::FolderCount:
        case AlertMetric::UnreadableFolders:
            reading.number = 0.0;
            break;
        case AlertMetric::NewestFileAgeHours:
            reading.number = entry.modified.secsTo(QDateTime::currentDateTime()) / 3600.0;
            break;
        default:
            reading.measured = false;
            reading.error = QStringLiteral("%1 does not apply to a file").arg(alertMetricLabel(rule.metric));
            return reading;
        }
        reading.text = formatNumber(rule.metric, reading.number);
        return reading;
    }

    TreeFacts facts;
    DirectoryWalker walker(fs);
    Result<void> walked = walker.walk(target, cancel, [&facts](const FileEntry& child, int) {
        if (child.isDir) {
            ++facts.folderCount;
        } else {
            ++facts.fileCount;
            facts.totalBytes += child.size;
            facts.largestFile = std::max(facts.largestFile, child.size);
        }
        if (child.modified.isValid()
            && (!facts.newestModified.isValid() || child.modified > facts.newestModified)) {
            facts.newestModified = child.modified;
        }
        return DirectoryWalker::Action::Continue;
    });
    facts.unreadableFolders = static_cast<int>(walker.errors().size());

    if (!walked.ok() && walked.error().code == VfsError::Cancelled) {
        reading.error = QStringLiteral("Cancelled");
        return reading;
    }

    reading.measured = true;
    switch (rule.metric) {
    case AlertMetric::TotalSize:
        reading.number = static_cast<double>(facts.totalBytes);
        break;
    case AlertMetric::FileCount:
        reading.number = static_cast<double>(facts.fileCount);
        break;
    case AlertMetric::FolderCount:
        reading.number = static_cast<double>(facts.folderCount);
        break;
    case AlertMetric::LargestFile:
        reading.number = static_cast<double>(facts.largestFile);
        break;
    case AlertMetric::UnreadableFolders:
        reading.number = facts.unreadableFolders;
        break;
    case AlertMetric::NewestFileAgeHours:
        if (!facts.newestModified.isValid()) {
            reading.measured = false;
            reading.error = QStringLiteral("This folder is empty");
            return reading;
        }
        reading.number = facts.newestModified.secsTo(QDateTime::currentDateTime()) / 3600.0;
        break;
    default:
        reading.measured = false;
        reading.error = QStringLiteral("%1 cannot be measured here").arg(alertMetricLabel(rule.metric));
        return reading;
    }

    reading.text = formatNumber(rule.metric, reading.number);
    return reading;
}

AlertRule AlertEvaluator::apply(const AlertRule& rule, const Reading& reading, const QDateTime& at)
{
    AlertRule updated = rule;
    updated.lastCheckedAt = at;

    if (!reading.measured) {
        // A metric that could not be read is not "fine". Reporting Ok here
        // would turn an unreachable drive into a green tick.
        updated.state = AlertState::Failed;
        updated.message = reading.error;
        return updated;
    }

    const bool numeric = alertMetricIsNumeric(rule.metric);
    bool tripped = false;

    switch (rule.comparison) {
    case AlertComparison::Above:
        tripped = numeric && reading.number > rule.threshold;
        break;
    case AlertComparison::Below:
        tripped = numeric && reading.number < rule.threshold;
        break;
    case AlertComparison::Equals:
        tripped = numeric ? std::abs(reading.number - rule.threshold) < 1e-9
                          : reading.text == QString::number(rule.threshold);
        break;
    case AlertComparison::Changed:
        // A first reading has nothing to differ from, so it establishes the
        // baseline rather than firing. Otherwise every new alert would trip
        // the moment it was created.
        tripped = rule.lastCheckedAt.isValid() && reading.text != rule.lastValue;
        break;
    }

    updated.lastValue = reading.text;
    updated.lastNumericValue = reading.number;
    updated.state = tripped ? AlertState::Triggered : AlertState::Ok;

    if (tripped) {
        if (rule.state != AlertState::Triggered)
            updated.triggeredAt = at;
        updated.message = rule.comparison == AlertComparison::Changed
            ? QStringLiteral("%1 changed from %2 to %3")
                  .arg(alertMetricLabel(rule.metric), rule.lastValue, reading.text)
            : QStringLiteral("%1 is %2").arg(alertMetricLabel(rule.metric), reading.text);
    } else {
        updated.triggeredAt = QDateTime();
        updated.message.clear();
    }

    return updated;
}

} // namespace mole
