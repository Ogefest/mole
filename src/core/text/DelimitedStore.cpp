#include "core/text/DelimitedStore.h"

#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QVariant>

namespace mole {
namespace {

    /// Rows per transaction during an import. Large enough that the per-commit
    /// cost disappears, small enough that a cancelled import does not sit in one
    /// enormous uncommitted write.
    constexpr int kBatchRows = 2000;

    /// How long a connection waits for the file rather than giving up, in
    /// milliseconds. The index's own figure -- see IndexDatabase::connectionFor().
    constexpr int kBusyTimeoutMs = 5000;

    QString connectionNameFor(const QString& path)
    {
        return QStringLiteral("mole-table-%1-%2")
            .arg(QString::number(reinterpret_cast<quintptr>(QThread::currentThread()), 16),
                QString::number(qHash(path), 16));
    }

} // namespace

DelimitedStore::DelimitedStore(QString path, std::shared_ptr<QTemporaryDir> scratch)
    : m_path(std::move(path))
    , m_scratch(std::move(scratch))
{
    if (m_path.isEmpty())
        m_path = QStringLiteral(":memory:");
}

DelimitedStore::~DelimitedStore()
{
    close();
}

QString DelimitedStore::columnName(int index)
{
    // Positional names rather than the file's own headers: a CSV column can be
    // called "select", "1" or the empty string, and quoting round every one of
    // those in every query is a bug waiting to happen. The real names live in
    // m_headers, where they are only ever displayed.
    return QStringLiteral("c%1").arg(index);
}

QSqlDatabase DelimitedStore::connectionForCurrentThread() const
{
    const QString name = connectionNameFor(m_path);
    if (QSqlDatabase::contains(name)) {
        QSqlDatabase existing = QSqlDatabase::database(name);
        if (existing.isOpen())
            return existing;
    }

    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    database.setDatabaseName(m_path);
    if (!database.open())
        return database;

    // Here rather than in open(), and this is the whole of MOLE-289. A
    // connection is keyed by the thread that asked for it, and open() is called
    // by the controller -- on the drawing thread. Set there, every one of these
    // landed on the reader's connection and the importer got a brand-new one
    // with SQLite's defaults: a rollback journal per transaction, synchronous
    // writes, and no busy timeout at all. Set here, a connection cannot exist
    // without them.
    QSqlQuery pragma(database);
    // WAL is what lets the grid be attached before the import starts: a reader
    // sees everything committed so far while the writer is still going. With no
    // journal there is nothing to roll back from, so a write transaction holds
    // the file exclusively -- and the importer commits every two thousand rows
    // and opens the next transaction at once, so it holds it almost throughout.
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    // A scratch database rebuilt from a file that still exists: a machine that
    // loses power mid-import has nothing to recover, so the fsync per commit
    // buys nothing. NORMAL rather than OFF because WAL needs a checkpoint it can
    // trust, and NORMAL is already the setting at which durability is the only
    // thing given up.
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA temp_store = MEMORY"));
    // The index sets the same figure on every connection it opens, for the same
    // reason: WAL removes almost all of the contention and a checkpoint is the
    // moment it does not, so a reader that met one must wait rather than fail.
    pragma.exec(QStringLiteral("PRAGMA busy_timeout = %1").arg(kBusyTimeoutMs));

    {
        const QMutexLocker held(&m_connectionGuard);
        if (!m_connections.contains(name))
            m_connections.append(name);
    }
    return database;
}

QString DelimitedStore::pragmaValue(const QString& name) const
{
    if (!m_open)
        return {};
    QSqlQuery query(connectionForCurrentThread());
    if (!query.exec(QStringLiteral("PRAGMA %1").arg(name)) || !query.next())
        return {};
    return query.value(0).toString();
}

bool DelimitedStore::open(QString* errorOut)
{
    QSqlDatabase database = connectionForCurrentThread();
    if (!database.isOpen()) {
        if (errorOut)
            *errorOut = database.lastError().text();
        return false;
    }

    // The settings this database needs are on the connection above, and on
    // every other one the store hands out -- see connectionForCurrentThread().
    m_open = true;
    return true;
}

void DelimitedStore::close()
{
    if (!m_open)
        return;
    m_open = false;

    QStringList names;
    {
        const QMutexLocker held(&m_connectionGuard);
        names.swap(m_connections);
    }

    // Every connection the store handed out, and not the calling thread's alone.
    // The importer writes through one of its own on a pool thread, so closing
    // only the caller's left that one behind for the life of the process --
    // whichever way the store ended, and however many files were opened.
    for (const QString& name : names) {
        if (!QSqlDatabase::contains(name))
            continue;
        // Removed without being fetched first. Taking a copy is what the old
        // close() did, to avoid removeDatabase's "still in use" warning, and it
        // cannot be done here: asking for a connection that belongs to another
        // thread is itself a warning, and one of these two always does. Removing
        // it deletes the driver, which closes the file -- and nothing holds a
        // copy at this point, because a store is only closed as it is destroyed.
        QSqlDatabase::removeDatabase(name);
    }
}

QStringList DelimitedStore::shape() const
{
    // Written once by the importer on a pool thread and read by the interface
    // on its own, because the grid is attached before the import starts and the
    // columns are only settled from the head of the file. One write and many
    // reads is still a race, and an unguarded QStringList copied while it is
    // being assigned is not a wrong answer but a crash.
    const QMutexLocker held(&m_shapeGuard);
    return m_headers;
}

bool DelimitedStore::beginImport(const QStringList& headers, QString* errorOut)
{
    if (!m_open && !open(errorOut))
        return false;

    m_totalRows.store(-1, std::memory_order_relaxed);

    QSqlDatabase database = connectionForCurrentThread();
    QSqlQuery query(database);

    if (!query.exec(QStringLiteral("DROP TABLE IF EXISTS rows_"))) {
        if (errorOut)
            *errorOut = query.lastError().text();
        return false;
    }

    QStringList columns;
    columns.reserve(headers.size());
    for (int i = 0; i < headers.size(); ++i)
        columns.append(QStringLiteral("%1 TEXT").arg(columnName(i)));

    const QString create = QStringLiteral("CREATE TABLE rows_ (id INTEGER PRIMARY KEY, %1)")
                               .arg(columns.join(QStringLiteral(", ")));
    if (!query.exec(create)) {
        if (errorOut)
            *errorOut = query.lastError().text();
        return false;
    }

    // The shape is published last, and only once there is a table under it. A
    // non-empty shape is what every read path takes as its cue to query, so
    // publishing it first left a window -- the drop and the create -- in which
    // every read failed on a table that did not exist. Under a reader on the
    // file that window is not brief: instrumenting it during an import showed
    // eighty reads inside it.
    {
        const QMutexLocker held(&m_shapeGuard);
        m_headers = headers;
    }

    // Checked, like every other write here. A transaction that never opened does
    // not lose rows -- each INSERT commits itself -- but it turns an import into
    // one fsync per row, and finding that out from the timing of a large file is
    // worse than being told.
    if (!database.transaction()) {
        if (errorOut)
            *errorOut = describe(database.lastError());
        return false;
    }
    return true;
}

bool DelimitedStore::addRows(const QList<QStringList>& rows, QString* errorOut)
{
    const QStringList headers = shape();
    if (!m_open || headers.isEmpty())
        return false;

    QSqlDatabase database = connectionForCurrentThread();

    QStringList placeholders;
    QStringList columns;
    for (int i = 0; i < headers.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
        columns.append(columnName(i));
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO rows_ (%1) VALUES (%2)")
                      .arg(columns.join(QStringLiteral(", ")), placeholders.join(QStringLiteral(", "))));

    int sinceCommit = 0;
    for (const QStringList& row : rows) {
        for (int i = 0; i < headers.size(); ++i) {
            // A ragged row is padded rather than rejected: real exports are
            // ragged, and refusing them would leave the file unviewable.
            query.addBindValue(i < row.size() ? row.at(i) : QString());
        }
        if (!query.exec()) {
            if (errorOut)
                *errorOut = describe(query.lastError());
            return false;
        }

        if (++sinceCommit >= kBatchRows) {
            if (!commitBatch(database, errorOut))
                return false;
            sinceCommit = 0;
        }
    }

    m_totalRows.store(-1, std::memory_order_relaxed);
    return true;
}

bool DelimitedStore::commitBatch(QSqlDatabase& database, QString* errorOut)
{
    // Both halves, because either one failing is the same fault. A commit that
    // failed leaves that batch uncommitted; a transaction() over the top of it
    // then succeeded, addRows() returned true, and the task counted the batch and
    // finished as Succeeded -- so the grid reported a number of records the table
    // did not hold and nothing anywhere said a write had been lost. This is the
    // half of the index's pattern the store had not copied: IndexDatabase checks
    // every transaction() and every commit() at all five of its write sites. See
    // MOLE-291.
    if (!database.commit()) {
        if (errorOut)
            *errorOut = describe(database.lastError());
        return false;
    }
    if (!database.transaction()) {
        if (errorOut)
            *errorOut = describe(database.lastError());
        return false;
    }
    return true;
}

bool DelimitedStore::endImport(QString* errorOut)
{
    if (!m_open)
        return false;

    QSqlDatabase database = connectionForCurrentThread();
    if (!database.commit()) {
        // describe(), like every other write here: a locked database said as that
        // rather than in the driver's words, which is what describe() is for and
        // what addRows() has always done.
        if (errorOut)
            *errorOut = describe(database.lastError());
        return false;
    }
    m_totalRows.store(-1, std::memory_order_relaxed);
    return true;
}

QString DelimitedStore::whereClause(const QString& filter, int columns)
{
    if (filter.isEmpty())
        return {};

    // Substring, any column, case-insensitive. A filter that only searched one
    // column would need a column picker before it was useful at all.
    QStringList conditions;
    for (int i = 0; i < columns; ++i)
        conditions.append(QStringLiteral("%1 LIKE :pattern ESCAPE '\\'").arg(columnName(i)));
    return QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" OR "));
}

