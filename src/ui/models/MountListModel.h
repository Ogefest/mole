#pragma once

#include "core/vfs/VfsManager.h"

#include <QAbstractListModel>
#include <QHash>
#include <QTimer>

namespace mole {

class TaskManager;

/// The sidebar's list of drives. A local disk, an SFTP share and an in-memory
/// scratch space are all just rows here -- that uniformity is the point.
class MountListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        DisplayNameRole,
        RootUriRole,
        SchemeRole,
        IconTextRole,
        /// False for drives with no meaningful capacity -- a bucket, an
        /// archive, the scratch space. The sidebar draws nothing rather than
        /// a bar that would be read as a fact.
        HasSpaceRole,
        UsedFractionRole,
        TotalTextRole,
        FreeTextRole,
        UsedTextRole,
    };

    /// `tasks` may be null in tests that do not care about capacity; the model
    /// then simply never learns any and reports every drive as unknown.
    explicit MountListModel(VfsManager* vfs, TaskManager* tasks = nullptr, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString rootUriAt(int row) const;
    Q_INVOKABLE void unmount(int row);

    /// Re-asks every drive how full it is. Called on a timer and whenever the
    /// mount table changes; exposed so a test does not have to wait a minute.
    Q_INVOKABLE void refreshSpace();
    /// How often the periodic refresh runs. 0 stops it.
    void setRefreshInterval(int milliseconds);

signals:
    void countChanged();

private:
    void reload();

    void onSpaceReady(const QString& mountId, const SpaceInfo& info);
    int rowOfMount(const QString& mountId) const;

    VfsManager* m_vfs = nullptr;
    TaskManager* m_tasks = nullptr;
    QList<Mount> m_mounts;
    /// Keyed by mount id, so a drive keeps its figure across a reload of the
    /// table and the bar does not flicker away every time a mount is added.
    QHash<QString, SpaceInfo> m_space;
    QTimer m_refreshTimer;
};

} // namespace mole
