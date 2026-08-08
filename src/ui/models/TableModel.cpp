#include "ui/models/TableModel.h"

#include <limits>

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
{
}

void TableModel::setSource(ITableSource* source)
{
    beginResetModel();
    m_source = source;
    m_filter.clear();
    m_pages.clear();
    m_pageOrder.clear();
    m_headers = source ? source->headers() : QStringList {};
    m_totalRows = source ? source->totalRows() : 0;
    m_matchingRows = m_totalRows;
    m_columnWidths = source ? source->columnWidths() : QList<int> {};
    endResetModel();

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
    m_pages.clear();
    m_pageOrder.clear();
    m_headers = m_source ? m_source->headers() : QStringList {};
    m_totalRows = m_source ? m_source->totalRows() : 0;
    m_matchingRows = m_source ? m_source->matchingRows(m_filter) : 0;
    m_columnWidths = m_source ? m_source->columnWidths() : QList<int> {};
    endResetModel();

    emit tableChanged();
}

void TableModel::setFilter(const QString& filter)
{
    if (m_filter == filter)
        return;

    beginResetModel();
    m_filter = filter;
    m_pages.clear();
    m_pageOrder.clear();
    m_matchingRows = m_source ? m_source->matchingRows(m_filter) : 0;
    endResetModel();

    emit filterChanged();
    emit tableChanged();
}

QVariantList TableModel::columnWidths() const
{
    QVariantList out;
    out.reserve(m_columnWidths.size());
    for (int width : m_columnWidths)
        out.append(width);
    return out;
}

int TableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    // Clamped: QAbstractItemModel counts in int, and a file with more rows
    // than that is beyond what any view can scroll to anyway.
    return static_cast<int>(std::min<qint64>(m_matchingRows, std::numeric_limits<int>::max()));
}

int TableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_headers.size());
}

void TableModel::ensureLoaded(int row) const
{
    if (!m_source)
        return;

    const int page = row / kPageRows;
    if (m_pages.contains(page))
        return;

    QList<QStringList> rows = m_source->rows(static_cast<qint64>(page) * kPageRows, kPageRows, m_filter);
    m_pages.insert(page, std::move(rows));
    m_pageOrder.append(page);

    while (m_pageOrder.size() > kMaxCachedPages)
        m_pages.remove(m_pageOrder.takeFirst());
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

    const int page = index.row() / kPageRows;
    const auto position = m_pages.constFind(page);
    if (position == m_pages.constEnd())
        return {};

    const int offset = index.row() % kPageRows;
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
