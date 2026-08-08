#pragma once

#include "core/analysis/AnalysisReport.h"

#include <QAbstractListModel>

namespace mole {

/// The per-extension breakdown of a report, filterable.
///
/// Filtering is what turns a wall of numbers into an answer: hiding everything
/// under a megabyte, or narrowing to one extension, is usually how you find out
/// what a directory is actually full of. The totals follow the filter, so they
/// always describe what is on screen.
class BreakdownModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY changed)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY changed)
    Q_PROPERTY(qint64 minimumBytes READ minimumBytes WRITE setMinimumBytes NOTIFY changed)
    Q_PROPERTY(bool byCount READ byCount WRITE setByCount NOTIFY changed)
    /// Totals of the rows that survived the filter.
    Q_PROPERTY(qint64 visibleBytes READ visibleBytes NOTIFY changed)
    Q_PROPERTY(qint64 visibleCount READ visibleCount NOTIFY changed)
    /// The largest single value on screen, so bars can be scaled to it.
    Q_PROPERTY(qint64 peak READ peak NOTIFY changed)
    Q_PROPERTY(int hiddenRows READ hiddenRows NOTIFY changed)

public:
    enum Role {
        ExtensionRole = Qt::UserRole + 1,
        CountRole,
        BytesRole,
        SizeTextRole,
        ShareRole, ///< 0..1 of the visible total, for a stacked bar
        PeakShareRole, ///< 0..1 of the largest row, for a bar chart
    };

    explicit BreakdownModel(QObject* parent = nullptr);

    void setExtensions(QList<ExtensionStat> extensions);

    QString filterText() const { return m_filterText; }
    void setFilterText(const QString& text);
    qint64 minimumBytes() const { return m_minimumBytes; }
    void setMinimumBytes(qint64 bytes);
    /// Rank and scale by file count instead of by size.
    bool byCount() const { return m_byCount; }
    void setByCount(bool byCount);

    qint64 visibleBytes() const { return m_visibleBytes; }
    qint64 visibleCount() const { return m_visibleCount; }
    qint64 peak() const { return m_peak; }
    int hiddenRows() const { return static_cast<int>(m_all.size() - m_visible.size()); }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void clearFilters();

signals:
    void changed();

private:
    void rebuild();

    QList<ExtensionStat> m_all;
    QList<ExtensionStat> m_visible;
    QString m_filterText;
    qint64 m_minimumBytes = 0;
    bool m_byCount = false;
    qint64 m_visibleBytes = 0;
    qint64 m_visibleCount = 0;
    qint64 m_peak = 0;
};

} // namespace mole
