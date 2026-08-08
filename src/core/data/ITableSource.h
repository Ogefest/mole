#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace mole {

/// A table the grid can read: paged, filterable, of unknown size.
///
/// The grid was written against the CSV importer, which meant every other
/// tabular thing -- a SQLite table, a Parquet file -- would have needed its own
/// grid or its own import. Both are wasteful: a database *is* already a queryable
/// table, and importing it again to page through it would be absurd.
///
/// Every method is called from the thread that owns the source. Implementations
/// that talk to a file must be usable from the interface thread, which in
/// practice means answering a windowed query quickly rather than scanning.
class ITableSource
{
public:
    virtual ~ITableSource() = default;

    /// Column names. An empty entry means "no name", which the view renders as
    /// a spreadsheet letter rather than inventing one.
    virtual QStringList headers() const = 0;
    int columnCount() const { return static_cast<int>(headers().size()); }

    /// Rows in the whole table.
    virtual qint64 totalRows() const = 0;
    /// Rows matching `filter`, or every row when it is empty. Substring, any
    /// column, case-insensitive -- a filter that searched one column would need
    /// a column picker before it was useful at all.
    virtual qint64 matchingRows(const QString& filter) const = 0;

    /// A window of rows in table order. This is the only read path the model
    /// uses, so scrolling costs one query per screen rather than one per table.
    virtual QList<QStringList> rows(qint64 offset, int limit, const QString& filter = {}) const = 0;

    /// The longest value in each column over a sample, in characters, so the
    /// grid can size columns to their contents instead of to a default that
    /// wastes half the window.
    virtual QList<int> columnWidths(int sampleRows = 200) const = 0;
};

} // namespace mole
