#include "core/index/IndexDatabase.h"

#include "core/data/SqliteError.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <QVariant>

namespace mole {
namespace {

    constexpr int kSchemaVersion = 5;

    /// How long a write waits for another connection before giving up. Named
    /// because the sentence a reader gets says how long that was -- see
    /// sqlite::describe().
    constexpr int kBusyTimeoutMs = 5000;

} // namespace

IndexDatabase::IndexDatabase(QString filePath)
    : m_filePath(std::move(filePath))
    , m_connections(m_filePath,
          sqlite::Connection::Settings {
              .readOnly = false,
              // WAL is what lets a scan write while the interface reads, and it
              // is the whole of ADR-0065. An index parked on a filesystem that
              // will not have it -- MOLE_INDEX_PATH pointing at a network mount
              // -- used to run in rollback-journal mode with nothing said, so
              // readers queued behind writers again and the fault was back
              // invisibly. Now it refuses to open and says which filesystem.
              .requireWal = true,
              .busyTimeoutMs = kBusyTimeoutMs,
              .pragmas
              = { QStringLiteral("PRAGMA synchronous=NORMAL"), QStringLiteral("PRAGMA foreign_keys=ON") },
          })
{
}

IndexDatabase::~IndexDatabase()
{
    close();
}

QString IndexDatabase::defaultFilePath()
{
    // An explicit override keeps tests and throwaway sessions out of the real
    // profile, and gives users a way to park a large index on another disk.
    const QByteArray override = qgetenv("MOLE_INDEX_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("index.sqlite"));
}

Result<void> IndexDatabase::open()
{
    QMutexLocker lock(&m_writers);
    if (m_open)
        return {};

    const QFileInfo info(m_filePath);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath())) {
        return Result<void>::failure(VfsError::IoError,
            QStringLiteral("Cannot create index directory %1").arg(info.dir().absolutePath()));
    }

    m_open = true;
    if (!connectionForCurrentThread().isOpen()) {
        m_open = false;
        // With what the driver said, which used to be dropped on the floor --
        // "Cannot open index /..." and nothing about why. See MOLE-306 for the
        // shape of that complaint and MOLE-356 for the last of it.
        const QString why = m_connections.lastError();
        return Result<void>::failure(VfsError::IoError,
            why.isEmpty() ? QStringLiteral("Cannot open index %1").arg(m_filePath)
                          : QStringLiteral("Cannot open index %1: %2").arg(m_filePath, why));
    }
    lock.unlock();

    Result<void> migration = applyMigrations();
    if (!migration.ok()) {
        close();
        return migration;
    }
    return {};
}

void IndexDatabase::close()
{
    QMutexLocker lock(&m_writers);
    if (!m_open)
        return;
    m_open = false;

    // Close every thread's connection. Safe only once no task can still be
    // using the index -- the application tears down its TaskManager first.
    // That contract is what makes this safe now that a reader holds no lock of
    // ours while its query runs: nothing here waits for one to finish.
    m_connections.closeAll();
}

void IndexDatabase::doNotQueryFrom(QThread* thread)
{
    m_noQueriesFrom = thread;
}

void IndexDatabase::checkNotOnTheDrawingThread(const char* what) const
{
    if (m_noQueriesFrom.load() != QThread::currentThread())
        return;
    // Not qCWarning: this is a programming fault rather than an operational
    // fact, and it should be visible without anybody turning a category on.
    qWarning("Index query on the thread that draws the window: %s. Read IndexSummary, "
             "or ask through a task -- see ADR-0066.",
        what);
}

bool IndexDatabase::isOpen() const
{
    // Not guarded: this reads an atomic and touches no database, so the
    // interface is welcome to ask it as often as it likes.
    return m_open;
}

Result<void> IndexDatabase::sqlError(const QSqlQuery& query, const QString& context)
{
    // The query's error and not the connection's, which is the difference between
    // an explanation and a colon with nothing after it. A statement that fails
    // records its error on itself: QSqlDatabase::lastError() is about opening,
    // transactions and commits, and for a failed INSERT it is empty -- so
    // "Inserting indexed file: " is what every write failure in this file used to
    // report. Found writing the case for MOLE-306, which was about the *wording*
    // of a locked database and could say nothing at all here until this was fixed.
    return Result<void>::failure(VfsError::IoError,
        QStringLiteral("%1: %2").arg(context, sqlite::describe(query.lastError(), kBusyTimeoutMs)));
}

Result<void> IndexDatabase::sqlError(const QSqlDatabase& db, const QString& context)
{
    // Every write failure in this file arrives here, and until MOLE-306 every one
    // of them arrived as the driver's own text: "database is locked Unable to
    // fetch row" behind whatever this was doing. A locked index is another
    // connection holding the file and waiting is the answer, which is worth
    // saying, and it is not what a full disk needs.
    return Result<void>::failure(VfsError::IoError,
        QStringLiteral("%1: %2").arg(context, sqlite::describe(db.lastError(), kBusyTimeoutMs)));
}

