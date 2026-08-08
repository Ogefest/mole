#pragma once

#include "core/data/ITableSource.h"

#include <QSqlDatabase>
#include <QStringList>

namespace mole {

/// One table inside a SQLite file, read in place.
///
/// No import: the file already is a queryable table, so paging and filtering are
/// queries against it. That also means a database of any size opens instantly,
/// which is the whole reason the grid was made source-agnostic.
///
/// Opened read-only and with immutable disabled, so a database another process
/// has open is still readable and can never be written to by this one.
/// Previewing a file is not a licence to modify it.
class SqliteTable : public ITableSource
{
public:
    explicit SqliteTable(QString path);
    ~SqliteTable() override;

    SqliteTable(const SqliteTable&) = delete;
    SqliteTable& operator=(const SqliteTable&) = delete;

    bool open(QString* errorOut = nullptr);
    void close();
    bool isOpen() const { return m_open; }

    /// Tables and views, in the order SQLite lists them. Internal `sqlite_*`
    /// tables are left out: they are the file's own bookkeeping, not its data.
    QStringList tableNames() const { return m_tables; }
    /// Rows in each table, keyed by name, for the picker to show alongside it.
    qint64 rowCountOf(const QString& table) const;

    /// Which table the grid is reading. Empty until one is chosen.
    QString currentTable() const { return m_table; }
    bool setCurrentTable(const QString& table);

    // ---- ITableSource ---------------------------------------------------

    QStringList headers() const override { return m_headers; }
    qint64 totalRows() const override;
    qint64 matchingRows(const QString& filter) const override;
    QList<QStringList> rows(qint64 offset, int limit, const QString& filter = {}) const override;
    QList<int> columnWidths(int sampleRows = 200) const override;

private:
    QSqlDatabase connection() const;
    /// Quotes an identifier for use in a query. A table called "select" or
    /// containing a quote is legal in SQLite and must not become an injection.
    static QString quoted(const QString& identifier);
    QString whereClause(const QString& filter) const;
    void bindFilter(class QSqlQuery& query, const QString& filter) const;

    QString m_path;
    QString m_connectionName;
    QStringList m_tables;
    QString m_table;
    QStringList m_headers;
    bool m_open = false;
    mutable qint64 m_totalRows = -1;
};

} // namespace mole