void DelimitedStore::bindFilter(QSqlQuery& query, const QString& filter)
{
    if (filter.isEmpty())
        return;
    QString escaped = filter;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    query.bindValue(QStringLiteral(":pattern"), QStringLiteral("%%%1%%").arg(escaped));
}

QString DelimitedStore::describe(const QSqlError& error)
{
    // SQLITE_BUSY and SQLITE_LOCKED. Told apart from an I/O error because they
    // are a different thing to report and a different thing to do about: a
    // locked database is another connection holding the file, and an import
    // that reported one as a fault of the disk sent whoever read it looking in
    // the wrong place entirely.
    const QString code = error.nativeErrorCode();
    if (code == QLatin1String("5") || code == QLatin1String("6")) {
        return QStringLiteral("The scratch database was still locked by another connection after %1 seconds")
            .arg(kBusyTimeoutMs / 1000);
    }
    return error.text();
}

qint64 DelimitedStore::totalRows() const
{
    const qint64 cached = m_totalRows.load(std::memory_order_relaxed);
    if (cached >= 0)
        return cached;
    // A count that failed comes back as -1, so it is not remembered as an
    // answer: the next ask takes it again rather than showing a nought for the
    // rest of the import.
    const qint64 taken = matchingRows({});
    m_totalRows.store(taken, std::memory_order_relaxed);
    return taken;
}