QString IndexDatabase::pragmaValue(const QString& name) const
{
    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return {};
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA %1").arg(name)) || !query.next())
        return {};
    return query.value(0).toString();
}

Result<void> IndexDatabase::noConnection() const
{
    // sqlError(db, ...) cannot say anything here. The QSqlDatabase is the
    // invalid one forCurrentThread() hands back when it could not open, and an
    // invalid connection has no lastError -- which is how thirteen callers came
    // to report "No index connection for this thread: " with nothing after the
    // colon. The reason belongs to the thing that tried to open it.
    const QString why = m_connections.lastError();
    return Result<void>::failure(VfsError::IoError,
        why.isEmpty() ? QStringLiteral("No index connection for this thread")
                      : QStringLiteral("No index connection for this thread: %1").arg(why));
}

Result<void> IndexDatabase::applyMigrations()
{
    QMutexLocker lock(&m_writers);
    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection();

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA user_version")))
        return sqlError(query, QStringLiteral("Reading schema version"));

    int version = 0;
    if (query.next())
        version = query.value(0).toInt();

    if (version == kSchemaVersion)
        return {};

    // A file from the future. The credential store already refuses one and says
    // so; this accepted it without a word and then wrote to a schema it does not
    // understand, which is how an index gets corrupted by an older build that
    // was only trying to help. Refusing is the same answer for the same reason:
    // there is nothing safe to do with columns nobody here has heard of, and a
    // read-only opening would be a second mode to keep working for ever.
    // See ADR-0093.
    if (version > kSchemaVersion) {
        return Result<void>::failure(VfsError::NotSupported,
            QStringLiteral("This index was written by a newer version of Mole (schema %1, this build "
                           "understands %2)")
                .arg(version)
                .arg(kSchemaVersion));
    }

    // Each block runs on its own so a database part way up the sequence gets
    // only what it is missing, and each is one transaction -- *including the
    // line that records it happened*, which is the whole of MOLE-356's first
    // fault. The version used to be written once, after every block, so a
    // failure in block 3 left user_version at 0 with blocks 1 and 2 applied.
    // The next open re-ran block 2, `ALTER TABLE files ADD COLUMN generation`
    // failed with "duplicate column name", and every open from then on failed
    // the same way until the user deleted the file. Written inside the
    // transaction, a block either happened and is recorded or did neither.
    const auto apply = [&db](int number, const QStringList& statements) -> Result<void> {
        if (!db.transaction())
            return sqlError(db, QStringLiteral("Starting migration %1").arg(number));
        for (const QString& statement : statements) {
            if (statement.isEmpty())
                continue; // a column that is already there -- see addColumn() below
            QSqlQuery migration(db);
            if (!migration.exec(statement)) {
                db.rollback();
                return sqlError(migration, QStringLiteral("Applying migration %1").arg(number));
            }
        }
        // PRAGMA user_version is a header write and takes part in the
        // transaction like any other, which is what makes this one step.
        QSqlQuery bump(db);
        if (!bump.exec(QStringLiteral("PRAGMA user_version=%1").arg(number))) {
            db.rollback();
            return sqlError(bump, QStringLiteral("Recording migration %1").arg(number));
        }
        if (!db.commit())
            return sqlError(db, QStringLiteral("Committing migration %1").arg(number));
        return {};
    };

    // `ALTER TABLE ... ADD COLUMN`, unless the column is already there.
    //
    // SQLite has no IF NOT EXISTS for a column, and every other statement in
    // these blocks has one -- which is why re-running a block only ever failed
    // on an ALTER. An index that an older Mole left half migrated has the column
    // and no record of it, and the block's job is that the column exists rather
    // than that this particular statement ran. Nothing was lost by running the
    // statement blind while the version was written once at the end; with the
    // version written per block, this is what lets the wreckage of an
    // interrupted upgrade be finished rather than refused for ever.
    const auto addColumn
        = [&db](const QString& table, const QString& name, const QString& definition) -> QString {
        QSqlQuery info(db);
        if (info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            while (info.next()) {
                if (info.value(1).toString().compare(name, Qt::CaseInsensitive) == 0)
                    return {};
            }
        }
        return QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, name, definition);
    };

    // Migrations are append-only: add a new `if (version < N)` block and bump
    // kSchemaVersion. Never edit an existing block -- users have it applied.
    if (version < 1) {
        const QStringList statements = {
            QStringLiteral(R"(
                CREATE TABLE IF NOT EXISTS volumes (
                    id        INTEGER PRIMARY KEY AUTOINCREMENT,
                    root_uri  TEXT NOT NULL UNIQUE,
                    label     TEXT NOT NULL,
                    last_scan INTEGER,
                    file_count INTEGER NOT NULL DEFAULT 0
                ))"),
            QStringLiteral(R"(
                CREATE TABLE IF NOT EXISTS files (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    volume_id   INTEGER NOT NULL REFERENCES volumes(id) ON DELETE CASCADE,
                    name        TEXT NOT NULL,
                    name_folded TEXT NOT NULL,
                    path        TEXT NOT NULL,
                    parent_path TEXT NOT NULL,
                    extension   TEXT,
                    is_dir      INTEGER NOT NULL DEFAULT 0,
                    size        INTEGER NOT NULL DEFAULT 0,
                    mtime       INTEGER NOT NULL DEFAULT 0
                ))"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_name ON files(name_folded)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_volume ON files(volume_id)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_ext ON files(extension)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_size ON files(size)"),
        };

        if (Result<void> applied = apply(1, statements); !applied.ok())
            return applied;
    }

    if (version < 2) {
        // A scan writes its rows beside the ones already there and swaps them
        // in when it finishes; see the scanning section of the header for what
        // the two generation columns mean to a search.
        //
        // Both default to nought, which is what every row and every volume
        // already in an index will read as -- so an index built before this
        // migration stays visible in full, and its next rescan is the first
        // one to hand out a generation.
        const QStringList statements = {
            addColumn(QStringLiteral("files"), QStringLiteral("generation"),
                QStringLiteral("INTEGER NOT NULL DEFAULT 0")),
            addColumn(QStringLiteral("volumes"), QStringLiteral("generation"),
                QStringLiteral("INTEGER NOT NULL DEFAULT 0")),
            addColumn(QStringLiteral("volumes"), QStringLiteral("next_generation"),
                QStringLiteral("INTEGER NOT NULL DEFAULT 0")),
            // Every search joins on this pair, and so does the sweep that ends
            // a scan; without it both are a scan of the volume's whole extent.
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_generation "
                           "ON files(volume_id, generation)"),
        };

        if (Result<void> applied = apply(2, statements); !applied.ok())
            return applied;
    }

    if (version < 3) {
        // What a file says about itself. Key and value rather than a column
        // apiece, because the fields come from readers that plugins may add and
        // a schema migration per new EXIF tag is not a design.
        //
        // Numbers go in their own column so a range is a range in SQL; text
        // goes in the other; a fact is written to whichever fits and to both
        // when it makes sense -- an exposure is text to read and a number to
        // compare.
        const QStringList statements = {
            QStringLiteral(R"(
                CREATE TABLE IF NOT EXISTS file_facts (
                    file_id   INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
                    key       TEXT NOT NULL,
                    text      TEXT,
                    num       REAL
                ))"),
            // One index per way of asking: a camera by name, an ISO by range.
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_facts_key_text ON file_facts(key, text)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_facts_key_num ON file_facts(key, num)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_facts_file ON file_facts(file_id)"),
        };

        if (Result<void> applied = apply(3, statements); !applied.ok())
            return applied;
    }

    if (version < 4) {
        // A row that is not addressed the way its volume is: a file inside an
        // archive. Null for everything else, which is almost every row, so the
        // column costs a byte apiece and answers a question nothing else can.
        const QStringList statements = {
            addColumn(QStringLiteral("files"), QStringLiteral("uri"), QStringLiteral("TEXT")),
        };
        if (Result<void> applied = apply(4, statements); !applied.ok())
            return applied;
    }

    if (version < 5) {
        // What the scan that built a volume was asked for, so a list of indexes
        // can say which of them can answer a question about a camera and which
        // cannot. Nullable on purpose: a volume written before this has no
        // recorded options, and "not known" is the honest answer for it rather
        // than "no metadata", which would be a lie about a tree indexed with it.
        const QStringList statements = {
            addColumn(
                QStringLiteral("volumes"), QStringLiteral("scan_incremental"), QStringLiteral("INTEGER")),
            addColumn(QStringLiteral("volumes"), QStringLiteral("scan_metadata"), QStringLiteral("INTEGER")),
            addColumn(QStringLiteral("volumes"), QStringLiteral("scan_archives"), QStringLiteral("INTEGER")),
        };
        if (Result<void> applied = apply(5, statements); !applied.ok())
            return applied;
    }

    return {};
}

