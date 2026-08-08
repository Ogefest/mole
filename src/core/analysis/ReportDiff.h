#pragma once

#include "core/analysis/AnalysisReport.h"

namespace mole {

/// How one extension changed between two runs.
struct ExtensionDelta
{
    QString extension;
    qint64 countBefore = 0;
    qint64 countAfter = 0;
    qint64 bytesBefore = 0;
    qint64 bytesAfter = 0;

    qint64 countDelta() const { return countAfter - countBefore; }
    qint64 bytesDelta() const { return bytesAfter - bytesBefore; }
    bool isNew() const { return countBefore == 0 && countAfter > 0; }
    bool isGone() const { return countBefore > 0 && countAfter == 0; }
    bool changed() const { return countDelta() != 0 || bytesDelta() != 0; }
};

/// What changed in a directory between two analysis runs.
///
/// The reason reports are kept at all: a single number for a folder says
/// little, but "40 GB more, almost all of it .mkv" says what happened.
struct ReportDiff
{
    bool valid = false;
    QDateTime beforeAt;
    QDateTime afterAt;

    qint64 fileCountBefore = 0;
    qint64 fileCountAfter = 0;
    qint64 folderCountBefore = 0;
    qint64 folderCountAfter = 0;
    qint64 totalBytesBefore = 0;
    qint64 totalBytesAfter = 0;

    /// Every extension in either report, biggest absolute byte change first,
    /// so what actually moved is at the top whichever way it went.
    QList<ExtensionDelta> extensions;

    qint64 fileCountDelta() const { return fileCountAfter - fileCountBefore; }
    qint64 folderCountDelta() const { return folderCountAfter - folderCountBefore; }
    qint64 totalBytesDelta() const { return totalBytesAfter - totalBytesBefore; }

    /// Compares two reports of the same directory. Returns an invalid diff
    /// when either report is invalid or they describe different roots.
    static ReportDiff between(const AnalysisReport& before, const AnalysisReport& after);
};

} // namespace mole
