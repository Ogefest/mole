#pragma once

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
    QString parentPath;
    QString extension; ///< lowercased, no dot
    bool isDir = false;
    qint64 size = 0;
    qint64 modifiedEpoch = 0; ///< seconds since epoch, 0 when unknown
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

struct IndexSearchQuery
{
    QString text; ///< substring match on the file name
    QString extension; ///< optional exact extension filter
    qint64 volumeId = -1; ///< -1 = all volumes
    bool caseSensitive = false;
    bool includeDirs = true;
    bool includeFiles = true;
    qint64 minSize = -1;
    qint64 maxSize = -1;
    int limit = 5000;
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
    [[nodiscard]] Result<void> clearVolume(qint64 volumeId);
    [[nodiscard]] Result<void> removeVolume(qint64 volumeId);
    [[nodiscard]] Result<void> markVolumeScanned(qint64 volumeId, const QDateTime& when);
    [[nodiscard]] Result<QList<IndexVolume>> volumes() const;

    // ---- writing ---------------------------------------------------------

    /// Inserts one batch inside a single transaction. Call repeatedly from a
    /// scan; keep batches around a couple of thousand rows.
    [[nodiscard]] Result<void> insertBatch(qint64 volumeId, const QList<IndexedFile>& files);

    // ---- reading ---------------------------------------------------------

    [[nodiscard]] Result<QList<IndexSearchHit>> search(const IndexSearchQuery& query) const;
    [[nodiscard]] Result<qint64> fileCount(qint64 volumeId = -1) const;

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
