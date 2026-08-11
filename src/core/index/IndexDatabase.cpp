#include "core/index/IndexDatabase.h"

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

    constexpr int kSchemaVersion = 2;

} // namespace

IndexDatabase::IndexDatabase(QString filePath)
    : m_filePath(std::move(filePath))
    , m_baseName(QStringLiteral("mole_index_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

IndexDatabase::~IndexDatabase()
{
    close();
}

QSqlDatabase IndexDatabase::connectionForCurrentThread() const
{
    QThread* thread = QThread::currentThread();

    const auto cached = m_connections.constFind(thread);
    if (cached != m_connections.constEnd()) {
        QSqlDatabase existing = QSqlDatabase::database(*cached, false);
        if (existing.isValid() && existing.isOpen())
            return existing;
        // A pool thread died and its address was recycled, so the cached
        // connection belongs to a thread that no longer exists. Drop it and
        // open a fresh one below.
        m_connections.erase(cached);
    }

    const QString name = QStringLiteral("%1_%2").arg(m_baseName).arg(m_nextConnection++);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(m_filePath);
    if (!db.open()) {
        QSqlDatabase::removeDatabase(name);
        return {};
    }

    QSqlQuery pragma(db);
    // WAL is what lets a scan write while the UI reads. It is a property of
    // the database file, but every connection needs the busy timeout.
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));

    m_connections.insert(thread, name);
    return db;
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
    QMutexLocker lock(&m_mutex);
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
        return Result<void>::failure(
            VfsError::IoError, QStringLiteral("Cannot open index %1").arg(m_filePath));
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
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return;
    m_open = false;

    // Close every thread's connection. Safe only once no task can still be
    // using the index -- the application tears down its TaskManager first.
    QThread* const self = QThread::currentThread();
    for (auto it = m_connections.cbegin(); it != m_connections.cend(); ++it) {
        if (it.key() == self) {
            QSqlDatabase db = QSqlDatabase::database(it.value(), false);
            if (db.isValid())
                db.close();
        }
        // A connection belonging to another thread is dropped by name alone.
        // Fetching the QSqlDatabase object for it from here is precisely what
        // Qt warns about, and it buys nothing: removeDatabase() closes it.
        QSqlDatabase::removeDatabase(it.value());
    }
    m_connections.clear();
}

bool IndexDatabase::isOpen() const
{
    QMutexLocker lock(&m_mutex);
    return m_open;
}

Result<void> IndexDatabase::sqlError(const QSqlDatabase& db, const QString& context)
{
    return Result<void>::failure(
        VfsError::IoError, QStringLiteral("%1: %2").arg(context, db.lastError().text()));
}

Result<void> IndexDatabase::applyMigrations()
{
    QMutexLocker lock(&m_mutex);
    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread"));

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA user_version")))
        return sqlError(db, QStringLiteral("Reading schema version"));

    int version = 0;
    if (query.next())
        version = query.value(0).toInt();

    if (version >= kSchemaVersion)
        return {};

    // Each block runs on its own so a database part way up the sequence gets
    // only what it is missing, and each is one transaction so a migration that
    // fails leaves the schema it started from rather than half of the next.
    const auto apply = [&db](int number, const QStringList& statements) -> Result<void> {
        if (!db.transaction())
            return sqlError(db, QStringLiteral("Starting migration %1").arg(number));
        for (const QString& statement : statements) {
            QSqlQuery migration(db);
            if (!migration.exec(statement)) {
                db.rollback();
                return sqlError(db, QStringLiteral("Applying migration %1").arg(number));
            }
        }
        if (!db.commit())
            return sqlError(db, QStringLiteral("Committing migration %1").arg(number));
        return {};
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
            QStringLiteral("ALTER TABLE files ADD COLUMN generation INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE volumes ADD COLUMN generation INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE volumes ADD COLUMN next_generation INTEGER NOT NULL DEFAULT 0"),
            // Every search joins on this pair, and so does the sweep that ends
            // a scan; without it both are a scan of the volume's whole extent.
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_generation "
                           "ON files(volume_id, generation)"),
        };

        if (Result<void> applied = apply(2, statements); !applied.ok())
            return applied;
    }

    QSqlQuery bump(db);
    if (!bump.exec(QStringLiteral("PRAGMA user_version=%1").arg(kSchemaVersion)))
        return sqlError(db, QStringLiteral("Writing schema version"));

    return {};
}

Result<qint64> IndexDatabase::upsertVolume(const VfsUri& root, const QString& label)
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread")).error();
    const QString rootUri = root.toString();

    QSqlQuery select(db);
    select.prepare(QStringLiteral("SELECT id FROM volumes WHERE root_uri = ?"));
    select.addBindValue(rootUri);
    if (!select.exec())
        return sqlError(db, QStringLiteral("Looking up volume")).error();
    if (select.next())
        return select.value(0).toLongLong();

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral("INSERT INTO volumes (root_uri, label) VALUES (?, ?)"));
    insert.addBindValue(rootUri);
    insert.addBindValue(label.isEmpty() ? rootUri : label);
    if (!insert.exec())
        return sqlError(db, QStringLiteral("Creating volume")).error();

    return insert.lastInsertId().toLongLong();
}

