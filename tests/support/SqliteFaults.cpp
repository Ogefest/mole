#include "support/SqliteFaults.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace mole::test::sqlite {

namespace {

    bool run(QSqlDatabase& connection, const QString& statement)
    {
        QSqlQuery query(connection);
        return query.exec(statement);
    }

} // namespace

QSqlDatabase connectionOn(const QString& databasePath)
{
    for (const QString& name : QSqlDatabase::connectionNames()) {
        // `false` so finding a connection does not open one: a closed connection
        // is not the store's working one, and opening it here would change what
        // the case is looking at.
        QSqlDatabase candidate = QSqlDatabase::database(name, false);
        if (candidate.isOpen() && candidate.databaseName() == databasePath)
            return candidate;
    }
    return {};
}

bool stopWaitingForLocks(QSqlDatabase& connection)
{
    return connection.isOpen() && run(connection, QStringLiteral("PRAGMA busy_timeout=0"));
}

bool capAt(QSqlDatabase& connection, int pages)
{
    if (!connection.isOpen())
        return false;
    QSqlQuery query(connection);
    if (!query.exec(QStringLiteral("PRAGMA max_page_count=%1").arg(pages)))
        return false;
    // The pragma answers with the cap it settled on, and it will not go below the
    // pages already in use -- so a cap that did not take is a case that would
    // otherwise assert on a write nothing was stopping.
    return query.next() && query.value(0).toInt() == pages;
}

bool capAtCurrentSize(QSqlDatabase& connection)
{
    if (!connection.isOpen())
        return false;
    QSqlQuery query(connection);
    if (!query.exec(QStringLiteral("PRAGMA page_count")) || !query.next())
        return false;
    const int used = query.value(0).toInt();
    return used > 0 && capAt(connection, used);
}

WriteLock::WriteLock(const QString& databasePath)
    : m_name(
          QStringLiteral("mole-test-write-lock-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    QSqlDatabase holder = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
    holder.setDatabaseName(databasePath);
    if (!holder.open())
        return;
    // IMMEDIATE rather than DEFERRED: a deferred transaction takes no lock until
    // it writes, so it would hold nothing at all.
    m_held = run(holder, QStringLiteral("BEGIN IMMEDIATE"));
}

WriteLock::~WriteLock()
{
    {
        QSqlDatabase holder = QSqlDatabase::database(m_name, false);
        if (holder.isOpen()) {
            if (m_held)
                run(holder, QStringLiteral("ROLLBACK"));
            holder.close();
        }
    }
    // Outside the scope above, because Qt warns -- and rightly -- about removing a
    // connection that something still holds a handle to.
    QSqlDatabase::removeDatabase(m_name);
}

StaleReadSnapshot::StaleReadSnapshot(QSqlDatabase& connection, const QString& databasePath)
    : m_connection(connection)
    , m_moverName(QStringLiteral("mole-test-snapshot-mover-%1")
                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    if (!m_connection.isOpen())
        return;

    // The read this connection will be holding when the database moves on. A
    // deferred transaction takes its snapshot at the first read rather than at
    // BEGIN, so the SELECT is the part that matters.
    //
    // BEGIN is attempted rather than required: the interesting case is a store
    // that already has a transaction open -- an import -- and SQLite refuses a
    // transaction inside a transaction. Either way what follows is a connection
    // holding a read, which is the state being arranged. Only a transaction this
    // opened is one this may close.
    m_began = run(m_connection, QStringLiteral("BEGIN"));
    {
        QSqlQuery reading(m_connection);
        if (!reading.exec(QStringLiteral("SELECT count(*) FROM sqlite_master")) || !reading.next()) {
            if (m_began)
                run(m_connection, QStringLiteral("ROLLBACK"));
            return;
        }
    }

    QSqlDatabase mover = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_moverName);
    mover.setDatabaseName(databasePath);
    if (!mover.open()) {
        run(m_connection, QStringLiteral("ROLLBACK"));
        return;
    }
    // Anything that changes the file will do; a table of its own so this cannot
    // disturb what the case is looking at.
    m_stale = run(mover, QStringLiteral("CREATE TABLE IF NOT EXISTS mole_test_snapshot_ (n INTEGER)"))
        && run(mover, QStringLiteral("INSERT INTO mole_test_snapshot_ (n) VALUES (1)"));
    if (!m_stale && m_began)
        run(m_connection, QStringLiteral("ROLLBACK"));
}

StaleReadSnapshot::~StaleReadSnapshot()
{
    {
        QSqlDatabase mover = QSqlDatabase::database(m_moverName, false);
        if (mover.isOpen())
            mover.close();
    }
    QSqlDatabase::removeDatabase(m_moverName);
}

int primaryCode(const QString& nativeErrorCode)
{
    bool ok = false;
    const int code = nativeErrorCode.toInt(&ok);
    return ok ? (code & 0xff) : -1;
}

} // namespace mole::test::sqlite
