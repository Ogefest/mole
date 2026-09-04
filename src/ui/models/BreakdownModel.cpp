#include "ui/models/BreakdownModel.h"

#include "ui/models/FileListModel.h"

#include "core/text/SizeWords.h"

#include <algorithm>

namespace mole {

BreakdownModel::BreakdownModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

void BreakdownModel::setExtensions(QList<ExtensionStat> extensions)
{
    beginResetModel();
    m_all = std::move(extensions);
    rebuild();
    endResetModel();
    emit changed();
}

void BreakdownModel::setFilterText(const QString& text)
{
    if (m_filterText == text)
        return;
    m_filterText = text;
    beginResetModel();
    rebuild();
    endResetModel();
    emit changed();
}

void BreakdownModel::setMinimumBytes(qint64 bytes)
{
    if (m_minimumBytes == bytes)
        return;
    m_minimumBytes = bytes;
    beginResetModel();
    rebuild();
    endResetModel();
    emit changed();
}

void BreakdownModel::setByCount(bool byCount)
{
    if (m_byCount == byCount)
        return;
    m_byCount = byCount;
    beginResetModel();
    rebuild();
    endResetModel();
    emit changed();
}

void BreakdownModel::clearFilters()
{
    if (m_filterText.isEmpty() && m_minimumBytes == 0)
        return;
    m_filterText.clear();
    m_minimumBytes = 0;
    beginResetModel();
    rebuild();
    endResetModel();
    emit changed();
}

void BreakdownModel::rebuild()
{
    const QString folded = m_filterText.toLower();

    m_visible.clear();
    m_visibleBytes = 0;
    m_visibleCount = 0;
    m_peak = 0;

    for (const ExtensionStat& stat : std::as_const(m_all)) {
        if (stat.bytes < m_minimumBytes)
            continue;
        if (!folded.isEmpty() && !stat.extension.toLower().contains(folded))
            continue;

        m_visible.append(stat);
        m_visibleBytes += stat.bytes;
        m_visibleCount += stat.count;
        m_peak = std::max(m_peak, m_byCount ? stat.count : stat.bytes);
    }

    // Ranked by whichever measure the chart is showing, so the bars are always
    // in descending order rather than sorted by something invisible.
    std::sort(m_visible.begin(), m_visible.end(), [this](const ExtensionStat& a, const ExtensionStat& b) {
        return m_byCount ? a.count > b.count : a.bytes > b.bytes;
    });
}

int BreakdownModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_visible.size());
}

QVariant BreakdownModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};

    const ExtensionStat& stat = m_visible.at(index.row());
    const qint64 value = m_byCount ? stat.count : stat.bytes;

    switch (role) {
    case ExtensionRole:
    case Qt::DisplayRole:
        // An empty extension is a real group, and it needs a name on screen.
        return stat.extension.isEmpty() ? QStringLiteral("(no extension)") : stat.extension;
    case CountRole:
        return stat.count;
    case BytesRole:
        return stat.bytes;
    case SizeTextRole:
        return sizeInWords(stat.bytes);
    case ShareRole: {
        const qint64 total = m_byCount ? m_visibleCount : m_visibleBytes;
        return total > 0 ? static_cast<double>(value) / static_cast<double>(total) : 0.0;
    }
    case PeakShareRole:
        return m_peak > 0 ? static_cast<double>(value) / static_cast<double>(m_peak) : 0.0;
    default:
        return {};
    }
}

QHash<int, QByteArray> BreakdownModel::roleNames() const
{
    return {
        { ExtensionRole, "extension" },
        { CountRole, "fileCount" },
        { BytesRole, "bytes" },
        { SizeTextRole, "sizeText" },
        { ShareRole, "share" },
        { PeakShareRole, "peakShare" },
    };
}

} // namespace mole
