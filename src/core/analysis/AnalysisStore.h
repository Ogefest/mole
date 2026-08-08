#pragma once

#include "core/analysis/AnalysisReport.h"

#include <QList>
#include <QSet>
#include <QString>

namespace mole {

/// Enough of a report to list it without loading it.
struct ReportSummary
{
    QString id;
    QString rootUri;
    QString label;
    QDateTime createdAt;
    qint64 fileCount = 0;
    qint64 totalBytes = 0;
};

/// Keeps every analysis run, so the next one has something to be compared with.
///
/// One JSON file per report, grouped by the directory it describes. Plain files
/// rather than a database because a report is written once and read rarely, the
/// history is a handful of entries, and being able to open one in a text editor
/// has already paid for itself more than a schema would.
class AnalysisStore
{
public:
    /// `MOLE_ANALYSIS_PATH` wins, so tests never touch the user's history.
    static QString defaultDirectory();

    explicit AnalysisStore(QString directory);

    const QString& directory() const { return m_directory; }

    bool save(const AnalysisReport& report) const;

    /// Newest first. Missing or unreadable files are skipped rather than
    /// failing the whole listing.
    QList<ReportSummary> history(const QString& rootUri) const;
    /// Every directory that has ever been analysed.
    QStringList analysedRoots() const;

    /// The hashed folder names present on disk, without reading any of them.
    ///
    /// This is what makes "does this row have a report?" affordable for a
    /// listing of five thousand entries: one directory read, then a hash per
    /// row and a set lookup, rather than a file open per row.
    QSet<QString> storedFolderNames() const;

    AnalysisReport load(const QString& rootUri, const QString& id) const;
    /// The most recent report for a directory, or an invalid one.
    AnalysisReport latest(const QString& rootUri) const;

    bool remove(const QString& rootUri, const QString& id) const;
    /// Drops the oldest reports beyond `keep`. History is useful; unbounded
    /// history is a slow leak.
    int prune(const QString& rootUri, int keep) const;

    /// A stable, filesystem-safe folder name for a uri. Public because it is
    /// how a caller tests membership in storedFolderNames() without any I/O.
    static QString folderNameFor(const QString& rootUri);

private:
    QString folderFor(const QString& rootUri) const;

    QString m_directory;
};

} // namespace mole
