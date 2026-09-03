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

SqliteTable::SqliteTable(QString path)
    : m_path(std::move(path))
    // Read-only through SQLite's URI form, which is the only way it will refuse
    // writes outright. No pragmas: this is somebody else's database, opened to
    // be looked at, and another process may be writing to it.
    , m_connections(
          std::make_unique<sqlite::Connection>(m_path, sqlite::Connection::Settings { .readOnly = true }))
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
    return m_connections->forCurrentThread();
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

    QSqlDatabase database = m_connections->forCurrentThread();
    if (!database.isOpen()) {
        const QString message = m_connections->lastError();
        return fail(message.isEmpty() ? QStringLiteral("Not a SQLite database") : message);
    }

    m_open = true;

    // The query and the local copy of the connection are both scoped, because
    // close() below removes the connection and Qt says "still in use, all
    // queries will cease to work" if anything is holding one when it does.
    QString wrong;
    QStringList found;
    {
        QSqlQuery query(database);
        // Views as well as tables: from the outside both are something to look
        // at, and excluding views would hide half of some databases.
        if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type IN ('table','view') "
                                       "AND name NOT LIKE 'sqlite_%' ORDER BY name"))) {
            wrong = query.lastError().text();
        } else {
            while (query.next())
                found.append(query.value(0).toString());
        }
    }
    database = QSqlDatabase();

    // Closed on the way out, not merely reported. open() used to return false
    // here with m_open left true and the connection still registered, so a
    // second attempt on the same object met the duplicate-name path and Qt's
    // warning rather than the answer it asked for.
    if (!wrong.isEmpty()) {
        close();
        return fail(wrong);
    }
    if (found.isEmpty()) {
        close();
        return fail(QStringLiteral("This database has no tables"));
    }

    m_tables = found;
    return setCurrentTable(m_tables.first());
}

void SqliteTable::close()
{
    if (!m_open)
        return;
    m_open = false;
    m_rowCounts.clear();
    m_connections->closeAll();
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
    // A count that could not be taken is not a count of nought. A view over a
    // table that has been dropped, a locked database, a corrupt page: all of
    // them answered 0, so the footer said "0 rows" and the picker listed the
    // table as empty. kRowsNotCounted is what the interface already understands
    // -- it leaves the figure blank and asks again -- and it is what
    // DelimitedStore has answered all along. ADR-0030; see MOLE-353.
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(quoted(table))) || !query.next())
        return kRowsNotCounted;

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
    // As above: not known, rather than none. The interface's own header says
    // matchingRows answers "-1 on the same terms as totalRows()", and this was
    // the one of the two implementations that did not.
    if (!query.exec() || !query.next())
        return kRowsNotCounted;
    return query.value(0).toLongLong();
}

QList<QStringList> SqliteTable::rows(qint64 offset, int limit, const QString& filter, bool* readable) const
{
    if (readable)
        *readable = true;

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

    // A window that could not be read is said so rather than answered as one
    // that held nothing -- see ITableSource::rows() and ADR-0030.
    if (!query.exec()) {
        if (readable)
            *readable = false;
        return out;
    }

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
