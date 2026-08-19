#pragma once

#include "core/vfs/RemoteRegistry.h"
#include "core/vfs/VfsManager.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QSet>
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
    /// What a drive is *doing*, which is one question for every kind of drive.
    ///
    /// It used to be a mixture. A mount with no configured drive behind it -- a
    /// disk, an open archive, the scratch space -- was `Local`, which said where
    /// it was rather than what was happening to it, and it shared the sidebar's
    /// grey with `Disconnected`, where the grey means something real. A local
    /// disk is not doing anything and is not waiting to be connected either. See
    /// docs/adr/0052-a-drives-dot-says-what-it-is-doing.md.
    ///
    /// Whether anybody has a drive open, and whether work is running on it, are
    /// knowable without asking the drive anything -- and they are the same
    /// question for a disk, an archive, a bucket and an SFTP server. So all of
    /// them wear the same dot, and `Local` is not a state.
    ///
    /// `Connected` is gone with it: a mounted drive nobody is using is `Idle`.
    /// Whether the far end answers is still a separate question that nothing here
    /// polls for -- see docs/adr/0018-connected-is-not-reachable.md, whose
    /// decisions this leaves untouched.
    ///
    /// **Highest first: Unreachable → Busy → Open → Connecting → Not connected →
    /// Idle.** A drive that is open *and* has work running on it reads busy,
    /// because that is the more specific statement.
    enum class State {
        Idle, ///< available, and nothing is using it
        Open, ///< some tab's current location is on this drive
        Busy, ///< work the user asked for is running on it
        Disconnected, ///< configured, not connected, and could be
        Locked, ///< configured, and its secrets are in a store that is shut
        Connecting, ///< being connected right now
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
        /// The state reduced to what it means: "idle", "good", "attention" or
        /// "broken". The view turns that into a colour from its own palette.
        ///
        /// A role rather than the view reading the enum, because nothing
        /// registers this type with QML -- a delegate could only compare the
        /// state against bare numbers, and reordering the enum would silently
        /// recolour every row. Where the line falls: the model knows what a
        /// state means, the view knows what the palette calls that meaning.
        StateSeverityRole,
        /// Whether the dot is a solid disc or a hollow ring, and whether it
        /// pulses.
        ///
        /// Shape and motion rather than colour, and here for the same reason the
        /// severity is a word: the model knows what a state means, the view knows
        /// how this interface draws that meaning. Six states will not fit in one
        /// colour, so the encoding is spread over channels that each carry one
        /// idea -- hollow against filled is *not here yet* against *here*, and
        /// motion is *happening right now* and only that.
        DotFilledRole,
        /// How the dot moves: empty, `waiting` or `working`.
        ///
        /// A word rather than a flag, because two states move and they must not
        /// move alike -- and a word for what the motion *means*, leaving the view
        /// to choose the curve. `Connecting` is waiting for an answer that has not
        /// come back. `Busy` is work going through.
        DotMotionRole,
        /// The configured drive behind this row, or empty when there is none.
        /// What an action needs in order to name what it is acting on.
        ConfiguredIdRole,
        CanConnectRole,
        CanEjectRole,
        /// This drive is waiting on the credential store, not on a button.
        CanUnlockRole,
        /// What the last check said, and when it was taken. Empty until
        /// something has asked. "Not reachable" with no when is not something
        /// anyone can act on.
        CheckMessageRole,
        CheckedAtRole,
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

    // ---- what is known about reaching a drive ----------------------------
    //
    // Told to the model rather than found out by it. Nothing here polls: a
    // repeating check would be steady network traffic against every configured
    // drive whether or not anyone is looking at the sidebar, and QuerySpaceTask
    // cannot stand in for one because it deliberately says nothing when a
    // backend cannot answer -- unknown capacity is normal for a bucket, so its
    // silence cannot be read as unreachable. See
    // docs/adr/0018-connected-is-not-reachable.md.

    /// A check has been asked for and has not answered yet. Until it does, a
    /// drive that has just been connected reads as Connecting rather than
    /// Connected -- so a drive pointed at a server that is switched off never
    /// shows green on its way to showing red.
    void noteCheckStarted(const QString& driveId);
    /// What a check found, with the moment it was taken.
    void noteCheckResult(const QString& driveId, bool reachable, const QString& message);
    /// An operation against this location failed. Whatever the drive said when
    /// it was connected, something has just tried and been refused.
    ///
    /// Only errors that are about the drive count. A file that is not there, a
    /// permission that was refused, an operation the backend does not support:
    /// none of those says the server has gone, and marking the drive
    /// unreachable for them would send the reader looking at their network
    /// because they typed a name wrong.
    void noteFailureAt(const VfsUri& target, const VfsError& error);
    /// Whether this error says something about the drive rather than about the
    /// thing that was asked for.
    static bool saysTheDriveIsUnreachable(const VfsError& error);

    /// Re-asks every drive how full it is. Called on a timer and whenever the
    /// mount table changes; exposed so a test does not have to wait a minute.
    Q_INVOKABLE void refreshSpace();
    /// How often the periodic refresh runs. 0 stops it.
    void setRefreshInterval(int milliseconds);

    /// The words shown for a state. Here rather than in QML so the list and any
    /// dialog cannot end up calling the same state two different things.
    static QString stateText(State state);
    /// What a state means, for a view that has to pick a colour for it.
    ///
    /// Three answers now, not four: `idle` for nothing of yours, `using` for
    /// yours and in use, `broken` for a drive a check has failed. The green that
    /// used to mean *connected* is gone -- under this model that state is Idle,
    /// available and unused, and a colour of its own for it would be a
    /// celebration of nothing happening. Dropping it also settles an
    /// accessibility fault nobody filed: green against red is the one pair
    /// deuteranopia cannot separate, and it was carrying connected against
    /// unreachable.
    static QString stateSeverity(State state);
    /// Whether this state fills the dot or leaves it a ring. Hollow is *not here
    /// yet*: a drive that could be connected and is not.
    static bool stateFillsTheDot(State state);
    /// How this state moves: `waiting`, `working`, or nothing at all.
    ///
    /// Only the two transient states move, and they must not move alike. Both
    /// breathe; the view draws *waiting for an answer* as a deep, quicker breath
    /// and *work going through* as a shallow, slower one.
    static QString stateMotion(State state);

    // ---- what the window is doing with a drive ----------------------------

    /// Which locations the window has open, so a drive can say that somebody is
    /// on it.
    ///
    /// Told rather than found out, like everything else here: `Open` changes when
    /// a pane navigates, and the shell already learns that from the tab. Nothing
    /// is polled and no drive is contacted -- which is more compliant with
    /// ADR-0018 than the state it replaces, since reachability needs a check to
    /// be learnt and this needs a signal the application already sends itself.
    ///
    /// A location counts whether or not its tab is the visible one: the question
    /// is about the drive, not about the window. Two panes on one drive is still
    /// one Open.
    void noteOpenLocations(const QList<VfsUri>& locations);

    /// Works out which drives have work running on them, from the tasks the
    /// manager is holding. Called when a task is added and when one starts or
    /// ends -- never on a timer.
    ///
    /// **Only tasks the user asked for count**, which is `Task::isBackground()`,
    /// and only the locations a task has declared through `Task::touching()`.
    /// Without the first, `QuerySpaceTask` -- which runs per mount every minute to
    /// keep the capacity bars honest -- would make every drive in the list pulse
    /// once a minute for ever, and the feature would be noise in its first hour.
    void refreshBusyDrives();

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
    /// Redraws the rows whose drive is in `mounts`. What both of the sets above
    /// need after they change.
    void announceStateOf(const QSet<QString>& mounts);
    /// Redraws one drive's row after what is known about it changed.
    void refreshRowFor(const QString& driveId);

    /// Mount ids the window currently has a location open on. Ids rather than
    /// uris: the question a row asks is whether *this* drive is in use, and the
    /// mount table is what turns a location into a drive.
    QSet<QString> m_openMounts;
    /// Mount ids with work the user asked for running on them. Derived when a
    /// task starts or ends rather than per row per repaint.
    QSet<QString> m_busyMounts;

    /// The last thing anything learned about reaching a drive.
    struct Reachability
    {
        /// A check is out and has not come back.
        bool pending = false;
        /// Meaningless while `pending` and before anything has asked.
        bool reachable = true;
        bool asked = false;
        QString message;
        QDateTime at;
    };

    void onSpaceReady(const QString& mountId, const SpaceInfo& info);
    int rowOfMount(const QString& mountId) const;

    VfsManager* m_vfs = nullptr;
    RemoteRegistry* m_remotes = nullptr;
    TaskManager* m_tasks = nullptr;
    QList<Row> m_rows;
    /// Keyed by mount id, so a drive keeps its figure across a reload of the
    /// table and the bar does not flicker away every time a mount is added.
    QHash<QString, SpaceInfo> m_space;
    /// Keyed by configured drive id, and kept across a reload of the mount
    /// table: what a check found does not stop being true because something
    /// else was mounted.
    QHash<QString, Reachability> m_reach;
    QTimer m_refreshTimer;
};

} // namespace mole
