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

    constexpr int kSchemaVersion = 1;

    /// SQLite's LIKE and NOCASE only fold ASCII, so "Łódź" would never match
    /// "łódź". We store a Qt-lowercased copy of the name and match against
    /// that instead -- Qt's toLower() is Unicode-aware.
    QString foldForSearch(const QString& text)
    {
        return text.toLower();
    }

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

        if (!db.transaction())
            return sqlError(db, QStringLiteral("Starting migration"));
        for (const QString& statement : statements) {
            QSqlQuery migration(db);
            if (!migration.exec(statement)) {
                db.rollback();
                return sqlError(db, QStringLiteral("Applying migration 1"));
            }
        }
        if (!db.commit())
            return sqlError(db, QStringLiteral("Committing migration"));
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

Result<void> IndexDatabase::clearVolume(qint64 volumeId)
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread"));
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM files WHERE volume_id = ?"));
    query.addBindValue(volumeId);
    if (!query.exec())
        return sqlError(db, QStringLiteral("Clearing volume"));
    return {};
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

Result<void> IndexDatabase::markVolumeScanned(qint64 volumeId, const QDateTime& when)
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return Result<void>::failure(VfsError::IoError, QStringLiteral("Index is not open"));

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread"));
    QSqlQuery query(db);
    query.prepare(
        QStringLiteral("UPDATE volumes SET last_scan = ?, "
                       "file_count = (SELECT COUNT(*) FROM files WHERE volume_id = ?) WHERE id = ?"));
    query.addBindValue(when.toSecsSinceEpoch());
    query.addBindValue(volumeId);
    query.addBindValue(volumeId);
    if (!query.exec())
        return sqlError(db, QStringLiteral("Updating volume"));
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

Result<void> IndexDatabase::insertBatch(qint64 volumeId, const QList<IndexedFile>& files)
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
    query.prepare(QStringLiteral(
        "INSERT INTO files (volume_id, name, name_folded, path, parent_path, extension, is_dir, size, mtime) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));

    for (const IndexedFile& file : files) {
        query.addBindValue(volumeId);
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

Result<QList<IndexSearchHit>> IndexDatabase::search(const IndexSearchQuery& query) const
{
    QMutexLocker lock(&m_mutex);
    if (!m_open)
        return VfsError::make(VfsError::IoError, QStringLiteral("Index is not open"));
    if (!query.includeDirs && !query.includeFiles)
        return QList<IndexSearchHit> {};

    QSqlDatabase db = connectionForCurrentThread();
    if (!db.isOpen())
        return sqlError(db, QStringLiteral("No index connection for this thread")).error();

    QString sql = QStringLiteral(
        "SELECT f.name, v.root_uri, f.path, f.parent_path, f.is_dir, f.size, f.mtime, v.label "
        "FROM files f JOIN volumes v ON v.id = f.volume_id WHERE 1=1");
    QVariantList bindings;

    if (!query.text.isEmpty()) {
        // instr() instead of LIKE: no wildcard escaping to get wrong, and it
        // lets the case-insensitive path use our Unicode-folded column.
        if (query.caseSensitive) {
            sql += QStringLiteral(" AND instr(f.name, ?) > 0");
            bindings.append(query.text);
        } else {
            sql += QStringLiteral(" AND instr(f.name_folded, ?) > 0");
            bindings.append(foldForSearch(query.text));
        }
    }
    if (!query.extension.isEmpty()) {
        sql += QStringLiteral(" AND f.extension = ?");
        bindings.append(query.extension.toLower());
    }
    if (query.volumeId >= 0) {
        sql += QStringLiteral(" AND f.volume_id = ?");
        bindings.append(query.volumeId);
    }
    if (!query.includeDirs)
        sql += QStringLiteral(" AND f.is_dir = 0");
    if (!query.includeFiles)
        sql += QStringLiteral(" AND f.is_dir = 1");
    if (query.minSize >= 0) {
        sql += QStringLiteral(" AND f.size >= ?");
        bindings.append(query.minSize);
    }
    if (query.maxSize >= 0) {
        sql += QStringLiteral(" AND f.size <= ?");
        bindings.append(query.maxSize);
    }

    sql += QStringLiteral(" ORDER BY f.is_dir DESC, f.name COLLATE NOCASE LIMIT ?");
    bindings.append(query.limit > 0 ? query.limit : 5000);

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
    QSqlQuery query(db);
    if (volumeId >= 0) {
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM files WHERE volume_id = ?"));
        query.addBindValue(volumeId);
    } else {
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM files"));
    }
    if (!query.exec())
        return sqlError(db, QStringLiteral("Counting files")).error();
    if (!query.next())
        return qint64 { 0 };
    return query.value(0).toLongLong();
}

} // namespace mole
