#pragma once

#include "core/vfs/RemoteRegistry.h"
#include "core/vfs/VfsManager.h"

#include <QAbstractListModel>
#include <QHash>
#include <QTimer>

namespace mole {

class TaskManager;

/// The sidebar's list of drives. A local disk, an SFTP share and an in-memory
/// scratch space are all just rows here -- that uniformity is the point.
///
/// It used to list what was *mounted*, and was called MountListModel for it. A
/// configured drive that was not connected right now appeared nowhere, so the
/// one place in the window that answers "what can I get at" was silent about
/// most of what somebody had set up. A row is now the drive, whether it happens
/// to be connected or not, and the name says so -- see
/// docs/adr/0008-naming-what-an-operation-touches.md.
///
/// A configured drive that is connected is **one** row, not two. The join was
/// already there: AppController::connectDrive() gives the mount the drive's own
/// id.
class DriveListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    /// What a drive is doing, as far as the window can tell.
    ///
    /// `Connected` means a backend was built and mounted, not that the far end
    /// answered -- the two are different questions and nothing here polls for
    /// the second. See docs/adr/0018-connected-is-not-reachable.md.
    ///
    /// `Connecting` and `Unreachable` have no source yet: the first needs
    /// connecting to stop being synchronous, the second needs the drive check.
    /// They are here because leaving them out would mean every reader of this
    /// enum changing when they arrive.
    enum class State {
        Local, ///< a mount with no configured drive behind it: a disk, an archive
        Disconnected, ///< configured, not connected, and could be
        Locked, ///< configured, and its secrets are in a store that is shut
        Connecting, ///< being connected right now
        Connected, ///< mounted; whether the far end answers is a separate question
        Unreachable, ///< mounted, and a check said the far end is not there
    };
    Q_ENUM(State)

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
        StateRole,
        /// The state in words, for a row that has room to say it.
        StateTextRole,
        /// The configured drive behind this row, or empty when there is none.
        /// What an action needs in order to name what it is acting on.
        ConfiguredIdRole,
        CanConnectRole,
        CanEjectRole,
    };

    /// `remotes` may be null in tests that only care about mounts, and `tasks`
    /// in tests that do not care about capacity; the model then simply never
    /// learns any and reports every drive as unknown.
    explicit DriveListModel(VfsManager* vfs, RemoteRegistry* remotes = nullptr, TaskManager* tasks = nullptr,
        QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString rootUriAt(int row) const;
    /// The configured drive at this row, or empty for a plain mount.
    Q_INVOKABLE QString configuredIdAt(int row) const;
    Q_INVOKABLE void unmount(int row);

    /// Re-asks every drive how full it is. Called on a timer and whenever the
    /// mount table changes; exposed so a test does not have to wait a minute.
    Q_INVOKABLE void refreshSpace();
    /// How often the periodic refresh runs. 0 stops it.
    void setRefreshInterval(int milliseconds);

    /// The words shown for a state. Here rather than in QML so the list and any
    /// dialog cannot end up calling the same state two different things.
    static QString stateText(State state);

signals:
    void countChanged();

private:
    /// One drive. `mount` is only filled in when it is connected, `drive` only
    /// when somebody configured it, and a configured drive that is connected
    /// has both -- which is what makes it one row instead of two.
    struct Row
    {
        Mount mount;
        RemoteDrive drive;

        bool isMounted() const { return mount.isValid(); }
        bool isConfigured() const { return drive.isValid(); }
    };

    void reload();
    State stateOf(const Row& row) const;

    void onSpaceReady(const QString& mountId, const SpaceInfo& info);
    int rowOfMount(const QString& mountId) const;

    VfsManager* m_vfs = nullptr;
    RemoteRegistry* m_remotes = nullptr;
    TaskManager* m_tasks = nullptr;
    QList<Row> m_rows;
    /// Keyed by mount id, so a drive keeps its figure across a reload of the
    /// table and the bar does not flicker away every time a mount is added.
    QHash<QString, SpaceInfo> m_space;
    QTimer m_refreshTimer;
};

} // namespace mole
