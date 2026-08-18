#pragma once

#include "core/data/ITableSource.h"

#include <QAbstractTableModel>
#include <QHash>

class QTimer;

namespace mole {

/// Presents one page of a table to a QML TableView.
///
/// A page is kPageRows of the table, and the view is given that and nothing
/// else: rowCount() never exceeds it, and a row index here is counted from the
/// top of the page rather than from the top of the table. The footer under the
/// grid is the only place a row's number within the whole table appears.
///
/// The page exists because the offset is what costs, not the fetch. Rows have
/// always been pulled a chunk at a time -- kChunkRows of them, kMaxCachedChunks
/// kept -- so the model never holds more than a few thousand however far
/// anybody scrolls. What was unbounded was the offset it fetched *at*:
/// `LIMIT 500 OFFSET 9000000` is answered by stepping over nine million rows
/// with the interface thread waiting, and one drag of a scrollbar over the
/// whole table issues a run of them. Inside a page the largest offset any query
/// can carry is the page's own start plus kPageRows.
///
/// See ADR-0045 for why the page is a fixed five thousand rows rather than a
/// setting, and why the alternative -- one scrollbar over the whole table --
/// promises a seek no source can perform.
class TableModel : public QAbstractTableModel
{
    Q_OBJECT
    Q_PROPERTY(int rows READ rowCount NOTIFY tableChanged)
    Q_PROPERTY(int columns READ columnCount NOTIFY tableChanged)
    Q_PROPERTY(QStringList headers READ headers NOTIFY tableChanged)
    /// Rows in the file, before filtering, or -1 while the source is still
    /// working it out. A view shows a blank rather than a guess -- see
    /// ITableSource::totalRows().
    Q_PROPERTY(qint64 totalRows READ totalRows NOTIFY tableChanged)
    /// Rows the filter matched, which is what the view is showing; -1 on the
    /// same terms.
    Q_PROPERTY(qint64 matchingRows READ matchingRows NOTIFY tableChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    /// Which page is being shown, counted from nought. The footer adds one,
    /// because that is the only place a page is a thing a reader counts.
    Q_PROPERTY(int page READ page NOTIFY pageChanged)
    /// How many pages the matching rows come to; at least one, so there is
    /// always a page to be on. One means the footer has nothing to offer and
    /// hides itself, which is how a small file looks as it always did.
    Q_PROPERTY(int pageCount READ pageCount NOTIFY tableChanged)
    /// The index within the whole table of the first row on this page, counted
    /// from nought. Everything else here counts from the top of the page.
    Q_PROPERTY(qint64 firstRowOnPage READ firstRowOnPage NOTIFY pageChanged)
    /// Width hints in characters, so columns fit their contents.
    Q_PROPERTY(QVariantList columnWidths READ columnWidths NOTIFY tableChanged)

public:
    enum Role {
        CellRole = Qt::UserRole + 1,
    };

    explicit TableModel(QObject* parent = nullptr);

    /// Points the model at a table the caller owns. Passing nullptr empties it.
    /// Anything implementing ITableSource will do -- a CSV import, a SQLite
    /// table, a Parquet file -- which is the whole point of the interface.
    void setSource(ITableSource* source);
    Q_INVOKABLE void clear();
    /// Re-reads counts and drops the cache, after an import has added rows.
    Q_INVOKABLE void refresh();

    QStringList headers() const { return m_headers; }
    qint64 totalRows() const { return m_totalRows; }
    qint64 matchingRows() const { return m_matchingRows; }

    /// What has been typed, which is not always what the rows on screen were
    /// fetched with -- see setFilter().
    QString filter() const { return m_typedFilter; }
    /// Takes what has been typed. The scan it costs is deferred until the
    /// typing stops, so holding a key down does not queue one per character.
    void setFilter(const QString& filter);
    /// Applies the typed filter now, without waiting for the quiet.
    Q_INVOKABLE void applyFilter();
    /// Whether there is typing the rows on screen have not caught up with yet.
    bool isFilterPending() const;

    int page() const { return m_page; }
    int pageCount() const;
    qint64 firstRowOnPage() const;

    /// Moving between pages. A move past the last page or before the first
    /// changes nothing rather than wrapping: the ends of a table are where a
    /// reader expects to stop.
    Q_INVOKABLE void firstPage();
    Q_INVOKABLE void previousPage();
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void lastPage();

    QVariantList columnWidths() const;

    /// Column header text, or a spreadsheet-style letter when the file had no
    /// header row.
    Q_INVOKABLE QString headerAt(int column) const;
    /// One cell, for the copy action. Returns an empty string out of range.
    Q_INVOKABLE QString cellAt(int row, int column) const;
    /// A rectangular block as text, tab-separated, ready for the clipboard.
    Q_INVOKABLE QString blockAsText(int topRow, int leftColumn, int bottomRow, int rightColumn) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void tableChanged();
    void filterChanged();
    /// The page moved, or was put back to the first because what is being shown
    /// changed underneath it. A view watching this clears its cell cursor and
    /// its selection: row indices are page-relative, so a block held across a
    /// page move would name different rows.
    void pageChanged();

private:
    /// Loads the chunk containing page-relative `row` if it is not cached.
    void ensureLoaded(int row) const;
    /// Re-reads the counts the source can answer for the applied filter.
    void readCounts();
    /// Moves to `page`, clamped to the pages that exist. Drops the chunks with
    /// it: they hold rows from the page being left.
    void setPage(int page);
    /// Puts the view back on the first page, because what it was showing has
    /// changed underneath it, and drops the chunks. Called from inside a model
    /// reset, so it reports whether the page moved rather than emitting: a
    /// signal delivered while the model is mid-reset reaches a view that has
    /// let go of its rows and not yet taken the new ones.
    bool resetToFirstPage();

public:
    /// Rows on a page: what the view is offered at once. A constant rather than
    /// a preference -- see ADR-0045 -- and public because a test asserting the
    /// page holds a number of rows should say which number it means.
    static constexpr int kPageRows = 5000;

private:
    /// Rows in one fetch from the source, inside the page. One screen is well
    /// under this; a chunk per scroll step would be a query per row.
    static constexpr int kChunkRows = 500;
    /// Chunks kept before the oldest is dropped. Enough to scroll back a few
    /// screens without refetching, bounded so a long scroll cannot grow
    /// without limit -- which would defeat the entire design.
    static constexpr int kMaxCachedChunks = 24;
    /// How long the typing has to stop before the filter is applied, in
    /// milliseconds. A filter is a scan -- no index answers a substring match
    /// across every column -- so a keystroke that started one immediately meant
    /// eight scans for an eight-letter word, each one of them on this thread.
    /// Short enough to feel immediate, long enough to sit inside a keypress.
    static constexpr int kFilterQuietMs = 250;

    ITableSource* m_source = nullptr;
    QStringList m_headers;
    /// What the rows on screen were fetched with.
    QString m_filter;
    /// What has been typed, which runs ahead of it while somebody is typing.
    QString m_typedFilter;
    QTimer* m_filterTimer = nullptr;
    qint64 m_totalRows = 0;
    qint64 m_matchingRows = 0;
    int m_page = 0;
    QList<int> m_columnWidths;

    /// Fetched chunks, keyed by their index within the current page.
    mutable QHash<int, QList<QStringList>> m_chunks;
    mutable QList<int> m_chunkOrder;
};

} // namespace mole
