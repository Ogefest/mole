#pragma once

#include "core/data/ITableSource.h"

#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <memory>

namespace mole {

/// A delimited file imported into SQLite so it can be paged, filtered and
/// sorted without ever holding it in memory.
///
/// The alternative -- parse the first N rows and stop -- means the viewer lies
/// about what is in the file, and a filter can only ever search the part that
/// happened to be loaded. Importing costs one pass; after that every question
/// is a query, and the answer covers the whole file however large it is.
///
/// One connection per thread, like the index: the importer writes from a pool
/// thread while the interface reads from its own.
class DelimitedStore : public ITableSource
{
public:
    /// `path` is a file the caller owns and will delete. An empty path makes a
    /// private in-memory database, which only the creating thread can use.
    explicit DelimitedStore(QString path);
    ~DelimitedStore();

    DelimitedStore(const DelimitedStore&) = delete;
    DelimitedStore& operator=(const DelimitedStore&) = delete;

    bool open(QString* errorOut = nullptr);
    void close();
    bool isOpen() const { return m_open; }

    // ---- writing --------------------------------------------------------

    /// Starts a fresh import. Any previous contents are dropped.
    bool beginImport(const QStringList& headers, QString* errorOut = nullptr);
    /// Appends rows. Short rows are padded, long ones truncated, so a ragged
    /// file imports rather than failing halfway through.
    bool addRows(const QList<QStringList>& rows, QString* errorOut = nullptr);
    bool endImport(QString* errorOut = nullptr);

    // ---- reading --------------------------------------------------------

    QStringList headers() const override { return m_headers; }
    /// Every row in the file.
    qint64 totalRows() const override;
    qint64 matchingRows(const QString& filter) const override;

    /// A window of rows in file order. This is the only read path the model
    /// uses, so scrolling costs one query per screen rather than one file.
    QList<QStringList> rows(qint64 offset, int limit, const QString& filter = {}) const override;

    /// The longest value seen in each column over the first `sampleRows` rows,
    /// in characters. Used to size columns to their contents instead of a
    /// default that wastes half the window.
    QList<int> columnWidths(int sampleRows = 200) const override;

private:
    QSqlDatabase connectionForCurrentThread() const;
    static QString columnName(int index);
    QString whereClause(const QString& filter) const;

    QString m_path;
    QStringList m_headers;
    bool m_open = false;
    /// Cached because every scroll step asks for it and it never changes
    /// after an import.
    mutable qint64 m_totalRows = -1;
};

} // namespace mole
