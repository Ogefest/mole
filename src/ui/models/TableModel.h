#pragma once

#include "core/data/ITableSource.h"

#include <QAbstractTableModel>
#include <QHash>

class QTimer;

namespace mole {

/// Presents an imported delimited file to a QML TableView.
///
/// Rows are fetched a window at a time from the store rather than held here,
/// so a file with fifty million rows costs the same as one with fifty. The
/// model reports the true row count and pulls whatever the view actually
/// scrolls to -- there is no cap, and no point at which the table quietly
/// stops being the file.
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

private:
    /// Loads the page containing `row` if it is not already cached.
    void ensureLoaded(int row) const;
    /// Re-reads the counts the source can answer for the applied filter.
    void readCounts();

    /// One screen is well under this; a page per scroll step would be a query
    /// per row.
    static constexpr int kPageRows = 500;
    /// Pages kept before the oldest is dropped. Enough to scroll back a few
    /// screens without refetching, bounded so a long scroll cannot grow
    /// without limit -- which would defeat the entire design.
    static constexpr int kMaxCachedPages = 24;
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
    QList<int> m_columnWidths;

    mutable QHash<int, QList<QStringList>> m_pages;
    mutable QList<int> m_pageOrder;
};

} // namespace mole