Result<qint64> IndexDatabase::upsertVolume(const VfsUri& root, const QString& label)
{
    checkNotOnTheDrawingThread("upsertVolume()");
    QMutexLocker lock(&m_writers);
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection().error();
    const QString rootUri = root.toString();

    QSqlQuery select(db);
    select.prepare(QStringLiteral("SELECT id FROM volumes WHERE root_uri = ?"));
    select.addBindValue(rootUri);
    if (!select.exec())
        return sqlError(select, QStringLiteral("Looking up volume")).error();
    if (select.next())
        return select.value(0).toLongLong();

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral("INSERT INTO volumes (root_uri, label) VALUES (?, ?)"));
    insert.addBindValue(rootUri);
    insert.addBindValue(label.isEmpty() ? rootUri : label);
    if (!insert.exec())
        return sqlError(insert, QStringLiteral("Creating volume")).error();

    return insert.lastInsertId().toLongLong();
}

Result<void> IndexDatabase::removeVolume(qint64 volumeId)
{
    checkNotOnTheDrawingThread("removeVolume()");
    QMutexLocker lock(&m_writers);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection();
    // One transaction, because half of it is worse than neither. The rows going
    // without the volume leaves an index that says a drive holds nothing; the
    // volume going without its rows leaves rows no search can reach and nothing
    // will ever clear, because the sweep that clears them is per volume.
    if (!db.transaction())
        return sqlError(db, QStringLiteral("Removing volume"));

    QSqlQuery files(db);
    files.prepare(QStringLiteral("DELETE FROM files WHERE volume_id = ?"));
    files.addBindValue(volumeId);
    if (!files.exec()) {
        db.rollback();
        return sqlError(files, QStringLiteral("Removing volume files"));
    }

    QSqlQuery volume(db);
    volume.prepare(QStringLiteral("DELETE FROM volumes WHERE id = ?"));
    volume.addBindValue(volumeId);
    if (!volume.exec()) {
        db.rollback();
        return sqlError(volume, QStringLiteral("Removing volume"));
    }

    if (!db.commit()) {
        db.rollback();
        return sqlError(db, QStringLiteral("Removing volume"));
    }
    return {};
}

