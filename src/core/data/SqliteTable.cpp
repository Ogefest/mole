#include "core/data/SqliteTable.h"

#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QThread>
#include <QUrl>
#include <QVariant>

#include <algorithm>

namespace mole {
namespace {

    QString connectionNameFor(const QString& path)
    {
        return QStringLiteral("mole-sqlite-%1-%2")
            .arg(QString::number(reinterpret_cast<quintptr>(QThread::currentThread()), 16),
                QString::number(qHash(path), 16));
    }

} // namespace

SqliteTable::SqliteTable(QString path)
    : m_path(std::move(path))
    , m_connectionName(connectionNameFor(m_path))
{
}

SqliteTable::~SqliteTable()
{
    close();
}

QString SqliteTable::quoted(const QString& identifier)
{
    QString escaped = identifier;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QSqlDatabase SqliteTable::connection() const
{
    return QSqlDatabase::database(m_connectionName, false);
}

bool SqliteTable::open(QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    if (!QFileInfo::exists(m_path))
        return fail(QStringLiteral("No such file"));

    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    // Read-only through the URI form, which is the only way SQLite will refuse
    // writes outright. `immutable` is deliberately *not* set: it would promise
    // the file cannot change, and another process may well be writing to it.
    database.setDatabaseName(
        QStringLiteral("file:%1?mode=ro").arg(QString::fromUtf8(QUrl::toPercentEncoding(m_path, "/"))));
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI"));

    if (!database.open()) {
        const QString message = database.lastError().text();
        QSqlDatabase::removeDatabase(m_connectionName);
        return fail(message.isEmpty() ? QStringLiteral("Not a SQLite database") : message);
    }

    m_open = true;

    QSqlQuery query(database);
    // Views as well as tables: from the outside both are something to look at,
    // and excluding views would hide half of some databases.
    if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type IN ('table','view') "
                                   "AND name NOT LIKE 'sqlite_%' ORDER BY name"))) {
        const QString message = query.lastError().text();
        close();
        return fail(message);
    }
    while (query.next())
        m_tables.append(query.value(0).toString());

    if (m_tables.isEmpty())
        return fail(QStringLiteral("This database has no tables"));

    return setCurrentTable(m_tables.first());
}

void SqliteTable::close()
{
    if (!m_open)
        return;
    m_open = false;
    m_rowCounts.clear();
    {
        QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
        if (database.isOpen())
            database.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool SqliteTable::setCurrentTable(const QString& table)
{
    if (!m_open || !m_tables.contains(table))
        return false;

    m_table = table;
    m_headers.clear();

    // Asked of the table rather than parsed out of its schema: a query returns
    // the columns a SELECT will actually produce, which is what the grid needs.
    QSqlQuery query(connection());
    if (!query.exec(QStringLiteral("SELECT * FROM %1 LIMIT 0").arg(quoted(table))))
        return false;

    const QSqlRecord record = query.record();
    for (int i = 0; i < record.count(); ++i)
        m_headers.append(record.fieldName(i));
    return !m_headers.isEmpty();
}

qint64 SqliteTable::rowCountOf(const QString& table) const
{
    if (!m_open || !m_tables.contains(table))
        return 0;

    // Remembered for the life of the open file. The picker asks for every name
    // and the summary strip asks again for the current one, and the file cannot
    // change under a connection that was opened read-only -- so the second and
    // third answers are free.
    const auto known = m_rowCounts.constFind(table);
    if (known != m_rowCounts.constEnd())
        return *known;

    QSqlQuery query(connection());
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(quoted(table))) || !query.next())
        return 0;

    const qint64 rows = query.value(0).toLongLong();
    m_rowCounts.insert(table, rows);
    return rows;
}

qint64 SqliteTable::knownRowCountOf(const QString& table) const
{
    return m_rowCounts.value(table, kRowsNotCounted);
}

void SqliteTable::setRowCount(const QString& table, qint64 rows)
{
    if (m_tables.contains(table) && rows >= 0)
        m_rowCounts.insert(table, rows);
}

QString SqliteTable::whereClause(const QString& filter) const
{
    if (filter.isEmpty() || m_headers.isEmpty())
        return {};

    // CAST, because a column can hold a number and LIKE on a number does not
    // match the digits the user is looking at.
    QStringList conditions;
    for (const QString& header : m_headers) {
        conditions.append(QStringLiteral("CAST(%1 AS TEXT) LIKE :pattern ESCAPE '\\'").arg(quoted(header)));
    }
    return QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" OR "));
}

void SqliteTable::bindFilter(QSqlQuery& query, const QString& filter) const
{
    if (filter.isEmpty())
        return;
    // A user typing "%" means a per cent sign. Leaving LIKE's wildcards live
    // would make the filter behave at random.
    QString escaped = filter;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    query.bindValue(QStringLiteral(":pattern"), QStringLiteral("%%%1%%").arg(escaped));
}

qint64 SqliteTable::totalRows() const
{
    // The remembered count, never a fresh one. This is read from the interface
    // thread -- by the model when it is pointed at a table, and by the summary
    // strip -- and that thread must not wait for a walk of the table.
    return knownRowCountOf(m_table);
}

qint64 SqliteTable::matchingRows(const QString& filter) const
{
    if (!m_open || m_table.isEmpty())
        return 0;
    if (filter.isEmpty())
        return totalRows();

    QSqlQuery query(connection());
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM %1%2").arg(quoted(m_table), whereClause(filter)));
    bindFilter(query, filter);
    if (!query.exec() || !query.next())
        return 0;
    return query.value(0).toLongLong();
}

QList<QStringList> SqliteTable::rows(qint64 offset, int limit, const QString& filter) const
{
    QList<QStringList> out;
    if (!m_open || m_table.isEmpty() || limit <= 0)
        return out;

    // No ORDER BY: a table without a primary key has no inherent order, and
    // sorting by an arbitrary column would be slower and no more truthful.
    QSqlQuery query(connection());
    query.prepare(QStringLiteral("SELECT * FROM %1%2 LIMIT :limit OFFSET :offset")
                      .arg(quoted(m_table), whereClause(filter)));
    bindFilter(query, filter);
    query.bindValue(QStringLiteral(":limit"), limit);
    query.bindValue(QStringLiteral(":offset"), offset);

    if (!query.exec())
        return out;

    out.reserve(limit);
    while (query.next()) {
        QStringList row;
        row.reserve(m_headers.size());
        for (int i = 0; i < m_headers.size(); ++i) {
            const QVariant value = query.value(i);
            // A NULL is not an empty string, and showing them the same way
            // hides the difference the reader is often looking for.
            row.append(value.isNull() ? QStringLiteral("NULL") : value.toString());
        }
        out.append(row);
    }
    return out;
}

QList<int> SqliteTable::columnWidths(int sampleRows) const
{
    QList<int> widths;
    if (!m_open || m_headers.isEmpty())
        return widths;

    for (const QString& header : m_headers)
        widths.append(static_cast<int>(header.size()));

    const QList<QStringList> sample = rows(0, sampleRows);
    for (const QStringList& row : sample) {
        for (int i = 0; i < widths.size() && i < row.size(); ++i)
            widths[i] = std::max(widths.at(i), static_cast<int>(row.at(i).size()));
    }
    return widths;
}

} // namespace mole
