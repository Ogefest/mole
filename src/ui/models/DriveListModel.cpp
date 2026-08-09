#include "ui/models/DriveListModel.h"

#include "core/tasks/QuerySpaceTask.h"
#include "core/tasks/TaskManager.h"

#include <QLocale>

namespace mole {

DriveListModel::DriveListModel(VfsManager* vfs, RemoteRegistry* remotes, TaskManager* tasks, QObject* parent)
    : QAbstractListModel(parent)
    , m_vfs(vfs)
    , m_remotes(remotes)
    , m_tasks(tasks)
{
    // A minute is often enough to notice a disk filling up and rare enough
    // that nobody will ever see it happen.
    m_refreshTimer.setInterval(60000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &DriveListModel::refreshSpace);

    if (m_vfs)
        connect(m_vfs, &VfsManager::mountsChanged, this, &DriveListModel::reload);
    // Both sources, because either can change what a row says: connecting a
    // drive moves the mount table, and adding, removing or unlocking one moves
    // the registry.
    if (m_remotes)
        connect(m_remotes, &RemoteRegistry::drivesChanged, this, &DriveListModel::reload);

    if (m_vfs || m_remotes)
        reload();
    if (m_tasks)
        m_refreshTimer.start();
}

void DriveListModel::setRefreshInterval(int milliseconds)
{
    if (milliseconds <= 0) {
        m_refreshTimer.stop();
        return;
    }
    m_refreshTimer.setInterval(milliseconds);
    m_refreshTimer.start();
}

void DriveListModel::reload()
{
    const QList<Mount> mounts = m_vfs ? m_vfs->mounts() : QList<Mount> {};
    const QList<RemoteDrive> drives = m_remotes ? m_remotes->drives() : QList<RemoteDrive> {};

    // Mounts that nobody configured first -- the local disks and whatever
    // archives are open -- then every configured drive, in the order the
    // registry holds them.
    //
    // Configured drives keep that place whether they are connected or not,
    // which is the whole reason for this ordering. Listing the connected ones
    // among the mounts would move a drive up the list the moment it connected
    // and back down when it dropped, and a row that moves under the pointer is
    // worse than a row that says nothing.
    QHash<QString, Mount> mountsByDriveId;
    mountsByDriveId.reserve(drives.size());
    for (const RemoteDrive& drive : drives) {
        for (const Mount& mount : mounts) {
            if (mount.id == drive.id) {
                mountsByDriveId.insert(drive.id, mount);
                break;
            }
        }
    }

    QList<Row> rows;
    rows.reserve(mounts.size() + drives.size());
    for (const Mount& mount : mounts) {
        if (!mountsByDriveId.contains(mount.id))
            rows.append(Row { mount, {} });
    }
    for (const RemoteDrive& drive : drives)
        rows.append(Row { mountsByDriveId.value(drive.id), drive });

    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    emit countChanged();

    // Figures for drives that have gone are dropped, or an id reused later
    // would inherit a stale bar.
    QHash<QString, SpaceInfo> kept;
    for (const Row& row : std::as_const(m_rows)) {
        if (row.isMounted() && m_space.contains(row.mount.id))
            kept.insert(row.mount.id, m_space.value(row.mount.id));
    }
    m_space = kept;

    refreshSpace();
}

DriveListModel::State DriveListModel::stateOf(const Row& row) const
{
    // A mount nobody configured is a local disk, an open archive or the scratch
    // space: there is no connecting or disconnecting to be done to it, and the
    // sidebar has always treated it that way.
    if (!row.isConfigured())
        return State::Local;

    if (row.isMounted()) {
        // value(), not operator[]: this is a const method, and the const
        // overload of operator[] hands back a copy through a reference that
        // reads as though it were the stored one.
        const Reachability known = m_reach.value(row.drive.id);
        // Connecting rather than Connected while the question is out. Building
        // a backend performs no I/O, so a drive pointed at a server that has
        // been switched off is mounted exactly as successfully as one that
        // works -- and must not show green on its way to showing red.
        if (known.pending)
            return State::Connecting;
        if (known.asked && !known.reachable)
            return State::Unreachable;
        return State::Connected;
    }

    if (m_remotes && m_remotes->needsUnlocking(row.drive))
        return State::Locked;
    return State::Disconnected;
}

void DriveListModel::noteCheckStarted(const QString& driveId)
{
    Reachability& known = m_reach[driveId];
    known.pending = true;
    refreshRowFor(driveId);
}

void DriveListModel::noteCheckResult(const QString& driveId, bool reachable, const QString& message)
{
    Reachability& known = m_reach[driveId];
    known.pending = false;
    known.asked = true;
    known.reachable = reachable;
    known.message = message;
    known.at = QDateTime::currentDateTime();
    refreshRowFor(driveId);
}

bool DriveListModel::saysTheDriveIsUnreachable(const VfsError& error)
{
    switch (error.code) {
    case VfsError::NetworkError:
    case VfsError::IoError:
        return true;
    default:
        break;
    }
    // Everything else is about what was asked for, not about whether anyone was
    // there to ask. NotFound and AccessDenied in particular arrive constantly
    // from ordinary browsing.
    return false;
}

void DriveListModel::noteFailureAt(const VfsUri& target, const VfsError& error)
{
    if (!m_vfs || !saysTheDriveIsUnreachable(error))
        return;
    const Mount mount = m_vfs->mountForUri(target);
    if (mount.id.isEmpty())
        return;

    // Only a configured drive has a state to move. A local disk that refused a
    // listing has a permission problem, not a reachability one, and calling it
    // unreachable would send the reader looking at their network.
    bool configured = false;
    for (const Row& row : std::as_const(m_rows)) {
        if (row.isConfigured() && row.drive.id == mount.id) {
            configured = true;
            break;
        }
    }
    if (!configured)
        return;

    Reachability& known = m_reach[mount.id];
    // A failure answers the question a pending check was asking, and answers it
    // the same way, so it is not left hanging at Connecting.
    known.pending = false;
    known.asked = true;
    known.reachable = false;
    known.message = error.message;
    known.at = QDateTime::currentDateTime();
    refreshRowFor(mount.id);
}

void DriveListModel::refreshRowFor(const QString& driveId)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        if (!m_rows.at(row).isConfigured() || m_rows.at(row).drive.id != driveId)
            continue;
        const QModelIndex index = this->index(row, 0);
        emit dataChanged(index, index,
            { StateRole, StateTextRole, StateSeverityRole, CanConnectRole, CanEjectRole, CheckMessageRole,
                CheckedAtRole });
        return;
    }
}

