#pragma once

#include "core/data/ITableSource.h"

#include <QHash>
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

    /// The file this reads. Needed by anything that wants a second connection
    /// to it -- the counting task opens one of its own on a pool thread.
    const QString& path() const { return m_path; }

    bool open(QString* errorOut = nullptr);
    void close();
    bool isOpen() const { return m_open; }

    /// Tables and views, in the order SQLite lists them. Internal `sqlite_*`
    /// tables are left out: they are the file's own bookkeeping, not its data.
    QStringList tableNames() const { return m_tables; }

    /// Rows in `table`, counted now if nobody has counted it yet and remembered
    /// for the life of the open file. `SELECT COUNT(*)` is a walk of the table,
    /// and nothing bounds how long that takes, so **call this from a worker
    /// thread** -- CountTableRowsTask is what does.
    qint64 rowCountOf(const QString& table) const;
    /// The remembered count, or kRowsNotCounted when nobody has worked it out
    /// yet. Answers without touching the file, so the interface thread may ask.
    qint64 knownRowCountOf(const QString& table) const;
    /// Remembers a count worked out on another connection, which is how the
    /// counting task hands its answers back.
    void setRowCount(const QString& table, qint64 rows);

    /// What a count reads as before anybody has taken it. A blank in the
    /// interface: honest, where a guess from `max(rowid)` would be wrong for a
    /// table that has had rows deleted and meaningless for a view.
    static constexpr qint64 kRowsNotCounted = -1;

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
    /// Counts already taken, by table name. Emptied when the file is closed,
    /// because a count belongs to the file that was open when it was taken.
    mutable QHash<QString, qint64> m_rowCounts;
};

} // namespace mole
