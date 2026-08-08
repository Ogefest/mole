#include "core/analysis/ReportDiff.h"

#include <QHash>

#include <algorithm>

namespace mole {

ReportDiff ReportDiff::between(const AnalysisReport& before, const AnalysisReport& after)
{
    ReportDiff diff;
    if (!before.isValid() || !after.isValid())
        return diff;
    // Comparing two different directories would produce numbers that look
    // meaningful and are not.
    if (before.rootUri != after.rootUri)
        return diff;

    diff.valid = true;
    diff.beforeAt = before.createdAt;
    diff.afterAt = after.createdAt;
    diff.fileCountBefore = before.fileCount;
    diff.fileCountAfter = after.fileCount;
    diff.folderCountBefore = before.folderCount;
    diff.folderCountAfter = after.folderCount;
    diff.totalBytesBefore = before.totalBytes;
    diff.totalBytesAfter = after.totalBytes;

    QHash<QString, ExtensionDelta> merged;
    for (const ExtensionStat& stat : before.extensions) {
        ExtensionDelta& delta = merged[stat.extension];
        delta.extension = stat.extension;
        delta.countBefore = stat.count;
        delta.bytesBefore = stat.bytes;
    }
    for (const ExtensionStat& stat : after.extensions) {
        ExtensionDelta& delta = merged[stat.extension];
        delta.extension = stat.extension;
        delta.countAfter = stat.count;
        delta.bytesAfter = stat.bytes;
    }

    diff.extensions = merged.values();

    // Absolute change, so a large deletion is as prominent as a large
    // addition. Unchanged rows sink to the bottom rather than being dropped:
    // "this did not move" is sometimes the answer you came for.
    std::sort(
        diff.extensions.begin(), diff.extensions.end(), [](const ExtensionDelta& a, const ExtensionDelta& b) {
            const qint64 left = std::abs(a.bytesDelta());
            const qint64 right = std::abs(b.bytesDelta());
            if (left != right)
                return left > right;
            return std::abs(a.countDelta()) > std::abs(b.countDelta());
        });

    return diff;
}

} // namespace mole