QString DriveListModel::stateText(State state)
{
    switch (state) {
    case State::Local:
        return QStringLiteral("Local");
    case State::Disconnected:
        return QStringLiteral("Not connected");
    case State::Locked:
        return QStringLiteral("Locked");
    case State::Connecting:
        return QStringLiteral("Connecting");
    case State::Connected:
        return QStringLiteral("Connected");
    case State::Unreachable:
        return QStringLiteral("Unreachable");
    }
    return {};
}

QString DriveListModel::stateSeverity(State state)
{
    switch (state) {
    case State::Connected:
        return QStringLiteral("good");
    case State::Locked:
    case State::Connecting:
        return QStringLiteral("attention");
    case State::Unreachable:
        return QStringLiteral("broken");
    case State::Local:
    case State::Disconnected:
        break;
    }
    // Nothing to report, which is what a local disk and a drive nobody has
    // connected yet have in common. Not a problem, and not a success either.
    return QStringLiteral("idle");
}

void DriveListModel::refreshSpace()
{
    if (!m_tasks)
        return;

    for (const Row& row : std::as_const(m_rows)) {
        if (!row.isMounted())
            continue; // a drive that is not connected has nothing to ask
        const Mount& mount = row.mount;
        if (!mount.fileSystem || !mount.fileSystem->capabilities().testFlag(VfsCapability::ReportsSpace)) {
            continue; // nothing to ask, and nothing to draw
        }

        auto* task = new QuerySpaceTask(mount.fileSystem, mount.root, mount.id);
        connect(task, &QuerySpaceTask::spaceReady, this, &DriveListModel::onSpaceReady);
        m_tasks->submit(task);
    }
}

