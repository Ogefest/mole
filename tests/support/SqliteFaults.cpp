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

int primaryCode(const QString& nativeErrorCode)
{
    bool ok = false;
    const int code = nativeErrorCode.toInt(&ok);
    return ok ? (code & 0xff) : -1;
}

} // namespace mole::test::sqlite