qint64 DelimitedStore::matchingRows(const QString& filter) const
{
    const QStringList headers = shape();
    // Nothing to count rather than a count that failed: before the shape is
    // settled there is no table, and an empty file has no rows in it.
    if (!m_open || headers.isEmpty())
        return 0;

    QSqlQuery query(connectionForCurrentThread());
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM rows_%1").arg(whereClause(filter, headers.size())));
    bindFilter(query, filter);
    // A count that could not be taken is not a count of nought. Reported as the
    // -1 the interface already understands as "not known yet" -- the grid leaves
    // the figure blank and asks again -- rather than as an empty table, which is
    // what made a file still importing read as one with nothing in it.
    if (!query.exec() || !query.next())
        return -1;
    return query.value(0).toLongLong();
}

QList<QStringList> DelimitedStore::rows(qint64 offset, int limit, const QString& filter, bool* readable) const
{
    if (readable)
        *readable = true;

    QList<QStringList> out;
    const QStringList headers = shape();
    if (!m_open || headers.isEmpty() || limit <= 0)
        return out;

    QStringList columns;
    for (int i = 0; i < headers.size(); ++i)
        columns.append(columnName(i));

    QSqlQuery query(connectionForCurrentThread());
    query.prepare(QStringLiteral("SELECT %1 FROM rows_%2 ORDER BY id LIMIT :limit OFFSET :offset")
                      .arg(columns.join(QStringLiteral(", ")), whereClause(filter, headers.size())));
    bindFilter(query, filter);
    query.bindValue(QStringLiteral(":limit"), limit);
    query.bindValue(QStringLiteral(":offset"), offset);

    if (!query.exec()) {
        // The same answer as the count above, in the shape ADR-0030 settled: the
        // window and whether it could be read at all. A caller that cached this
        // empty list would leave a blank stripe in the grid until something else
        // happened to clear it.
        if (readable)
            *readable = false;
        return out;
    }

    out.reserve(limit);
    while (query.next()) {
        QStringList row;
        row.reserve(headers.size());
        for (int i = 0; i < headers.size(); ++i)
            row.append(query.value(i).toString());
        out.append(row);
    }
    return out;
}

QList<int> DelimitedStore::columnWidths(int sampleRows) const
{
    QList<int> widths;
    const QStringList headers = shape();
    if (!m_open || headers.isEmpty())
        return widths;

    // The header is part of the measurement: a one-character column under a
    // twenty-character title still needs to fit the title.
    for (const QString& header : headers)
        widths.append(static_cast<int>(header.size()));

    const QList<QStringList> sample = rows(0, sampleRows);
    for (const QStringList& row : sample) {
        for (int i = 0; i < widths.size() && i < row.size(); ++i)
            widths[i] = std::max(widths.at(i), static_cast<int>(row.at(i).size()));
    }
    return widths;
}

} // namespace mole