Result<qint64> IndexDatabase::beginScan(qint64 volumeId)
{
    checkNotOnTheDrawingThread("beginScan()");
    QMutexLocker lock(&m_writers);
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection().error();
    if (!db.transaction())
        return sqlError(db, QStringLiteral("Opening a scan")).error();

    QSqlQuery take(db);
    take.prepare(QStringLiteral("UPDATE volumes SET next_generation = next_generation + 1 WHERE id = ?"));
    take.addBindValue(volumeId);
    if (!take.exec()) {
        db.rollback();
        return sqlError(take, QStringLiteral("Taking a scan generation")).error();
    }

    QSqlQuery read(db);
    read.prepare(QStringLiteral("SELECT generation, next_generation FROM volumes WHERE id = ?"));
    read.addBindValue(volumeId);
    // A statement that failed and a statement that found nothing were the same
    // answer here, so a locked index reported "No indexed volume 3" -- a fact
    // about the database being busy, told as a fact about the drive not being
    // in it. Whoever read that went looking for a volume that was there.
    if (!read.exec()) {
        db.rollback();
        return sqlError(read, QStringLiteral("Opening a scan")).error();
    }
    if (!read.next()) {
        db.rollback();
        return VfsError::make(VfsError::NotFound, QStringLiteral("No indexed volume %1").arg(volumeId));
    }
    const qint64 visible = read.value(0).toLongLong();
    const qint64 generation = read.value(1).toLongLong();

    // Rows from a scan that never finished: one killed with the process leaves
    // them behind, because nothing ran afterwards to drop them. No search can
    // reach them and no commit will ever claim them, so this is where they go
    // -- otherwise a database grows by a dead scan every time one is killed.
    QSqlQuery sweep(db);
    sweep.prepare(QStringLiteral("DELETE FROM files WHERE volume_id = ? AND generation <> ?"));
    sweep.addBindValue(volumeId);
    sweep.addBindValue(visible);
    if (!sweep.exec()) {
        db.rollback();
        return sqlError(sweep, QStringLiteral("Clearing an abandoned scan")).error();
    }

    if (!db.commit()) {
        db.rollback();
        return sqlError(db, QStringLiteral("Opening a scan")).error();
    }
    return generation;
}

Result<QHash<QString, qint64>> IndexDatabase::directoryTimes(qint64 volumeId) const
{
    checkNotOnTheDrawingThread("directoryTimes()");
    // No lock. WAL gives this read its own snapshot, which is the whole
    // reason it is turned on -- see ADR-0065. A locker added back here is
    // a scan starving the window again.
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection().error();

    QSqlQuery query(db);
    // Only folders that were already settled when the last scan ran.
    //
    // A folder changed in the same second the scan read it has the scan's own
    // timestamp and would look unchanged for ever after. Requiring it to be
    // strictly older costs one clause and closes a window that is small,
    // permanent, and impossible to notice from the outside.
    query.prepare(QStringLiteral("SELECT f.path, f.mtime FROM files f "
                                 "JOIN volumes v ON v.id = f.volume_id AND v.generation = f.generation "
                                 "WHERE f.volume_id = ? AND f.is_dir = 1 "
                                 "AND v.last_scan IS NOT NULL AND f.mtime < v.last_scan"));
    query.addBindValue(volumeId);
    if (!query.exec())
        return sqlError(query, QStringLiteral("Reading the last scan's folders")).error();

    QHash<QString, qint64> times;
    while (query.next())
        times.insert(query.value(0).toString(), query.value(1).toLongLong());
    return times;
}

