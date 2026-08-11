#pragma once

#include "core/search/SearchQuery.h"
#include "core/vfs/VfsTypes.h"
#include "core/vfs/VfsUri.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QSqlDatabase>
#include <QString>

class QThread;

namespace mole {

/// One row of the file index.
struct IndexedFile
{
    QString name;
    QString path; ///< absolute path inside the volume, uri-normalised

    /// The whole uri, when this row is not addressed the way its volume is.
    ///
    /// Empty for almost everything: a row's uri is the volume's scheme and
    /// authority with `path` on the end, which is true of every file in a
    /// walked tree. It is not true of a file inside an archive, which lives on
    /// an `archive://` authority naming the container -- and rebuilding that
    /// row's uri from the volume's scheme is how a member came back addressed
    /// as if it sat loose on the disk.
    QString uri;
    QString parentPath;
    QString extension; ///< lowercased, no dot
    bool isDir = false;
    qint64 size = 0;
    qint64 modifiedEpoch = 0; ///< seconds since epoch, 0 when unknown

    /// What the file says about itself, from the readers that fill the details
    /// panel. Only the facts carrying a key ever get here; see ADR-0039.
    ///
    /// Metadata is the one thing worth indexing precisely because the contents
    /// are not: a camera, a lens and a date taken are a few dozen bytes where
    /// the photograph is eight megabytes. That asymmetry is the whole argument.
    QList<SearchFact> facts;
};

/// A previously scanned root, e.g. "the /a/b/c tree on the NAS".
struct IndexVolume
{
    qint64 id = -1;
    QString rootUri;
    QString label;
    QDateTime lastScan;
    qint64 fileCount = 0;
};

struct IndexSearchHit
{
    QString name;
    QString uri;
    QString parentPath;
    bool isDir = false;
    qint64 size = 0;
    qint64 modifiedEpoch = 0;
    QString volumeLabel;
};

/// SQLite-backed catalogue of previously scanned trees. Searching it is what
/// makes "find that file on the 4 TB NAS" instant instead of a network walk.
///
/// CONCURRENCY
/// -----------
/// A QSqlDatabase connection belongs to the thread that opened it and cannot
/// be used from any other -- Qt refuses the call outright. Since scans run on
/// pool threads while the UI searches, this class opens one connection per
/// calling thread on demand and lets SQLite's WAL mode handle the overlap.
/// The mutex guards the connection bookkeeping, not the database itself.
class IndexDatabase
{
public:
    /// `filePath` is a real file; SQLite ":memory:" is not usable because the
    /// scanner and the UI share one connection by design.
    explicit IndexDatabase(QString filePath);
    ~IndexDatabase();

    IndexDatabase(const IndexDatabase&) = delete;
    IndexDatabase& operator=(const IndexDatabase&) = delete;

    /// The conventional location under the user's data directory.
    static QString defaultFilePath();

    [[nodiscard]] Result<void> open();
    void close();
    bool isOpen() const;

    // ---- volumes ---------------------------------------------------------

    /// Creates the volume row or returns the existing one for this root.
    [[nodiscard]] Result<qint64> upsertVolume(const VfsUri& root, const QString& label);
    [[nodiscard]] Result<void> removeVolume(qint64 volumeId);
    [[nodiscard]] Result<QList<IndexVolume>> volumes() const;

    // ---- scanning --------------------------------------------------------
    //
    // A rescan builds the new contents beside the old ones and swaps them in
    // when it finishes. Every row carries the generation of the scan that
    // wrote it, a volume names the one generation that is its contents, and a
    // search reads the rows where the two agree -- so it sees the whole of the
    // previous scan or the whole of the new one, never the half that has been
    // re-walked so far.
    //
    // Emptying the volume first, which is what this used to do, meant a rescan
    // of a large tree answered every search with a fast, confident, short
    // answer for as long as it ran: no error, no warning, just fewer files
    // than there are. The window is hours on the sizes worth indexing.

    /// Opens a scan of `volumeId` and returns the generation its rows carry.
    /// Nothing written under it is visible to a search until commitScan().
    [[nodiscard]] Result<qint64> beginScan(qint64 volumeId);

    /// Inserts one batch inside a single transaction. Call repeatedly from a
    /// scan; keep batches around a couple of thousand rows.
    [[nodiscard]] Result<void> insertBatch(
        qint64 volumeId, qint64 generation, const QList<IndexedFile>& files);

    /// Makes `generation` the volume's contents and records the scan time,
    /// dropping what the previous one left. One transaction, so this instant
    /// is where a search stops seeing the old scan and starts seeing this one.
    [[nodiscard]] Result<void> commitScan(qint64 volumeId, qint64 generation, const QDateTime& when);

    /// Throws away what a scan wrote without committing it. A cancelled or
    /// failed scan leaves the volume exactly as it was, `last_scan` included --
    /// and one killed with the process does too, because nothing it wrote was
    /// ever visible and the next beginScan() sweeps it out.
    [[nodiscard]] Result<void> abandonScan(qint64 volumeId, qint64 generation);

    // ---- reading ---------------------------------------------------------

    /// Answers the part of `query` that SQL can express.
    ///
    /// Only that part: what the planner could not push down is left to whoever
    /// has the entries, because a criterion the database cannot state is one it
    /// must not silently drop. See SearchPlan.
    [[nodiscard]] Result<QList<IndexSearchHit>> search(const SearchQuery& query) const;
    /// How many rows a search can currently reach. Rows an unfinished scan has
    /// written are not among them.
    [[nodiscard]] Result<qint64> fileCount(qint64 volumeId = -1) const;

    /// Which facts this volume's files were recorded as stating, or every
    /// volume's when -1.
    ///
    /// What a form offers has to follow this rather than a list written down in
    /// the form: a plugin that indexes a new fact should get a field without
    /// anybody editing the interface, and a key nothing in scope carries should
    /// not be offered at all.
    [[nodiscard]] Result<QStringList> factKeys(qint64 volumeId = -1) const;

private:
    /// Opens (or reuses) this thread's connection. Callers must hold m_mutex.
    QSqlDatabase connectionForCurrentThread() const;
    Result<void> applyMigrations();
    static Result<void> sqlError(const QSqlDatabase& db, const QString& context);

    QString m_filePath;
    QString m_baseName;
    mutable QMutex m_mutex;
    mutable QHash<QThread*, QString> m_connections;
    mutable int m_nextConnection = 0;
    bool m_open = false;
};

} // namespace mole