int DriveListModel::rowOfMount(const QString& mountId) const
{
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows.at(row).isMounted() && m_rows.at(row).mount.id == mountId)
            return row;
    }
    return -1;
}

void DriveListModel::onSpaceReady(const QString& mountId, const SpaceInfo& info)
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

int DriveListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant DriveListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Row& row = m_rows.at(index.row());
    const State state = stateOf(row);

    switch (role) {
    case IdRole:
        // The mount id when there is one, and the drive's otherwise -- which are
        // the same string for a configured drive, because that is the join.
        return row.isMounted() ? row.mount.id : row.drive.id;
    case DisplayNameRole:
    case Qt::DisplayRole:
        return row.isConfigured() ? row.drive.name : row.mount.displayName;
    case RootUriRole:
        return row.isMounted() ? row.mount.root.toString() : row.drive.rootUri().toString();
    case SchemeRole:
        return row.isMounted() ? row.mount.root.scheme() : row.drive.scheme();
    case IconTextRole:
        return row.mount.iconName;
    case StateRole:
        // A plain int. The enum is not registered with QML, so handing the
        // enumerator out would give a delegate a value it could only compare
        // against numbers.
        return static_cast<int>(state);
    case StateTextRole:
        return stateText(state);
    case StateSeverityRole:
        return stateSeverity(state);
    case ConfiguredIdRole:
        return row.isConfigured() ? row.drive.id : QString();
    case CanConnectRole:
        // Locked is not "cannot connect for ever", it is "not until the store
        // is open" -- and offering the action anyway would fail every time
        // rather than say what is wrong.
        return state == State::Disconnected;
    case CanEjectRole:
        return row.isMounted();
    case CanUnlockRole:
        return state == State::Locked;
    case CheckMessageRole:
        return row.isConfigured() ? m_reach.value(row.drive.id).message : QString();
    case CheckedAtRole: {
        if (!row.isConfigured())
            return QString();
        const QDateTime at = m_reach.value(row.drive.id).at;
        return at.isValid() ? QLocale().toString(at.time(), QLocale::ShortFormat) : QString();
    }
    default:
        break;
    }

    const SpaceInfo info = row.isMounted() ? m_space.value(row.mount.id) : SpaceInfo {};
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

QHash<int, QByteArray> DriveListModel::roleNames() const
{
    return {
        { IdRole, "driveId" },
        { DisplayNameRole, "displayName" },
        { RootUriRole, "rootUri" },
        { SchemeRole, "scheme" },
        { IconTextRole, "iconText" },
        { HasSpaceRole, "hasSpace" },
        { UsedFractionRole, "usedFraction" },
        { TotalTextRole, "totalText" },
        { FreeTextRole, "freeText" },
        { UsedTextRole, "usedText" },
        { StateRole, "driveState" },
        { StateTextRole, "stateText" },
        { StateSeverityRole, "stateSeverity" },
        { ConfiguredIdRole, "configuredId" },
        { CanConnectRole, "canConnect" },
        { CanEjectRole, "canEject" },
        { CanUnlockRole, "canUnlock" },
        { CheckMessageRole, "checkMessage" },
        { CheckedAtRole, "checkedAt" },
    };
}

QString DriveListModel::rootUriAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return data(index(row, 0), RootUriRole).toString();
}

QString DriveListModel::configuredIdAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows.at(row).isConfigured() ? m_rows.at(row).drive.id : QString();
}

void DriveListModel::unmount(int row)
{
    if (!m_vfs || row < 0 || row >= m_rows.size())
        return;
    if (!m_rows.at(row).isMounted())
        return;
    m_vfs->removeMount(m_rows.at(row).mount.id);
}

} // namespace mole