Result<qint64> IndexDatabase::carryForward(qint64 volumeId, qint64 generation, const QString& path)
{
    checkNotOnTheDrawingThread("carryForward()");
    QMutexLocker lock(&m_writers);
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection().error();
    if (!db.transaction())
        return sqlError(db, QStringLiteral("Carrying a subtree forward")).error();

    // The rows first, then their facts against the copies -- a fact belongs to
    // a row and the copy is a different row.
    QSqlQuery rows(db);
    rows.prepare(QStringLiteral(
        "INSERT INTO files (volume_id, generation, name, name_folded, path, parent_path, extension, "
        "is_dir, size, mtime, uri) "
        "SELECT f.volume_id, ?, f.name, f.name_folded, f.path, f.parent_path, f.extension, "
        "f.is_dir, f.size, f.mtime, f.uri "
        "FROM files f JOIN volumes v ON v.id = f.volume_id AND v.generation = f.generation "
        // Under it, not it: the walk saw the folder itself and has already
        // written its own row, which is the only reason it knows to carry the
        // rest across.
        "WHERE f.volume_id = ? AND f.path LIKE ? ESCAPE '\\'"));
    rows.addBindValue(generation);
    rows.addBindValue(volumeId);
    QString prefix = path;
    prefix.replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
        .replace(QLatin1Char('%'), QStringLiteral("\\%"))
        .replace(QLatin1Char('_'), QStringLiteral("\\_"));
    rows.addBindValue(prefix + QStringLiteral("/%"));
    if (!rows.exec()) {
        db.rollback();
        return sqlError(rows, QStringLiteral("Carrying rows forward")).error();
    }
    const qint64 carried = rows.numRowsAffected();

    QSqlQuery facts(db);
    facts.prepare(QStringLiteral("INSERT INTO file_facts (file_id, key, text, num) "
                                 "SELECT copy.id, m.key, m.text, m.num FROM file_facts m "
                                 "JOIN files old ON old.id = m.file_id "
                                 "JOIN volumes v ON v.id = old.volume_id AND v.generation = old.generation "
                                 "JOIN files copy ON copy.volume_id = old.volume_id AND copy.generation = ? "
                                 "AND copy.path = old.path "
                                 "WHERE old.volume_id = ? AND old.path LIKE ? ESCAPE '\\'"));
    facts.addBindValue(generation);
    facts.addBindValue(volumeId);
    facts.addBindValue(prefix + QStringLiteral("/%"));
    if (!facts.exec()) {
        db.rollback();
        return sqlError(facts, QStringLiteral("Carrying a subtree's facts forward")).error();
    }

    if (!db.commit()) {
        db.rollback();
        return sqlError(db, QStringLiteral("Carrying a subtree forward")).error();
    }
    return carried;
}

Result<void> IndexDatabase::commitScan(
    qint64 volumeId, qint64 generation, const QDateTime& when, const ScanOptions& options)
{
    checkNotOnTheDrawingThread("commitScan()");
    QMutexLocker lock(&m_writers);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection();
    // One transaction for the whole swap. A search runs either before it or
    // after it; there is no moment at which the volume holds some of each, and
    // a process that dies part way through leaves the previous scan intact.
    if (!db.transaction())
        return sqlError(db, QStringLiteral("Committing a scan"));

    QSqlQuery previous(db);
    previous.prepare(QStringLiteral("DELETE FROM files WHERE volume_id = ? AND generation <> ?"));
    previous.addBindValue(volumeId);
    previous.addBindValue(generation);
    if (!previous.exec()) {
        db.rollback();
        return sqlError(previous, QStringLiteral("Dropping the previous scan"));
    }

    QSqlQuery swap(db);
    swap.prepare(QStringLiteral(
        "UPDATE volumes SET generation = ?, last_scan = ?, "
        "scan_incremental = ?, scan_metadata = ?, scan_archives = ?, "
        "file_count = (SELECT COUNT(*) FROM files WHERE volume_id = ? AND generation = ?) WHERE id = ?"));
    swap.addBindValue(generation);
    swap.addBindValue(when.toSecsSinceEpoch());
    swap.addBindValue(options.incremental ? 1 : 0);
    swap.addBindValue(options.metadata ? 1 : 0);
    swap.addBindValue(options.archives ? 1 : 0);
    swap.addBindValue(volumeId);
    swap.addBindValue(generation);
    swap.addBindValue(volumeId);
    if (!swap.exec()) {
        db.rollback();
        return sqlError(swap, QStringLiteral("Updating volume"));
    }

    if (!db.commit()) {
        db.rollback();
        return sqlError(db, QStringLiteral("Committing a scan"));
    }
    return {};
}

