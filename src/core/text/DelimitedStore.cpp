#include "core/text/DelimitedStore.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>

namespace mole {
namespace {

    /// Rows per transaction during an import. Large enough that the per-commit
    /// cost disappears, small enough that a cancelled import does not sit in one
    /// enormous uncommitted write.
    constexpr int kBatchRows = 2000;

    QString connectionNameFor(const QString& path)
    {
        return QStringLiteral("mole-table-%1-%2")
            .arg(QString::number(reinterpret_cast<quintptr>(QThread::currentThread()), 16),
                QString::number(qHash(path), 16));
    }

} // namespace

DelimitedStore::DelimitedStore(QString path)
    : m_path(std::move(path))
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
    database.open();
    return database;
}

bool DelimitedStore::open(QString* errorOut)
{
    QSqlDatabase database = connectionForCurrentThread();
    if (!database.isOpen()) {
        if (errorOut)
            *errorOut = database.lastError().text();
        return false;
    }

    QSqlQuery pragma(database);
    // A scratch database rebuilt from a file that still exists: durability
    // buys nothing here, and turning it off makes the import several times
    // faster.
    pragma.exec(QStringLiteral("PRAGMA journal_mode = OFF"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = OFF"));
    pragma.exec(QStringLiteral("PRAGMA temp_store = MEMORY"));

    m_open = true;
    return true;
}

void DelimitedStore::close()
{
    if (!m_open)
        return;
    m_open = false;

    const QString name = connectionNameFor(m_path);
    if (QSqlDatabase::contains(name)) {
        // The scope matters: QSqlDatabase::removeDatabase warns loudly if any
        // copy of the connection is still alive when it is called.
        {
            QSqlDatabase database = QSqlDatabase::database(name, false);
            if (database.isOpen())
                database.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
}

bool DelimitedStore::beginImport(const QStringList& headers, QString* errorOut)
{
    if (!m_open && !open(errorOut))
        return false;

    m_headers = headers;
    m_totalRows = -1;

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

    database.transaction();
    return true;
}

bool DelimitedStore::addRows(const QList<QStringList>& rows, QString* errorOut)
{
    if (!m_open || m_headers.isEmpty())
        return false;

    QSqlDatabase database = connectionForCurrentThread();

    QStringList placeholders;
    QStringList columns;
    for (int i = 0; i < m_headers.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
        columns.append(columnName(i));
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO rows_ (%1) VALUES (%2)")
                      .arg(columns.join(QStringLiteral(", ")), placeholders.join(QStringLiteral(", "))));

    int sinceCommit = 0;
    for (const QStringList& row : rows) {
        for (int i = 0; i < m_headers.size(); ++i) {
            // A ragged row is padded rather than rejected: real exports are
            // ragged, and refusing them would leave the file unviewable.
            query.addBindValue(i < row.size() ? row.at(i) : QString());
        }
        if (!query.exec()) {
            if (errorOut)
                *errorOut = query.lastError().text();
            return false;
        }

        if (++sinceCommit >= kBatchRows) {
            database.commit();
            database.transaction();
            sinceCommit = 0;
        }
    }

    m_totalRows = -1;
    return true;
}

bool DelimitedStore::endImport(QString* errorOut)
{
    if (!m_open)
        return false;

    QSqlDatabase database = connectionForCurrentThread();
    if (!database.commit()) {
        if (errorOut)
            *errorOut = database.lastError().text();
        return false;
    }
    m_totalRows = -1;
    return true;
}

QString DelimitedStore::whereClause(const QString& filter) const
{
    if (filter.isEmpty())
        return {};

    // Substring, any column, case-insensitive. A filter that only searched one
    // column would need a column picker before it was useful at all.
    QStringList conditions;
    for (int i = 0; i < m_headers.size(); ++i)
        conditions.append(QStringLiteral("%1 LIKE :pattern ESCAPE '\\'").arg(columnName(i)));
    return QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" OR "));
}

qint64 DelimitedStore::totalRows() const
{
    if (m_totalRows >= 0)
        return m_totalRows;
    m_totalRows = matchingRows({});
    return m_totalRows;
}

qint64 DelimitedStore::matchingRows(const QString& filter) const
{
    if (!m_open || m_headers.isEmpty())
        return 0;

    QSqlQuery query(connectionForCurrentThread());
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM rows_%1").arg(whereClause(filter)));
    if (!filter.isEmpty()) {
        QString escaped = filter;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        query.bindValue(QStringLiteral(":pattern"), QStringLiteral("%%%1%%").arg(escaped));
    }
    if (!query.exec() || !query.next())
        return 0;
    return query.value(0).toLongLong();
}

QList<QStringList> DelimitedStore::rows(qint64 offset, int limit, const QString& filter) const
{
    QList<QStringList> out;
    if (!m_open || m_headers.isEmpty() || limit <= 0)
        return out;

    QStringList columns;
    for (int i = 0; i < m_headers.size(); ++i)
        columns.append(columnName(i));

    QSqlQuery query(connectionForCurrentThread());
    query.prepare(QStringLiteral("SELECT %1 FROM rows_%2 ORDER BY id LIMIT :limit OFFSET :offset")
                      .arg(columns.join(QStringLiteral(", ")), whereClause(filter)));
    if (!filter.isEmpty()) {
        QString escaped = filter;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        query.bindValue(QStringLiteral(":pattern"), QStringLiteral("%%%1%%").arg(escaped));
    }
    query.bindValue(QStringLiteral(":limit"), limit);
    query.bindValue(QStringLiteral(":offset"), offset);

    if (!query.exec())
        return out;

    out.reserve(limit);
    while (query.next()) {
        QStringList row;
        row.reserve(m_headers.size());
        for (int i = 0; i < m_headers.size(); ++i)
            row.append(query.value(i).toString());
        out.append(row);
    }
    return out;
}

QList<int> DelimitedStore::columnWidths(int sampleRows) const
{
    QList<int> widths;
    if (!m_open || m_headers.isEmpty())
        return widths;

    // The header is part of the measurement: a one-character column under a
    // twenty-character title still needs to fit the title.
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
