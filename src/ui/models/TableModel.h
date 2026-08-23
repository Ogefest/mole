#pragma once

#include "core/data/ITableSource.h"
#include "core/data/ReadTableTask.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QPointer>

#include <memory>

class QTimer;

namespace mole {

class TaskManager;

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
    /// True while something has been asked of the source and not yet answered.
    /// Only ever true for a source read on a task; a view says so where it says
    /// everything else, because a grid filling in a moment later must not read as
    /// a grid with holes in it.
    Q_PROPERTY(bool reading READ isReading NOTIFY readingChanged)

public:
    enum Role {
        CellRole = Qt::UserRole + 1,
    };

    explicit TableModel(QObject* parent = nullptr);

    /// Points the model at a table. Passing nothing empties it. Anything
    /// implementing ITableSource will do -- a CSV import, a SQLite table, a
    /// Parquet file -- which is the whole point of the interface.
    ///
    /// **Ownership is shared, and with `tasks` the reading leaves this thread.**
    /// A source that says it may be read on a task -- see
    /// ITableSource::canBeReadOnATask() -- has its windows, its filtered counts
    /// and its column widths asked for there, and this model fills them in as they
    /// land. The share is what makes that safe: a reader steps off a file while a
    /// read is running as a matter of course, and the answer arrives into a source
    /// that is still there. Without `tasks` every source is read inline, exactly as
    /// it always was.
    void setSource(std::shared_ptr<ITableSource> source, TaskManager* tasks = nullptr);
    Q_INVOKABLE void clear();
    /// Re-reads counts and drops the cache, after an import has added rows.
    Q_INVOKABLE void refresh();

    QStringList headers() const { return m_headers; }
    bool isReading() const;
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
    void readingChanged();
    /// The page moved, or was put back to the first because what is being shown
    /// changed underneath it. A view watching this clears its cell cursor and
    /// its selection: row indices are page-relative, so a block held across a
    /// page move would name different rows.
    void pageChanged();

private:
    /// Loads the chunk containing page-relative `row` if it is not cached. For a
    /// source read on a task, "loads" means asks: the answer arrives later and the
    /// cells stay blank until it does.
    void ensureLoaded(int row) const;
    /// Whether reads of the current source go on a task.
    bool readsOnATask() const;
    /// Asks for `chunk` unless it is already asked for.
    void requestChunk(int chunk) const;
    /// Starts the next thing wanted, if nothing is outstanding. Counts first,
    /// because a count decides how many rows the view is offered at all.
    void pumpReads() const;
    /// Takes an answer that has landed.
    void absorb(ReadTableTask* task, int chunk);
    /// Cancels what is outstanding and forgets what was queued. Called whenever
    /// what the view is showing changes underneath it: those answers are about a
    /// page, or a filter, that has been left.
    void abandonReads();
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
    /// Rows sampled to size the columns. A hint about how to draw a grid, not a
    /// claim about the file, so it is a sample and a small one.
    static constexpr int kWidthSampleRows = 200;
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

    std::shared_ptr<ITableSource> m_source;
    /// Where a read is submitted, and null for a model that reads inline.
    TaskManager* m_tasks = nullptr;
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

    // Reads that have been asked for and not yet answered. Mutable for the same
    // reason the cache above is: they are reached from data(), which Qt makes
    // const, and a read that has to happen is not a change to what the table says.
    /// The one read allowed to be outstanding, because a source is asked one
    /// question at a time.
    mutable QPointer<ReadTableTask> m_reading;
    /// Which chunk that read is for, or -1 when it is not a window.
    mutable int m_readingChunk = -1;
    /// Chunks wanted, in the order the view asked for them.
    mutable QList<int> m_wantedChunks;
    mutable bool m_wantCount = false;
    mutable bool m_wantWidths = false;
};

} // namespace mole
