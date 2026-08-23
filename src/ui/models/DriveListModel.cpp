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

    // Busy is learnt from what the application already tells itself: a task is
    // appended, one starts, one ends. Nothing is polled, and no drive is asked --
    // the rule ADR-0018 set and ADR-0052 keeps.
    if (m_tasks) {
        connect(m_tasks, &TaskManager::taskAppended, this, [this](Task* task) {
            if (task)
                connect(task, &Task::stateChanged, this, &DriveListModel::refreshBusyDrives);
            refreshBusyDrives();
        });
        connect(m_tasks, &TaskManager::activeCountChanged, this, &DriveListModel::refreshBusyDrives);
        connect(m_tasks, &TaskManager::tasksReset, this, &DriveListModel::refreshBusyDrives);
    }

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
        // A mount that exists to be read is not a drive, and neither is an
        // archive somebody is walking around in: neither has a place in a list of
        // places to go. See Mount::internal and Mount::unlisted.
        if (mount.internal || mount.unlisted)
            continue;
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
    // Highest first, and the order is the whole of the answer: Unreachable →
    // Busy → Open → Connecting → Not connected → Idle. A drive that is open and
    // has work running on it reads busy, because that is the more specific
    // statement; a drive nothing can reach reads unreachable whatever is being
    // attempted on it.
    //
    // value(), not operator[]: this is a const method, and the const overload of
    // operator[] hands back a copy through a reference that reads as though it
    // were the stored one.
    if (row.isConfigured()) {
        const Reachability known = m_reach.value(row.drive.id);
        if (row.isMounted() && !known.pending && known.asked && !known.reachable)
            return State::Unreachable;
    }

    // What the window is doing with it, which is the same question for a local
    // disk, an archive, a bucket and a server -- and is why State::Local is gone.
    // Busy outranks Open: a drive somebody is looking at *and* copying to is being
    // copied to, which is the more specific statement.
    if (row.isMounted() && m_busyMounts.contains(row.mount.id))
        return State::Busy;
    if (row.isMounted() && m_openMounts.contains(row.mount.id))
        return State::Open;

    if (row.isConfigured()) {
        // Connecting rather than Idle while the question is out. Building a
        // backend performs no I/O, so a drive pointed at a server that has been
        // switched off is mounted exactly as successfully as one that works.
        const Reachability known = m_reach.value(row.drive.id);
        if (row.isMounted())
            return known.pending ? State::Connecting : State::Idle;
        if (m_remotes && m_remotes->needsUnlocking(row.drive))
            return State::Locked;
        return State::Disconnected;
    }

    // A mount nobody configured -- a disk, an open archive, the scratch space.
    // There is nothing to connect or disconnect, which is expressed by it never
    // reaching the three states above rather than by a state of its own.
    return State::Idle;
}

void DriveListModel::noteOpenLocations(const QList<VfsUri>& locations)
{
    QSet<QString> open;
    if (m_vfs) {
        for (const VfsUri& uri : locations) {
            if (!uri.isValid())
                continue;
            const Mount mount = m_vfs->mountForUri(uri);
            if (!mount.id.isEmpty())
                open.insert(mount.id);
        }
    }
    if (open == m_openMounts)
        return;

    // Only the rows whose answer changed. The set is small and the list is
    // short, but a reset here would rebuild every delegate every time somebody
    // walked into a folder.
    const QSet<QString> changed = (open - m_openMounts) + (m_openMounts - open);
    m_openMounts = open;
    announceStateOf(changed);
}

void DriveListModel::refreshBusyDrives()
{
    QSet<QString> busy;
    if (m_tasks && m_vfs) {
        const QList<Task*> tasks = m_tasks->tasks();
        for (Task* task : tasks) {
            if (!task)
                continue;
            // Work the user asked for, and running now. Housekeeping the
            // application does on its own behalf lights nothing: QuerySpaceTask
            // runs per mount every minute to keep the capacity bars honest, and if
            // it counted, every drive in the list would pulse once a minute for
            // ever and the feature would be noise in its first hour. A finished
            // task stops counting the instant it finishes, including one that
            // failed or was cancelled -- a drive left pulsing after a failed copy
            // is the kind of thing nobody notices in review and everybody notices
            // in use.
            if (task->isBackground() || task->state() != Task::State::Running)
                continue;
            const QList<VfsUri> touching = task->touching();
            for (const VfsUri& uri : touching) {
                const Mount mount = m_vfs->mountForUri(uri);
                if (!mount.id.isEmpty())
                    busy.insert(mount.id);
            }
        }
    }
    if (busy == m_busyMounts)
        return;

    const QSet<QString> changed = (busy - m_busyMounts) + (m_busyMounts - busy);
    m_busyMounts = busy;
    announceStateOf(changed);
}