Result<void> IndexDatabase::abandonScan(qint64 volumeId, qint64 generation)
{
    checkNotOnTheDrawingThread("abandonScan()");
    QMutexLocker lock(&m_writers);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection();
    QSqlQuery query(db);
    // Never the generation the volume is currently serving, whatever it is
    // handed. Abandoning a scan is a tidy-up, and a tidy-up that can empty a
    // 4 TB index is the fault this whole arrangement exists to remove.
    query.prepare(QStringLiteral("DELETE FROM files WHERE volume_id = ? AND generation = ? "
                                 "AND generation <> (SELECT generation FROM volumes WHERE id = ?)"));
    query.addBindValue(volumeId);
    query.addBindValue(generation);
    query.addBindValue(volumeId);
    if (!query.exec())
        return sqlError(query, QStringLiteral("Abandoning a scan"));
    return {};
}

Result<QList<IndexVolume>> IndexDatabase::volumes() const
{
    checkNotOnTheDrawingThread("volumes()");
    // No lock. WAL gives this read its own snapshot, which is the whole
    // reason it is turned on -- see ADR-0065. A locker added back here is
    // a scan starving the window again.
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection().error();
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT id, root_uri, label, last_scan, file_count, "
                                   "scan_incremental, scan_metadata, scan_archives "
                                   "FROM volumes ORDER BY label")))
        return sqlError(query, QStringLiteral("Listing volumes")).error();

    QList<IndexVolume> out;
    while (query.next()) {
        IndexVolume v;
        v.id = query.value(0).toLongLong();
        v.rootUri = query.value(1).toString();
        v.label = query.value(2).toString();
        const qint64 scan = query.value(3).toLongLong();
        if (scan > 0)
            v.lastScan = QDateTime::fromSecsSinceEpoch(scan);
        v.fileCount = query.value(4).toLongLong();
        // All three or none: they are written together by one finished scan, so
        // a volume either remembers what it was asked for or it does not.
        if (!query.value(5).isNull()) {
            ScanOptions asked;
            asked.incremental = query.value(5).toBool();
            asked.metadata = query.value(6).toBool();
            asked.archives = query.value(7).toBool();
            v.scan = asked;
        }
        out.append(v);
    }
    return out;
}

