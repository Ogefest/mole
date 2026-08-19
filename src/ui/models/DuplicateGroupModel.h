#pragma once

#include "core/duplicates/FindDuplicatesTask.h"

#include <QAbstractListModel>
#include <QLocale>
#include <QSet>

#include <functional>

namespace mole {

/// The groups a duplicate scan has confirmed, as a model rather than as a list.
///
/// The difference is the whole of this class. A `QVariantList` carries no notion
/// of a row being added: a new list is a wholesale replacement, so every group
/// delegate and every per-file row inside it is destroyed and built again, and
/// the scroll position goes with them. Since ADR-0043 a group is announced the
/// instant it is confirmed, so a scan of a tree with many duplicates in it did
/// that once per group -- and built every group's rows from scratch each time,
/// which is G² maps and twice as many formatting calls, all on the drawing
/// thread. The window missed every frame for seconds at a time.
///
/// So an insertion is an insertion: one `beginInsertRows()` for a confirmed
/// group, in the place the scan worked out for it, and the rest of the list is
/// left alone. Rows still move -- that is ADR-0043's ordering, untouched -- but
/// nothing is rebuilt around them. The per-copy rows are formatted in `data()`
/// for the rows a view actually asks about rather than for every file in the
/// result on every update, and the totals are kept as they change rather than
/// found again by walking every group.
///
/// The selection lives here too, because that is what lets a tick cost one row:
/// the model knows which row holds a uri, so ticking a copy is a `dataChanged`
/// over one index instead of a new list for the view to take apart.
class DuplicateGroupModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        /// How many copies are in this group. Not spelled `count`: a delegate
        /// property of that name would collide with the one every view attaches.
        CopiesRole = Qt::UserRole + 1,
        /// What one copy takes -- they are all the same size.
        SizeTextRole,
        /// What keeping one and removing the rest would free.
        ReclaimableTextRole,
        /// One map per copy: uri, name, location, sizeText, modifiedText and
        /// whether it is ticked. Built on being asked for.
        FilesRole,
    };

    explicit DuplicateGroupModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// One confirmed group, at the position the scan worked out for it.
    void insertGroup(const DuplicateGroup& group, int position);
    /// Forgets everything, results and ticks alike. A no-op when there is
    /// nothing to forget, so a first scan does not announce a reset it has no
    /// reason to: a reset is the one thing this class exists to avoid, and a
    /// spurious one would be indistinguishable from the fault.
    void clear();

    const QList<DuplicateGroup>& groups() const { return m_groups; }
    int copyCount() const { return m_copyCount; }
    qint64 reclaimableBytes() const { return m_reclaimable; }

    bool isSelected(const QString& uri) const { return m_selected.contains(uri); }
    int selectedCount() const { return static_cast<int>(m_selected.size()); }
    qint64 selectedBytes() const { return m_selectedBytes; }
    QStringList selectedUris() const;

    /// Ticks or unticks one copy. One row changes, and one row is announced.
    void toggle(const QString& uri);
    /// Keeps this copy and ticks every other one in its group. Still one row:
    /// the point of a per-group override is that the other groups are untouched.
    void keepOnly(const QString& uri);
    /// Ticks everything in every group except the one `chooseKeeper` picks. Every
    /// row changes, and they are announced together rather than one at a time.
    void selectAllBut(const std::function<int(const QList<FileEntry>&)>& chooseKeeper);
    void clearSelection();

signals:
    void countChanged();

private:
    /// The row the uri sits in, or -1. Linear, and asked once per click.
    int rowOf(const QString& uri) const;
    /// Announces every row at once, for the operations that change all of them.
    void announceEveryRow();
    /// The ticked bytes, worked out again from the selection. Called only by the
    /// operations that replace the whole selection; a single tick adjusts the
    /// running figure instead.
    void recountSelectedBytes();

    QList<DuplicateGroup> m_groups;
    QSet<QString> m_selected;
    /// Kept as they change rather than found by walking every group, because
    /// they are read on every update and there may be five hundred groups.
    qint64 m_reclaimable = 0;
    int m_copyCount = 0;
    qint64 m_selectedBytes = 0;
    QLocale m_locale;
};

} // namespace mole
