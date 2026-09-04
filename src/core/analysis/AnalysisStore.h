#pragma once

#include "core/analysis/AnalysisReport.h"
#include "core/tasks/Task.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>

#include <atomic>

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
///
/// **The summaries are read once and kept, and this says when they change.**
/// history() opens and parses every report of a folder, and three callers were
/// doing that on the thread that draws on every `EventBus::directoryChanged` -- a
/// copy, a delete, a rename, a refresh, anywhere -- because that was the only
/// event they had to go on. With fifty reports of a large tree, each holding its
/// top folders, its largest files and an extension breakdown, that is megabytes
/// of JSON parsed per bus event, and on a config directory over a network a
/// stall. `PluginServices::index` already states this rule for the index -- "not
/// for the thread that draws … use indexSummary" -- and the report store had no
/// snapshot. See MOLE-380.
class AnalysisStore : public QObject
{
    Q_OBJECT

public:
    /// How many runs to keep per directory.
    ///
    /// One figure, because there were two: the analysis tab kept 50 and
    /// AnalysisJob defaulted to 30, and nothing ever called setHistoryKept(). So
    /// a folder with 45 manual runs lost runs 31 to 45 the first night its
    /// schedule fired -- and AnalysisJob's own header says it "deliberately
    /// shares the store with the analysis tab … or the two would drift apart and
    /// the diffs would lie". The depth is the one parameter that drifted.
    static constexpr int kHistoryKept = 50;

    /// `MOLE_ANALYSIS_PATH` wins, so tests never touch the user's history.
    static QString defaultDirectory();

    explicit AnalysisStore(QString directory, QObject* parent = nullptr);

    const QString& directory() const { return m_directory; }

    /// False when the report did not reach the disk. It used to be called as a
    /// statement by both callers, so a report a user waited for was announced as
    /// finished and was not there at the next start. See ADR-0089.
    [[nodiscard]] bool save(const AnalysisReport& report);

    /// Newest first. Missing or unreadable files are skipped rather than
    /// failing the whole listing.
    ///
    /// Read from disk once per folder and kept; every later ask is free until
    /// something here changes it. See the note on the class.
    QList<ReportSummary> history(const QString& rootUri) const;
    /// Every directory that has ever been analysed. Kept, like history().
    QStringList analysedRoots() const;
    /// Forgets what is kept, so the next ask goes to disk.
    ///
    /// For a caller that has reason to think the directory changed behind this
    /// object's back -- somebody pressing refresh, or a test that wrote a report
    /// file itself. Nothing else needs it: every write through this object
    /// invalidates what it touched and says so.
    void forgetSummaries();

    /// Reads every folder's summaries into memory. Safe from a pool thread, and
    /// meant for one: this is the read that costs.
    void readEverything();

    /// Names the thread that must not read from disk here.
    ///
    /// The same guard IndexDatabase has, for the same reason and after the same
    /// fault: this is a directory of JSON files, and reading a folder's history
    /// parses every one of them. Set once by the host; a read on that thread
    /// warns without anybody turning a logging category on, because it is a
    /// programming fault rather than an operational fact. See ADR-0066 for the
    /// index's version of this and MOLE-380 for the report store's.
    void doNotReadFrom(QThread* thread);

    /// The hashed folder names present on disk, without reading any of them.
    ///
    /// This is what makes "does this row have a report?" affordable for a
    /// listing of five thousand entries: one directory read, then a hash per
    /// row and a set lookup, rather than a file open per row.
    QSet<QString> storedFolderNames() const;

    AnalysisReport load(const QString& rootUri, const QString& id) const;
    /// The most recent report for a directory, or an invalid one.
    AnalysisReport latest(const QString& rootUri) const;

    bool remove(const QString& rootUri, const QString& id);
    /// Drops the oldest reports beyond `keep`. History is useful; unbounded
    /// history is a slow leak.
    int prune(const QString& rootUri, int keep);

    /// A stable, filesystem-safe folder name for a uri. Public because it is
    /// how a caller tests membership in storedFolderNames() without any I/O.
    static QString folderNameFor(const QString& rootUri);

signals:
    /// A report for this folder was filed, removed or pruned.
    ///
    /// What the Reports tab, the browser's folder facts and the analysis tab
    /// rebuild on. They used to rebuild on `EventBus::directoryChanged`, which is
    /// neither necessary nor sufficient: it fires for every copy anywhere, and it
    /// does not fire when the scheduler files a report -- so the comment saying
    /// "a run filed by the scheduler appears here without the user reopening the
    /// tab" was true only when some directory also happened to change.
    void changed(const QString& rootUri);

private:
    QString folderFor(const QString& rootUri) const;
    /// Reads one folder's summaries from disk. Called once per folder.
    QList<ReportSummary> readHistory(const QString& rootUri) const;
    QStringList readRoots() const;
    void checkNotOnTheDrawingThread(const char* what) const;

    QString m_directory;
    /// What has been read. Mutable because reading is a const question with a
    /// cost, which is exactly what a cache is for. Guarded, because it is filled
    /// on a pool thread and read on the one that draws.
    mutable QMutex m_lock;
    mutable QHash<QString, QList<ReportSummary>> m_history;
    mutable QStringList m_roots;
    mutable bool m_rootsKnown = false;
    std::atomic<QThread*> m_noReadsFrom { nullptr };
};

/// Reads every saved report's summary, off the thread that draws.
///
/// The store keeps what it read, so this is the one expensive pass and everything
/// afterwards is free until a report is filed. Beside the store rather than in
/// `core/tasks` because it is the store's own read and nothing else would use it.
class ReadReportSummariesTask final : public Task
{
    Q_OBJECT

public:
    explicit ReadReportSummariesTask(AnalysisStore* store, QObject* parent = nullptr);

protected:
    void run() override;

private:
    AnalysisStore* m_store = nullptr;
};

} // namespace mole
