#include "ui/models/MountListModel.h"

#include "core/tasks/QuerySpaceTask.h"
#include "core/tasks/TaskManager.h"

#include <QLocale>

namespace mole {

MountListModel::MountListModel(VfsManager* vfs, TaskManager* tasks, QObject* parent)
    : QAbstractListModel(parent)
    , m_vfs(vfs)
    , m_tasks(tasks)
{
    // A minute is often enough to notice a disk filling up and rare enough
    // that nobody will ever see it happen.
    m_refreshTimer.setInterval(60000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &MountListModel::refreshSpace);

    if (m_vfs) {
        connect(m_vfs, &VfsManager::mountsChanged, this, &MountListModel::reload);
        reload();
    }
    if (m_tasks)
        m_refreshTimer.start();
}

void MountListModel::setRefreshInterval(int milliseconds)
{
    if (milliseconds <= 0) {
        m_refreshTimer.stop();
        return;
    }
    m_refreshTimer.setInterval(milliseconds);
    m_refreshTimer.start();
}

void MountListModel::reload()
{
    beginResetModel();
    m_mounts = m_vfs ? m_vfs->mounts() : QList<Mount> {};
    endResetModel();
    emit countChanged();

    // Figures for drives that have gone are dropped, or an id reused later
    // would inherit a stale bar.
    QHash<QString, SpaceInfo> kept;
    for (const Mount& mount : std::as_const(m_mounts)) {
        if (m_space.contains(mount.id))
            kept.insert(mount.id, m_space.value(mount.id));
    }
    m_space = kept;

    refreshSpace();
}

void MountListModel::refreshSpace()
{
    if (!m_tasks)
        return;

    for (const Mount& mount : std::as_const(m_mounts)) {
        if (!mount.fileSystem || !mount.fileSystem->capabilities().testFlag(VfsCapability::ReportsSpace)) {
            continue; // nothing to ask, and nothing to draw
        }

        auto* task = new QuerySpaceTask(mount.fileSystem, mount.root, mount.id);
        connect(task, &QuerySpaceTask::spaceReady, this, &MountListModel::onSpaceReady);
        m_tasks->submit(task);
    }
}

int MountListModel::rowOfMount(const QString& mountId) const
{
    for (int row = 0; row < m_mounts.size(); ++row) {
        if (m_mounts.at(row).id == mountId)
            return row;
    }
    return -1;
}

void MountListModel::onSpaceReady(const QString& mountId, const SpaceInfo& info)
{
    m_space.insert(mountId, info);

    // The mount may have been unmounted while the answer was in flight.
    const int row = rowOfMount(mountId);
    if (row < 0)
        return;

    const QModelIndex index = this->index(row, 0);
    emit dataChanged(
        index, index, { HasSpaceRole, UsedFractionRole, TotalTextRole, FreeTextRole, UsedTextRole });
}

int MountListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_mounts.size());
}

QVariant MountListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_mounts.size())
        return {};

    const Mount& mount = m_mounts.at(index.row());
    switch (role) {
    case IdRole:
        return mount.id;
    case DisplayNameRole:
    case Qt::DisplayRole:
        return mount.displayName;
    case RootUriRole:
        return mount.root.toString();
    case SchemeRole:
        return mount.root.scheme();
    case IconTextRole:
        return mount.iconName;
    default:
        break;
    }

    const SpaceInfo info = m_space.value(mount.id);
    switch (role) {
    case HasSpaceRole:
        return info.isValid();
    case UsedFractionRole:
        return info.usedFraction();
    case TotalTextRole:
        return info.isValid() ? QLocale().formattedDataSize(info.totalBytes) : QString();
    case FreeTextRole:
        return info.isValid() ? QLocale().formattedDataSize(info.freeBytes) : QString();
    case UsedTextRole:
        return info.isValid() ? QLocale().formattedDataSize(info.usedBytes()) : QString();
    default:
        return {};
    }
}

QHash<int, QByteArray> MountListModel::roleNames() const
{
    return {
        { IdRole, "mountId" },
        { DisplayNameRole, "displayName" },
        { RootUriRole, "rootUri" },
        { SchemeRole, "scheme" },
        { IconTextRole, "iconText" },
        { HasSpaceRole, "hasSpace" },
        { UsedFractionRole, "usedFraction" },
        { TotalTextRole, "totalText" },
        { FreeTextRole, "freeText" },
        { UsedTextRole, "usedText" },
    };
}

QString MountListModel::rootUriAt(int row) const
{
    if (row < 0 || row >= m_mounts.size())
        return {};
    return m_mounts.at(row).root.toString();
}

void MountListModel::unmount(int row)
{
    if (!m_vfs || row < 0 || row >= m_mounts.size())
        return;
    m_vfs->removeMount(m_mounts.at(row).id);
}

} // namespace mole