void DriveListModel::announceStateOf(const QSet<QString>& mounts)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        if (!m_rows.at(row).isMounted() || !mounts.contains(m_rows.at(row).mount.id))
            continue;
        const QModelIndex at = index(row, 0);
        emit dataChanged(
            at, at, { StateRole, StateTextRole, StateSeverityRole, DotFilledRole, DotMotionRole });
    }
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
            { StateRole, StateTextRole, StateSeverityRole, DotFilledRole, DotMotionRole, CanConnectRole,
                CanEjectRole, CheckMessageRole, CheckedAtRole });
        return;
    }
}

QString DriveListModel::stateText(State state)
{
    switch (state) {
    case State::Idle:
        return QStringLiteral("Idle");
    case State::Open:
        return QStringLiteral("Open");
    case State::Busy:
        return QStringLiteral("Busy");
    case State::Disconnected:
        return QStringLiteral("Not connected");
    case State::Locked:
        return QStringLiteral("Locked");
    case State::Connecting:
        return QStringLiteral("Connecting");
    case State::Unreachable:
        return QStringLiteral("Unreachable");
    }
    return {};
}

bool DriveListModel::stateFillsTheDot(State state)
{
    switch (state) {
    // Not here yet: a drive that could be connected and is not. A ring says that
    // at eight pixels where a shade of grey cannot -- which is the whole fault
    // this fixes, since Idle wore the same grey and means the opposite.
    case State::Disconnected:
    case State::Locked:
    case State::Connecting:
        return false;
    case State::Idle:
    case State::Open:
    case State::Busy:
    case State::Unreachable:
        break;
    }
    return true;
}

QString DriveListModel::stateMotion(State state)
{
    // Motion is *happening right now*, and only that. Two states are transient,
    // and the word says which of the two, not what it looks like -- the same
    // division of labour `stateSeverity()` has, where the model answers a meaning
    // and the view knows how this interface paints it. Both of them breathe, and
    // the view draws waiting deep and quick and work shallow and slow. See the
    // second 2026-08-19 revision in
    // docs/adr/0052-a-drives-dot-says-what-it-is-doing.md.
    if (state == State::Connecting)
        return QStringLiteral("waiting");
    if (state == State::Busy)
        return QStringLiteral("working");
    return {};
}

QString DriveListModel::stateSeverity(State state)
{
    switch (state) {
    // Yours, and in use. The accent is not a new colour: it is what this
    // interface already means by *this is the thing you are on*.
    case State::Open:
        return QStringLiteral("using");
    // Work going through, which is a different statement from "you are on this"
    // and gets a channel of its own rather than sharing the accent. See the
    // 2026-08-19 revision in docs/adr/0052-a-drives-dot-says-what-it-is-doing.md.
    case State::Busy:
        return QStringLiteral("working");
    case State::Unreachable:
        return QStringLiteral("broken");
    case State::Idle:
    case State::Connecting:
    case State::Disconnected:
    // A drive whose password is in a shut store is not a problem: the store is
    // shut at every startup and may stay shut all session, and nothing has gone
    // wrong until somebody asks for that drive. So Locked reads muted with the
    // rest of the not-yet, and the word keeps the distinction: the row still says
    // Locked and still offers the key rather than the play triangle.
    case State::Locked:
        break;
    }
    // Nothing of yours. A quiet drive and one nobody has connected share this
    // colour and are told apart by the shape of the dot -- which is the fault
    // this replaces, where they shared both.
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
        // A mount that was told its size is not measured. Nothing on a real
        // machine sets this; a drive configured through MOLE_DRIVES does, so a
        // window photographed for the guide shows the same figures every time.
        if (mount.declaredSpace.isValid()) {
            onSpaceReady(mount.id, mount.declaredSpace);
            continue;
        }
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
    case DotFilledRole:
        return stateFillsTheDot(state);
    case DotMotionRole:
        return stateMotion(state);
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
        { DotFilledRole, "dotFilled" },
        { DotMotionRole, "dotMotion" },
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