Result<void> IndexDatabase::removeVolume(qint64 volumeId)
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread"));
    QSqlQuery files(db);
    files.prepare(QStringLiteral("DELETE FROM files WHERE volume_id = ?"));
    files.addBindValue(volumeId);
    if (!files.exec())
        return sqlError(db, QStringLiteral("Removing volume files"));

    QSqlQuery volume(db);
    volume.prepare(QStringLiteral("DELETE FROM volumes WHERE id = ?"));
    volume.addBindValue(volumeId);
    if (!volume.exec())
        return sqlError(db, QStringLiteral("Removing volume"));
    return {};
}

Result<qint64> IndexDatabase::beginScan(qint64 volumeId)
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread")).error();
    if (!db.transaction())
        return sqlError(db, QStringLiteral("Opening a scan")).error();

    QSqlQuery take(db);
    take.prepare(QStringLiteral("UPDATE volumes SET next_generation = next_generation + 1 WHERE id = ?"));
    take.addBindValue(volumeId);
    if (!take.exec()) {
        db.rollback();
        return sqlError(db, QStringLiteral("Taking a scan generation")).error();
    }

    QSqlQuery read(db);
    read.prepare(QStringLiteral("SELECT generation, next_generation FROM volumes WHERE id = ?"));
    read.addBindValue(volumeId);
    if (!read.exec() || !read.next()) {
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
        return sqlError(db, QStringLiteral("Clearing an abandoned scan")).error();
    }

    if (!db.commit()) {
        db.rollback();
        return sqlError(db, QStringLiteral("Opening a scan")).error();
    }
    return generation;
}

Result<void> IndexDatabase::commitScan(qint64 volumeId, qint64 generation, const QDateTime& when)
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread"));
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
        return sqlError(db, QStringLiteral("Dropping the previous scan"));
    }

    QSqlQuery swap(db);
    swap.prepare(QStringLiteral(
        "UPDATE volumes SET generation = ?, last_scan = ?, "
        "file_count = (SELECT COUNT(*) FROM files WHERE volume_id = ? AND generation = ?) WHERE id = ?"));
    swap.addBindValue(generation);
    swap.addBindValue(when.toSecsSinceEpoch());
    swap.addBindValue(volumeId);
    swap.addBindValue(generation);
    swap.addBindValue(volumeId);
    if (!swap.exec()) {
        db.rollback();
        return sqlError(db, QStringLiteral("Updating volume"));
    }

    if (!db.commit()) {
        db.rollback();
        return sqlError(db, QStringLiteral("Committing a scan"));
    }
    return {};
}

Result<void> IndexDatabase::abandonScan(qint64 volumeId, qint64 generation)
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread"));
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
        return sqlError(db, QStringLiteral("Abandoning a scan"));
    return {};
}

Result<QList<IndexVolume>> IndexDatabase::volumes() const
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread")).error();
    QSqlQuery query(db);
    if (!query.exec(
            QStringLiteral("SELECT id, root_uri, label, last_scan, file_count FROM volumes ORDER BY label")))
        return sqlError(db, QStringLiteral("Listing volumes")).error();

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
        out.append(v);
    }
    return out;
}

Result<void> IndexDatabase::insertBatch(qint64 volumeId, qint64 generation, const QList<IndexedFile>& files)
{
    if (files.isEmpty())
        return {};

    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread"));
    if (!db.transaction())
        return sqlError(db, QStringLiteral("Starting insert batch"));

    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT INTO files (volume_id, generation, name, name_folded, path, "
                                 "parent_path, extension, is_dir, size, mtime) "
                                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));

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
        if (!query.exec()) {
            db.rollback();
            return sqlError(db, QStringLiteral("Inserting indexed file"));
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
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread")).error();

    // The generations having to agree is what keeps a scan in progress out of
    // the answer: its rows are in the table and belong to no volume yet.
    QString sql = QStringLiteral(
        "SELECT f.name, v.root_uri, f.path, f.parent_path, f.is_dir, f.size, f.mtime, v.label "
        "FROM files f JOIN volumes v ON v.id = f.volume_id AND v.generation = f.generation WHERE 1=1");
    QVariantList bindings;

    for (const SearchPredicate& predicate : planSearch(query, SearchSource::Index).pushedDown()) {
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
        case SearchPredicate::Field::Modified:
        case SearchPredicate::Field::Created:
        case SearchPredicate::Field::Accessed:
        case SearchPredicate::Field::Hidden:
        case SearchPredicate::Field::TypeClass:
        case SearchPredicate::Field::Path:
        case SearchPredicate::Field::Under:
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
        return sqlError(db, QStringLiteral("Searching index")).error();

    QList<IndexSearchHit> hits;
    while (statement.next()) {
        IndexSearchHit hit;
        hit.name = statement.value(0).toString();
        const VfsUri root = VfsUri::fromString(statement.value(1).toString());
        hit.uri = VfsUri(root.scheme(), root.authority(), statement.value(2).toString()).toString();
        hit.parentPath = statement.value(3).toString();
        hit.isDir = statement.value(4).toInt() != 0;
        hit.size = statement.value(5).toLongLong();
        hit.modifiedEpoch = statement.value(6).toLongLong();
        hit.volumeLabel = statement.value(7).toString();
        hits.append(hit);
    }
    return hits;
}

Result<qint64> IndexDatabase::fileCount(qint64 volumeId) const
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread")).error();
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
        return sqlError(db, QStringLiteral("Counting files")).error();
    if (!query.next())
        return qint64 { 0 };
    return query.value(0).toLongLong();
}

} // namespace mole
