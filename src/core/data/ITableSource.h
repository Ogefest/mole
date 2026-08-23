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
/// THREADING
/// ---------
/// A source is asked one question at a time, and `canBeReadOnATask()` says where
/// the asking may happen. The default is the thread that owns the source -- which
/// is what a SQLite table and a delimited store need, their connections being
/// bound to the thread that opened them -- and a source that answers true instead
/// gets its file-touching questions asked on a pool thread, one at a time, with
/// the model filling the rows in when they land. See ADR-0079.
class ITableSource
{
public:
    virtual ~ITableSource() = default;

    /// Column names. An empty entry means "no name", which the view renders as
    /// a spreadsheet letter rather than inventing one.
    virtual QStringList headers() const = 0;
    int columnCount() const { return static_cast<int>(headers().size()); }

    /// Whether rows(), a filtered matchingRows() and columnWidths() may be called
    /// from a pool thread rather than from the thread that owns this source.
    ///
    /// False by default, which is the safe answer: a source whose reads are a
    /// query with a bounded offset -- see ADR-0045 -- costs little enough to
    /// answer inline, and both of the ones that do hold a database connection
    /// belonging to the thread that opened it.
    ///
    /// A source that answers true is promising two things. That those three may be
    /// called from another thread, never two at once. And that headers(),
    /// totalRows() and an *unfiltered* matchingRows() answer without touching the
    /// file at all, because the model keeps asking those where it always did: they
    /// are what it needs to know the shape of a table before it has any of it.
    virtual bool canBeReadOnATask() const { return false; }

    /// Rows in the whole table, or -1 when that is not known yet.
    ///
    /// Counting a table is a walk of it, and the rule above says who may not
    /// wait for one -- so a source whose count is expensive answers -1 until
    /// somebody has taken the count somewhere the window is not, and the view
    /// leaves the figure blank until it turns up. A source that knows its size
    /// without asking, as an import into a store does, simply always answers.
    virtual qint64 totalRows() const = 0;
    /// Rows matching `filter`, or every row when it is empty; -1 on the same
    /// terms as totalRows(). Substring, any column, case-insensitive -- a filter
    /// that searched one column would need a column picker before it was useful
    /// at all.
    virtual qint64 matchingRows(const QString& filter) const = 0;

    /// A window of rows in table order. This is the only read path the model
    /// uses, so scrolling costs one query per screen rather than one per table.
    ///
    /// `readable` is set false when the window could not be read at all, and a
    /// caller with one must not treat the empty list it got back as a window
    /// that happens to hold nothing. A source that cannot be read is not a
    /// source that is empty -- ADR-0030 decided that for a sync plan, and the
    /// same mistake here turns a grid still filling from an import into a table
    /// that reads as empty.
    virtual QList<QStringList> rows(
        qint64 offset, int limit, const QString& filter = {}, bool* readable = nullptr) const
        = 0;

    /// The longest value in each column over a sample, in characters, so the
    /// grid can size columns to their contents instead of to a default that
    /// wastes half the window.
    virtual QList<int> columnWidths(int sampleRows = 200) const = 0;
};

} // namespace mole