Result<void> IndexDatabase::insertBatch(qint64 volumeId, qint64 generation, const QList<IndexedFile>& files)
{
    checkNotOnTheDrawingThread("insertBatch()");
    if (files.isEmpty())
        return {};

    QMutexLocker lock(&m_writers);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection();
    if (!db.transaction())
        return sqlError(db, QStringLiteral("Starting insert batch"));

    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT INTO files (volume_id, generation, name, name_folded, path, "
                                 "parent_path, extension, is_dir, size, mtime, uri) "
                                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    QSqlQuery fact(db);
    fact.prepare(QStringLiteral("INSERT INTO file_facts (file_id, key, text, num) VALUES (?, ?, ?, ?)"));

    for (const IndexedFile& file : files) {
        query.addBindValue(volumeId);
        query.addBindValue(generation);
        query.addBindValue(file.name);
        query.addBindValue(foldForSearch(file.name));
        query.addBindValue(file.path);
        query.addBindValue(file.parentPath);
        query.addBindValue(file.extension);
        query.addBindValue(file.isDir ? 1 : 0);
        query.addBindValue(file.size);
        query.addBindValue(file.modifiedEpoch);
        query.addBindValue(file.uri.isEmpty() ? QVariant() : QVariant(file.uri));
        if (!query.exec()) {
            db.rollback();
            return sqlError(query, QStringLiteral("Inserting indexed file"));
        }

        if (file.facts.isEmpty())
            continue;
        const qint64 fileId = query.lastInsertId().toLongLong();
        for (const SearchFact& one : file.facts) {
            fact.addBindValue(fileId);
            fact.addBindValue(one.key);
            fact.addBindValue(one.text.isEmpty() ? QVariant() : QVariant(one.text));
            fact.addBindValue(one.hasNumber ? QVariant(one.number) : QVariant());
            if (!fact.exec()) {
                db.rollback();
                return sqlError(fact, QStringLiteral("Inserting a file's own facts"));
            }
        }
    }

    if (!db.commit()) {
        db.rollback();
        return sqlError(db, QStringLiteral("Committing insert batch"));
    }
    return {};
}

Result<QList<IndexSearchHit>> IndexDatabase::search(const SearchQuery& query) const
{
    checkNotOnTheDrawingThread("search()");
    // No lock. WAL gives this read its own snapshot, which is the whole
    // reason it is turned on -- see ADR-0065. A locker added back here is
    // a scan starving the window again.
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection().error();

    // The generations having to agree is what keeps a scan in progress out of
    // the answer: its rows are in the table and belong to no volume yet.
    QString sql = QStringLiteral(
        "SELECT f.name, v.root_uri, f.path, f.parent_path, f.is_dir, f.size, f.mtime, v.label, f.uri "
        "FROM files f JOIN volumes v ON v.id = f.volume_id AND v.generation = f.generation WHERE 1=1");
    // f.id is selected by nothing and needed by the fact subqueries below.
    QVariantList bindings;

    // Named rather than called inline. A range-for extends the lifetime of the range
    // expression's *result*, not of the temporaries that produced it -- and
    // pushedDown() hands back a reference into the plan, so a plan built on that line
    // is destroyed before the first iteration and the loop walks a QList whose
    // storage is gone. It reads as working, because the freed stack still holds the
    // old bytes; what it does instead is decided by whatever the compiler puts there
    // next. See MOLE-185.
    const SearchPlan plan = planSearch(query, SearchSource::Index);
    for (const SearchPredicate& predicate : plan.pushedDown()) {
        switch (predicate.field) {
        case SearchPredicate::Field::Name:
            // instr() instead of LIKE: no wildcard escaping to get wrong, and
            // it lets the case-insensitive path use our Unicode-folded column.
            if (predicate.caseSensitive) {
                sql += QStringLiteral(" AND instr(f.name, ?) > 0");
                bindings.append(predicate.text);
            } else {
                sql += QStringLiteral(" AND instr(f.name_folded, ?) > 0");
                bindings.append(foldForSearch(predicate.text));
            }
            break;
        case SearchPredicate::Field::Extension: {
            // A list, so a search for photographs can say jpg, jpeg and heic
            // and mean one question. (`slots` would be a Qt macro.)
            QStringList placeholders;
            for (const QString& one : predicate.list) {
                placeholders.append(QStringLiteral("?"));
                bindings.append(one);
            }
            sql += QStringLiteral(" AND f.extension IN (%1)").arg(placeholders.join(QStringLiteral(", ")));
            break;
        }
        case SearchPredicate::Field::Size:
            sql += predicate.match == SearchPredicate::Match::AtMost ? QStringLiteral(" AND f.size <= ?")
                                                                     : QStringLiteral(" AND f.size >= ?");
            bindings.append(predicate.number);
            break;
        case SearchPredicate::Field::Kind:
            sql += QStringLiteral(" AND f.is_dir = ?");
            bindings.append(predicate.flag ? 1 : 0);
            break;
        case SearchPredicate::Field::Metadata: {
            // An EXISTS over the fact table rather than a join, so a file with
            // two cameras named in it comes back once. Both indexes are on
            // (key, …), so the key narrows first whichever way it is asked.
            const QString key = predicate.list.value(0);
            if (predicate.match == SearchPredicate::Match::AtLeast
                || predicate.match == SearchPredicate::Match::AtMost) {
                sql += QStringLiteral(" AND EXISTS (SELECT 1 FROM file_facts m WHERE m.file_id = f.id "
                                      "AND m.key = ? AND m.num %1 ?)")
                           .arg(predicate.match == SearchPredicate::Match::AtMost ? QStringLiteral("<=")
                                                                                  : QStringLiteral(">="));
                bindings.append(key);
                bindings.append(predicate.numberValue);
            } else {
                sql += QStringLiteral(" AND EXISTS (SELECT 1 FROM file_facts m WHERE m.file_id = f.id "
                                      "AND m.key = ? AND instr(lower(m.text), ?) > 0)");
                bindings.append(key);
                bindings.append(foldForSearch(predicate.text));
            }
            break;
        }
        case SearchPredicate::Field::Modified:
            // The column the scan has been filling in since there was a scan.
            // Seconds since the epoch on both sides, and a row that has no date
            // -- mtime defaults to nought -- answers "before" and not "after",
            // which is what the walk does with an invalid QDateTime. See
            // MOLE-371.
            sql += predicate.match == SearchPredicate::Match::AtMost
                ? QStringLiteral(" AND f.mtime > 0 AND f.mtime <= ?")
                : QStringLiteral(" AND f.mtime >= ?");
            bindings.append(predicate.number);
            break;
        case SearchPredicate::Field::Created:
        case SearchPredicate::Field::Accessed:
        case SearchPredicate::Field::Hidden:
        case SearchPredicate::Field::TypeClass:
        case SearchPredicate::Field::Under: {
            // Answered on f.path, which holds where a row sits within its
            // volume -- including a member of an archive, whose path is written
            // under its container as <container>!<inside>. The uri column cannot
            // answer it: a member's uri is on the archive's own authority.
            //
            // instr(x, y) = 1 rather than LIKE, for the same reason the name
            // clause uses it: no wildcard escaping to get wrong. The volume is
            // asked as well, because two drives can hold the same path and a
            // uri names one of them.
            //
            // Two arms, because a folder and a volume can contain each other
            // either way round. A volume that *is* the folder or sits inside it
            // -- somebody scanned one subtree and is searching the tree above it
            // -- answers with all of its rows and no path test at all. A volume
            // that contains the folder answers with the rows whose path is
            // under it.
            const QString folder = VfsUri::fromString(predicate.text).path();
            sql += QStringLiteral(" AND ((v.root_uri = ? OR instr(v.root_uri, ?) = 1)"
                                  " OR (instr(?, v.root_uri) = 1"
                                  " AND (f.path = ? OR instr(f.path, ?) = 1 OR instr(f.path, ?) = 1)))");
            bindings.append(predicate.text);
            bindings.append(predicate.text + QLatin1Char('/'));
            bindings.append(predicate.text);
            bindings.append(folder);
            bindings.append(folder + QLatin1Char('/'));
            // A folder-scoped search whose folder *is* an archive: its members
            // are under it by the same reading, spelled with the separator that
            // says "inside this container".
            bindings.append(folder + QLatin1Char('!'));
            break;
        }
        case SearchPredicate::Field::Path:
            // Never pushed down; the planner does not hand these over.
            break;
        }
    }

    if (query.volumeId >= 0) {
        sql += QStringLiteral(" AND f.volume_id = ?");
        bindings.append(query.volumeId);
    }

    sql += QStringLiteral(" ORDER BY f.is_dir DESC, f.name COLLATE NOCASE LIMIT ?");
    bindings.append(query.limit > 0 ? query.limit : SearchQuery {}.limit);

    QSqlQuery statement(db);
    statement.prepare(sql);
    for (const QVariant& value : bindings)
        statement.addBindValue(value);

    if (!statement.exec())
        return sqlError(statement, QStringLiteral("Searching index")).error();

    QList<IndexSearchHit> hits;
    while (statement.next()) {
        IndexSearchHit hit;
        hit.name = statement.value(0).toString();
        const QString own = statement.value(8).toString();
        if (!own.isEmpty()) {
            hit.uri = own;
        } else {
            const VfsUri root = VfsUri::fromString(statement.value(1).toString());
            hit.uri = VfsUri(root.scheme(), root.authority(), statement.value(2).toString()).toString();
        }
        hit.parentPath = statement.value(3).toString();
        hit.isDir = statement.value(4).toInt() != 0;
        hit.size = statement.value(5).toLongLong();
        hit.modifiedEpoch = statement.value(6).toLongLong();
        hit.volumeLabel = statement.value(7).toString();
        hits.append(hit);
    }
    return hits;
}

Result<QStringList> IndexDatabase::factKeys(qint64 volumeId) const
{
    checkNotOnTheDrawingThread("factKeys()");
    // No lock. WAL gives this read its own snapshot, which is the whole
    // reason it is turned on -- see ADR-0065. A locker added back here is
    // a scan starving the window again.
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection().error();

    // The same generation join a search makes, so a scan in progress cannot
    // offer a field for facts nothing can yet find.
    QString sql = QStringLiteral("SELECT DISTINCT m.key FROM file_facts m "
                                 "JOIN files f ON f.id = m.file_id "
                                 "JOIN volumes v ON v.id = f.volume_id AND v.generation = f.generation");
    QSqlQuery query(db);
    if (volumeId >= 0) {
        query.prepare(sql + QStringLiteral(" WHERE f.volume_id = ? ORDER BY m.key"));
        query.addBindValue(volumeId);
    } else {
        query.prepare(sql + QStringLiteral(" ORDER BY m.key"));
    }
    if (!query.exec())
        return sqlError(query, QStringLiteral("Listing the facts a volume holds")).error();

    QStringList keys;
    while (query.next())
        keys.append(query.value(0).toString());
    return keys;
}

Result<qint64> IndexDatabase::fileCount(qint64 volumeId) const
{
    checkNotOnTheDrawingThread("fileCount()");
    // No lock. WAL gives this read its own snapshot, which is the whole
    // reason it is turned on -- see ADR-0065. A locker added back here is
    // a scan starving the window again.
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return noConnection().error();
    // What a search can reach, on the same join it uses, rather than every row
    // in the table -- a count that included a scan in progress would say the
    // index holds files nothing can find.
    const QString sql = QStringLiteral("SELECT COUNT(*) FROM files f JOIN volumes v "
                                       "ON v.id = f.volume_id AND v.generation = f.generation");
    QSqlQuery query(db);
    if (volumeId >= 0) {
        query.prepare(sql + QStringLiteral(" WHERE f.volume_id = ?"));
        query.addBindValue(volumeId);
    } else {
        query.prepare(sql);
    }
    if (!query.exec())
        return sqlError(query, QStringLiteral("Counting files")).error();
    if (!query.next())
        return qint64 { 0 };
    return query.value(0).toLongLong();
}

} // namespace mole
