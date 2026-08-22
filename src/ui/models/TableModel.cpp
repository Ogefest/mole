#include "ui/models/TableModel.h"

#include <QTimer>

#include <algorithm>

namespace mole {
namespace {

    /// "A", "B", ... "AA" -- what a spreadsheet calls a column with no name.
    QString spreadsheetLetter(int column)
    {
        QString name;
        int value = column;
        do {
            name.prepend(QChar(QLatin1Char('A' + value % 26)));
            value = value / 26 - 1;
        } while (value >= 0);
        return name;
    }

} // namespace

TableModel::TableModel(QObject* parent)
    : QAbstractTableModel(parent)
    , m_filterTimer(new QTimer(this))
{
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(kFilterQuietMs);
    connect(m_filterTimer, &QTimer::timeout, this, &TableModel::applyFilter);
}

void TableModel::setSource(ITableSource* source)
{
    beginResetModel();
    m_source = source;
    m_filter.clear();
    m_typedFilter.clear();
    m_filterTimer->stop();
    m_headers = source ? source->headers() : QStringList {};
    m_columnWidths = source ? source->columnWidths() : QList<int> {};
    readCounts();
    const bool moved = resetToFirstPage();
    endResetModel();

    if (moved)
        emit pageChanged();
    emit tableChanged();
    emit filterChanged();
}

void TableModel::clear()
{
    setSource(nullptr);
}

void TableModel::refresh()
{
    beginResetModel();
    m_chunks.clear();
    m_chunkOrder.clear();
    m_headers = m_source ? m_source->headers() : QStringList {};
    m_columnWidths = m_source ? m_source->columnWidths() : QList<int> {};
    readCounts();
    // The page it was on may be gone: the delimited viewer refreshes while rows
    // are still arriving from an import, and a filter narrowing the table can
    // take the far end of it away.
    const bool moved = m_page >= pageCount() && resetToFirstPage();
    endResetModel();

    if (moved)
        emit pageChanged();
    emit tableChanged();
}

void TableModel::readCounts()
{
    // Whatever the source can answer without going to look. A count it has not
    // taken yet comes back as -1, which travels: the view shows a blank until
    // it lands rather than a nought that would read as an empty table.
    m_totalRows = m_source ? m_source->totalRows() : 0;
    m_matchingRows = m_source ? m_source->matchingRows(m_filter) : 0;
}

void TableModel::setFilter(const QString& filter)
{
    if (m_typedFilter == filter)
        return;
    m_typedFilter = filter;

    // Not applied here. Counting the matches is a scan of the table -- no index
    // answers a substring match across every column -- and applying it per
    // keystroke made an eight-letter word eight scans on this thread. So the
    // typing is taken now and the scan waits for it to stop.
    m_filterTimer->start();
    emit filterChanged();
}

void TableModel::applyFilter()
{
    m_filterTimer->stop();
    if (m_filter == m_typedFilter)
        return;

    beginResetModel();
    m_filter = m_typedFilter;
    m_matchingRows = m_source ? m_source->matchingRows(m_filter) : 0;
    // A different set of rows is a different table as far as the page is
    // concerned, and page seven of the old one means nothing in the new.
    const bool moved = resetToFirstPage();
    endResetModel();

    if (moved)
        emit pageChanged();
    emit tableChanged();
}

bool TableModel::isFilterPending() const
{
    return m_filter != m_typedFilter;
}

QVariantList TableModel::columnWidths() const
{
    QVariantList out;
    out.reserve(m_columnWidths.size());
    for (int width : m_columnWidths)
        out.append(width);
    return out;
}

int TableModel::pageCount() const
{
    // At least one, so there is always a page to be on -- an empty table, and a
    // source that has not finished counting, are both one page with nothing on
    // it rather than none.
    if (m_matchingRows <= 0)
        return 1;
    return static_cast<int>((m_matchingRows + kPageRows - 1) / kPageRows);
}

qint64 TableModel::firstRowOnPage() const
{
    return static_cast<qint64>(m_page) * kPageRows;
}

void TableModel::setPage(int page)
{
    const int wanted = std::clamp(page, 0, pageCount() - 1);
    if (wanted == m_page)
        return; // past the end, or before the start: nothing changes

    beginResetModel();
    m_page = wanted;
    // The chunks hold rows from the page being left, and they are keyed by
    // their position within it.
    m_chunks.clear();
    m_chunkOrder.clear();
    endResetModel();

    emit pageChanged();
    emit tableChanged();
}

bool TableModel::resetToFirstPage()
{
    m_chunks.clear();
    m_chunkOrder.clear();
    if (m_page == 0)
        return false;
    m_page = 0;
    return true;
}

void TableModel::firstPage()
{
    setPage(0);
}

void TableModel::previousPage()
{
    setPage(m_page - 1);
}

void TableModel::nextPage()
{
    setPage(m_page + 1);
}

void TableModel::lastPage()
{
    setPage(pageCount() - 1);
}

int TableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    // The rows of this page, never the rows of the table. A source that has not
    // finished counting answers -1, which is no rows to show yet rather than a
    // number to hand a view.
    if (m_matchingRows <= 0)
        return 0;
    return static_cast<int>(std::clamp<qint64>(m_matchingRows - firstRowOnPage(), 0, kPageRows));
}

int TableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_headers.size());
}

void TableModel::ensureLoaded(int row) const
{
    if (!m_source)
        return;

    const int chunk = row / kChunkRows;
    if (m_chunks.contains(chunk))
        return;

    // The page's own start, plus where in the page this chunk begins. This is
    // the only place the two coordinate systems meet.
    const qint64 offset = firstRowOnPage() + static_cast<qint64>(chunk) * kChunkRows;
    bool readable = true;
    QList<QStringList> rows = m_source->rows(offset, kChunkRows, m_filter, &readable);
    // A window that could not be read is not cached. The cache is only cleared
    // when something else changes -- a refresh, a page move, a filter -- so a
    // chunk stored from a failed read would leave that stripe of the grid blank
    // until one of those happened to come along, which during an import is the
    // next batch and may be seconds away. Asking again costs one query; the
    // alternative is showing nothing and calling it the file's contents.
    if (!readable)
        return;
    m_chunks.insert(chunk, std::move(rows));
    m_chunkOrder.append(chunk);

    while (m_chunkOrder.size() > kMaxCachedChunks)
        m_chunks.remove(m_chunkOrder.takeFirst());
}

QVariant TableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || (role != CellRole && role != Qt::DisplayRole))
        return {};
    if (index.row() < 0 || index.row() >= rowCount() || index.column() < 0
        || index.column() >= columnCount()) {
        return {};
    }

    ensureLoaded(index.row());

    const int chunk = index.row() / kChunkRows;
    const auto position = m_chunks.constFind(chunk);
    if (position == m_chunks.constEnd())
        return {};

    const int offset = index.row() % kChunkRows;
    if (offset >= position->size())
        return {};

    const QStringList& row = position->at(offset);
    return index.column() < row.size() ? row.at(index.column()) : QString();
}

QHash<int, QByteArray> TableModel::roleNames() const
{
    return {
        { CellRole, "cell" },
        { Qt::DisplayRole, "display" },
    };
}

QString TableModel::headerAt(int column) const
{
    if (column < 0 || column >= m_headers.size())
        return {};
    const QString name = m_headers.at(column);
    return name.isEmpty() ? spreadsheetLetter(column) : name;
}

QString TableModel::cellAt(int row, int column) const
{
    return data(index(row, column), CellRole).toString();
}

QString TableModel::blockAsText(int topRow, int leftColumn, int bottomRow, int rightColumn) const
{
    const int top = std::max(0, std::min(topRow, bottomRow));
    const int bottom = std::min(rowCount() - 1, std::max(topRow, bottomRow));
    const int left = std::max(0, std::min(leftColumn, rightColumn));
    const int right = std::min(columnCount() - 1, std::max(leftColumn, rightColumn));
    if (top > bottom || left > right)
        return {};

    // Tab-separated, which is what every spreadsheet expects on paste. A cell
    // containing a tab or a newline is quoted so the shape survives.
    QStringList lines;
    for (int row = top; row <= bottom; ++row) {
        QStringList cells;
        for (int column = left; column <= right; ++column) {
            QString value = cellAt(row, column);
            if (value.contains(QLatin1Char('\t')) || value.contains(QLatin1Char('\n'))
                || value.contains(QLatin1Char('"'))) {
                value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
                value = QLatin1Char('"') + value + QLatin1Char('"');
            }
            cells.append(value);
        }
        lines.append(cells.join(QLatin1Char('\t')));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace mole
