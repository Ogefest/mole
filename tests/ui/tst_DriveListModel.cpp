#include "support/FakePlugin.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/models/DriveListModel.h"

#include "core/CoreMetaTypes.h"
#include "core/credentials/SecretStore.h"
#include "core/settings/Preferences.h"
#include "core/tasks/QuerySpaceTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/RemoteRegistry.h"
#include "core/vfs/SystemVolumes.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <atomic>

using namespace mole;
using namespace mole::test;

namespace {

/// A drive that claims a capacity, so the sidebar has something to draw
/// without depending on whatever the machine running the tests happens to
/// have mounted.
class SizedFileSystem : public IFileSystem
{
public:
    QString scheme() const override { return QStringLiteral("sized"); }

    VfsCapabilities capabilities() const override
    {
        return VfsCapability::Read | VfsCapability::ReportsSpace;
    }

    Result<FileEntryList> list(const VfsUri&, const CancelToken&) override { return FileEntryList {}; }

    Result<FileEntry> stat(const VfsUri& target) override
    {
        FileEntry entry;
        entry.uri = target;
        entry.name = target.fileName();
        entry.isDir = true;
        return entry;
    }

    Result<SpaceInfo> space(const VfsUri&) override
    {
        if (fail)
            return VfsError::make(VfsError::NotSupported, QStringLiteral("no idea"));
        SpaceInfo info;
        info.totalBytes = totalBytes;
        info.freeBytes = freeBytes;
        return info;
    }

    qint64 totalBytes = 1000;
    qint64 freeBytes = 250;
    bool fail = false;
};

/// A drive whose space() has stopped coming back.
///
/// What a mount whose server has gone actually does: `statvfs` blocks in the
/// kernel until its timeout, which for a hard NFS mount is about fifteen minutes
/// and can be for ever, and the cooperative token cannot reach it. Held rather
/// than slept for the same reason -- there is no duration that stands in for
/// "never". See MOLE-362.
class StalledFileSystem final : public SizedFileSystem
{
public:
    explicit StalledFileSystem(std::shared_ptr<QSemaphore> gate)
        : m_gate(std::move(gate))
    {
    }

    Result<SpaceInfo> space(const VfsUri& target) override
    {
        ++m_asked;
        m_gate->acquire();
        return SizedFileSystem::space(target);
    }

    /// How many times the drive has been asked how full it is.
    int timesAsked() const { return m_asked.load(); }

private:
    std::shared_ptr<QSemaphore> m_gate;
    std::atomic_int m_asked { 0 };
};

/// The state a row reports. The role carries a plain int -- the enum is not
/// registered with QML and the model hands out something a delegate can use --
/// so the cast lives here rather than at every assertion.
DriveListModel::State stateAt(const QModelIndex& index)
{
    return static_cast<DriveListModel::State>(index.data(DriveListModel::StateRole).toInt());
}

} // namespace

class TestDriveListModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void everyRowHasAGlyph();

    void obeysTheModelContract();
    void reportsCapacityWhenTheBackendKnowsIt();
    void saysNothingWhenTheBackendCannotAnswer();
    void saysNothingWhenTheBackendRefuses();
    void aFullDriveReadsAsFull();
    void unmountingDropsItsFigures();

    void aConfiguredDriveIsOneRowWhetherOrNotItIsConnected();
    void connectingDoesNotMoveTheRow();
    void disconnectingReturnsTheRowRatherThanRemovingIt();
    void aShutCredentialStoreShowsAsLocked();

    // ---- which drives the sidebar shows -----------------------------------
    void everyDriveIsShownUntilSomebodySaysOtherwise();
    void hidingOneTakesItOutOfTheSidebarAndNothingElse();
    void theChoiceIsRememberedAcrossARebuild();
    void aVolumeHiddenAtOnePathIsVisibleAgainAtAnother();
    void hidingEverythingLeavesNothingToShow();
    void aMountNobodyConfiguredIsIdleLikeAnythingNobodyIsUsing();

    void theMountTableIsReadOncePerRebuildAndNotOncePerRow();
    void aDriveIsConnectingUntilSomethingHasActuallyAsked();
    void aDriveThatCannotBeReachedKeepsItsMountAndSaysWhy();
    void aFailedOperationMovesAConnectedDriveToUnreachable();
    void aFailureOnALocalDiskIsNotADriveProblem();
    void nothingSchedulesARepeatingCheck();
    void aMissingFolderIsNotAnUnreachableDrive();

    void aDriveSomebodyIsOnIsOpenAndOneNobodyIsOnIsIdle();
    void aLocationOnADriveNobodyMountedOpensNothing();
    void unreachableOutranksOpenAndOpenOutranksTheConnectionStates();
    void everyStateHasAWordAColourAShapeAndAPulse();

    void aTaskOnTwoDrivesMakesBothOfThemBusy();
    void thePerMinuteSpaceQueryLightsNothing();
    void aTaskThatDeclaresNothingLightsNothing();
    void aTaskThatFailsOrIsCancelledStopsTheBusyDot();
    void busyOutranksOpenAndUnreachableOutranksBusy();
    void nothingRecomputesBusyOnATimer();

    // ---- what a drive that stopped answering costs -------------------------
    void onlyOneSpaceQueryIsEverOutPerMount();
    void addingAMountAsksNothingOfTheDisksAlreadyMeasured();

private:
    /// Adds a mount and waits for its space answer to arrive, if one is coming.
    void mountSized(const QString& id, std::shared_ptr<SizedFileSystem> fs);
    /// Puts a configured drive in the registry and returns its stored id.
    QString configure(const QString& name, bool withSecret = false);
    /// What connectDrive() does, minus the factory: the mount takes the drive's
    /// own id, which is the join the whole model rests on.
    void connectConfigured(const QString& driveId);
    /// A task that holds still on the pool until the test lets it go, declaring
    /// the locations it touches. Held still because "busy" is a state a task is
    /// *in*, and a task that has already finished is not in it.
    ScriptedTask* heldTask(const QList<VfsUri>& touching, bool background = false);
    /// The state of the row for a mount id.
    DriveListModel::State stateOfMount(const QString& id) const;

    /// Lets every held task go, so nothing outlives a test.
    std::shared_ptr<QSemaphore> m_gate;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<RemoteRegistry> m_registry;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<DriveListModel> m_model;
    std::unique_ptr<Preferences> m_preferences;
};

void TestDriveListModel::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_secrets
        = std::make_unique<SecretStore>(QDir(m_dir->path()).filePath(QStringLiteral("credentials.enc")));
    m_registry = std::make_unique<RemoteRegistry>(
        QDir(m_dir->path()).filePath(QStringLiteral("drives.json")), m_secrets.get());

    m_vfs = std::make_unique<VfsManager>();
    m_tasks = std::make_unique<TaskManager>();
    m_gate = std::make_shared<QSemaphore>();
    m_preferences = std::make_unique<Preferences>(QDir(m_dir->path()).filePath(QStringLiteral("prefs.json")));
    m_model
        = std::make_unique<DriveListModel>(m_vfs.get(), m_registry.get(), m_tasks.get(), m_preferences.get());
    // The periodic refresh would keep submitting work behind the assertions.
    m_model->setRefreshInterval(0);
}

ScriptedTask* TestDriveListModel::heldTask(const QList<VfsUri>& touching, bool background)
{
    auto gate = m_gate;
    auto* task = new ScriptedTask(QStringLiteral("held"), [gate](ScriptedTask&) { gate->acquire(); });
    task->noteTouching(touching);
    task->setBackground(background);
    m_tasks->submit(task);
    return task;
}

DriveListModel::State TestDriveListModel::stateOfMount(const QString& id) const
{
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex at = m_model->index(row, 0);
        if (at.data(DriveListModel::IdRole).toString() == id)
            return static_cast<DriveListModel::State>(at.data(DriveListModel::StateRole).toInt());
    }
    return DriveListModel::State::Idle;
}

void TestDriveListModel::cleanup()
{
    // Let every held task go before anything it might touch is destroyed. A
    // generous count: releasing permits nobody is waiting on costs nothing.
    if (m_gate)
        m_gate->release(64);
    // And then the pool, *before* the model -- releasing a task is not the same
    // as waiting for it, and this order is the whole of it. ~TaskManager cancels
    // and joins; destroying the model first left QuerySpaceTask emitting
    // spaceReady from a pool thread into a receiver being torn down on this one,
    // which ThreadSanitizer reported as a data race on the task's own connection
    // state in 4 runs of 20. AppController's shutdown says the same thing in as
    // many words: the task manager has to go first. See MOLE-273.
    m_tasks.reset();
    m_model.reset();
    m_vfs.reset();
    m_registry.reset();
    m_secrets.reset();
    m_dir.reset();
}

QString TestDriveListModel::configure(const QString& name, bool withSecret)
{
    RemoteDrive drive;
    drive.name = name;
    drive.factoryScheme = QStringLiteral("sftp");
    drive.variant = QStringLiteral("sftp");
    drive.root = QStringLiteral("/data");
    drive.settings.insert(QStringLiteral("host"), QStringLiteral("example.invalid"));
    if (withSecret)
        drive.secretFields.append(QStringLiteral("pass"));

    QString id;
    QString error;
    QVariantMap secretValues;
    if (withSecret)
        secretValues.insert(QStringLiteral("pass"), QStringLiteral("not-a-real-password"));
    if (!m_registry->put(drive, secretValues, &error, &id))
        return {};
    return id;
}

void TestDriveListModel::connectConfigured(const QString& driveId)
{
    const RemoteDrive drive = m_registry->drive(driveId);
    Mount mount;
    mount.id = drive.id;
    mount.displayName = drive.name;
    mount.root = drive.rootUri();
    mount.fileSystem = std::make_shared<MemoryFileSystem>();
    m_vfs->addMount(mount);
}

void TestDriveListModel::mountSized(const QString& id, std::shared_ptr<SizedFileSystem> fs)
{
    Mount mount;
    mount.id = id;
    mount.displayName = id;
    mount.root = VfsUri::fromString(QStringLiteral("sized://%1/").arg(id));
    mount.fileSystem = std::move(fs);
    m_vfs->addMount(mount);
}

void TestDriveListModel::obeysTheModelContract()
{
    QAbstractItemModelTester tester(m_model.get(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    mountSized(QStringLiteral("a"), std::make_shared<SizedFileSystem>());
    QCOMPARE(m_model->rowCount(), 1);
}

void TestDriveListModel::reportsCapacityWhenTheBackendKnowsIt()
{
    auto fs = std::make_shared<SizedFileSystem>();
    fs->totalBytes = 1000;
    fs->freeBytes = 250;
    mountSized(QStringLiteral("a"), fs);

    const QModelIndex index = m_model->index(0, 0);
    QVERIFY(waitFor([&] { return index.data(DriveListModel::HasSpaceRole).toBool(); }));

    // 750 of 1000 used.
    QCOMPARE(index.data(DriveListModel::UsedFractionRole).toDouble(), 0.75);
    QVERIFY(!index.data(DriveListModel::TotalTextRole).toString().isEmpty());
    QVERIFY(!index.data(DriveListModel::FreeTextRole).toString().isEmpty());
}

void TestDriveListModel::saysNothingWhenTheBackendCannotAnswer()
{
    // A plain memory drive has no capacity, and neither does a bucket. The
    // row must stay a name rather than gaining a bar showing an invented
    // number -- a chart is read as a fact.
    Mount mount;
    mount.id = QStringLiteral("scratch");
    mount.displayName = QStringLiteral("Scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem://scratch/"));
    mount.fileSystem = std::make_shared<MemoryFileSystem>();
    m_vfs->addMount(mount);

    const QModelIndex index = m_model->index(0, 0);
    QCOMPARE(index.data(DriveListModel::HasSpaceRole).toBool(), false);
    QCOMPARE(index.data(DriveListModel::TotalTextRole).toString(), QString());
    QCOMPARE(index.data(DriveListModel::UsedFractionRole).toDouble(), 0.0);

    // And nothing was submitted for it: a backend that does not advertise the
    // capability is not asked at all.
    m_model->refreshSpace();
    QCOMPARE(index.data(DriveListModel::HasSpaceRole).toBool(), false);
}

void TestDriveListModel::saysNothingWhenTheBackendRefuses()
{
    auto fs = std::make_shared<SizedFileSystem>();
    fs->fail = true; // advertises the capability, then cannot answer
    mountSized(QStringLiteral("a"), fs);

    QSignalSpy changed(m_model.get(), &QAbstractItemModel::dataChanged);
    m_model->refreshSpace();
    QVERIFY(waitFor([this] { return m_tasks->activeCount() == 0; }));

    const QModelIndex index = m_model->index(0, 0);
    QCOMPARE(index.data(DriveListModel::HasSpaceRole).toBool(), false);
}

void TestDriveListModel::aFullDriveReadsAsFull()
{
    auto fs = std::make_shared<SizedFileSystem>();
    fs->totalBytes = 500;
    fs->freeBytes = 0;
    mountSized(QStringLiteral("a"), fs);

    const QModelIndex index = m_model->index(0, 0);
    QVERIFY(waitFor([&] { return index.data(DriveListModel::HasSpaceRole).toBool(); }));
    QCOMPARE(index.data(DriveListModel::UsedFractionRole).toDouble(), 1.0);
}

void TestDriveListModel::unmountingDropsItsFigures()
{
    auto fs = std::make_shared<SizedFileSystem>();
    mountSized(QStringLiteral("a"), fs);

    QVERIFY(waitFor([this] { return m_model->index(0, 0).data(DriveListModel::HasSpaceRole).toBool(); }));

    m_vfs->removeMount(QStringLiteral("a"));
    QCOMPARE(m_model->rowCount(), 0);

    // Mounted again under the same id, it must not inherit the old bar before
    // its own answer arrives.
    auto replacement = std::make_shared<SizedFileSystem>();
    replacement->fail = true;
    mountSized(QStringLiteral("a"), replacement);

    QCOMPARE(m_model->index(0, 0).data(DriveListModel::HasSpaceRole).toBool(), false);
}

// ------------------------------------------------- drives, not just mounts

/// The point of the model. A configured drive that is not connected right now
/// used to appear nowhere, so the sidebar -- the one place that answers "what
/// can I get at" -- was silent about most of what somebody had set up.
void TestDriveListModel::aConfiguredDriveIsOneRowWhetherOrNotItIsConnected()
{
    const QString id = configure(QStringLiteral("Office NAS"));
    QVERIFY(!id.isEmpty());

    QCOMPARE(m_model->rowCount(), 1);
    const QModelIndex row = m_model->index(0, 0);
    QCOMPARE(row.data(DriveListModel::DisplayNameRole).toString(), QStringLiteral("Office NAS"));
    QCOMPARE(stateAt(row), DriveListModel::State::Disconnected);
    QCOMPARE(row.data(DriveListModel::ConfiguredIdRole).toString(), id);
    QVERIFY(row.data(DriveListModel::CanConnectRole).toBool());
    QVERIFY(!row.data(DriveListModel::CanEjectRole).toBool());

    // Not connected, so there is nothing to say about how full it is. A bar
    // drawn from no measurement would be read as one.
    QVERIFY(!row.data(DriveListModel::HasSpaceRole).toBool());
}

/// One row before and one row after, in the same place. Two rows would be the
/// obvious bug -- the mount and the configuration listed separately -- and a
/// row that moves when it connects is the subtler one: the list is read with a
/// pointer already on the way to it.
void TestDriveListModel::connectingDoesNotMoveTheRow()
{
    mountSized(QStringLiteral("disk"), std::make_shared<SizedFileSystem>());
    const QString first = configure(QStringLiteral("Alpha"));
    const QString second = configure(QStringLiteral("Beta"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());

    QCOMPARE(m_model->rowCount(), 3);
    const int rowOfBeta = 2;
    QCOMPARE(m_model->index(rowOfBeta, 0).data(DriveListModel::ConfiguredIdRole).toString(), second);

    // The second one connects -- the one that would jump to the top if
    // connected drives were listed among the mounts.
    connectConfigured(second);

    QCOMPARE(m_model->rowCount(), 3);
    const QModelIndex beta = m_model->index(rowOfBeta, 0);
    QCOMPARE(beta.data(DriveListModel::ConfiguredIdRole).toString(), second);
    QCOMPARE(stateAt(beta), DriveListModel::State::Idle);
    QVERIFY(beta.data(DriveListModel::CanEjectRole).toBool());
    QVERIFY(!beta.data(DriveListModel::CanConnectRole).toBool());

    // And the one that did not connect has not moved either.
    QCOMPARE(m_model->index(1, 0).data(DriveListModel::ConfiguredIdRole).toString(), first);
}

/// A drive that drops is still a drive somebody configured. Removing the row
/// would be the mount model's behaviour, and it is the behaviour this replaces.
void TestDriveListModel::disconnectingReturnsTheRowRatherThanRemovingIt()
{
    const QString id = configure(QStringLiteral("Office NAS"));
    connectConfigured(id);
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Idle);

    m_model->unmount(0);

    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Disconnected);
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::ConfiguredIdRole).toString(), id);
}

/// "Locked" rather than "not connected", because they need different answers
/// from the reader: one is a button, the other is a passphrase.
void TestDriveListModel::aShutCredentialStoreShowsAsLocked()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    QVERIFY(m_secrets->create(QStringLiteral("phrase")));
    const QString id = configure(QStringLiteral("Office NAS"), true);
    QVERIFY(!id.isEmpty());

    // Open store: it is merely not connected yet.
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Disconnected);

    // Shutting the store is not a change to any drive, but it changes what can
    // be said about one -- so the list has to hear about it rather than wait for
    // something else to happen.
    QSignalSpy relisted(m_model.get(), &QAbstractItemModel::modelReset);
    m_secrets->lock();
    QCOMPARE(relisted.count(), 1);

    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Locked);
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::StateTextRole).toString(), QStringLiteral("Locked"));
    QVERIFY2(!m_model->index(0, 0).data(DriveListModel::CanConnectRole).toBool(),
        "offering connect here would fail every time instead of saying what is wrong");

    QVERIFY(m_secrets->unlock(QStringLiteral("phrase")));
    QCOMPARE(relisted.count(), 2);
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Disconnected);
    QVERIFY(m_model->index(0, 0).data(DriveListModel::CanConnectRole).toBool());
}

/// The rows that were there before this change: a local disk, an open archive,
/// the scratch space. Nothing connects or ejects them in the drive sense, and
/// the sidebar must go on showing them exactly as it did.
/// A disk, an open archive and the scratch space used to be State::Local, which
/// said where a drive was rather than what was happening to it -- and shared the
/// sidebar's grey with a drive nobody had connected, where the grey means
/// something real. There is nothing to connect here, and that is now expressed by
/// the row never reaching the connection states rather than by a state of its own.
/// See MOLE-161.
void TestDriveListModel::aMountNobodyConfiguredIsIdleLikeAnythingNobodyIsUsing()
{
    auto fs = std::make_shared<SizedFileSystem>();
    mountSized(QStringLiteral("disk"), fs);

    const QModelIndex row = m_model->index(0, 0);
    QCOMPARE(stateAt(row), DriveListModel::State::Idle);
    QCOMPARE(row.data(DriveListModel::ConfiguredIdRole).toString(), QString());
    QVERIFY(!row.data(DriveListModel::CanConnectRole).toBool());
    QVERIFY(row.data(DriveListModel::CanEjectRole).toBool());
    QCOMPARE(row.data(DriveListModel::DisplayNameRole).toString(), QStringLiteral("disk"));

    // Capacity still arrives, and unmounting still takes the row away, because
    // there is no configuration behind it to leave in the list.
    QVERIFY(waitFor([&] { return row.data(DriveListModel::HasSpaceRole).toBool(); }));
    m_model->unmount(0);
    QCOMPARE(m_model->rowCount(), 0);
}

// ------------------------------------------- reachable, not merely mounted

/// connectDrive() returns as soon as the backend object is built, and building
/// one performs no I/O -- so a drive pointed at a host that has been switched
/// off is mounted exactly as successfully as one that works. A dot that means
/// "we have an object" while looking like it means "the server answered" is
/// worse than no dot, because it is believed.
/// The mount table, once.
///
/// keyFor() needs the device behind a detected volume, and it enumerated the
/// system's volumes to find it -- reached from isShown(), from data() for two
/// roles and from every rebuild, so once per row per read. Each enumeration was
/// a QStorageInfo per entry in the mount table, which is a statvfs apiece, so a
/// sidebar of eight rows on a machine with sixty mounts was hundreds of statvfs
/// calls on the thread that draws -- and one of them on a mount that has stopped
/// answering blocks for the kernel's timeout. See MOLE-361.
void TestDriveListModel::theMountTableIsReadOncePerRebuildAndNotOncePerRow()
{
    int asked = 0;
    // A machine of this test's own, so the count means something wherever this
    // runs -- and one of them a network mount, which is the entry that hangs.
    SystemVolumes::setEnumerator([&asked] {
        ++asked;
        QList<SystemVolume> volumes;
        SystemVolume root;
        root.name = QStringLiteral("Root");
        root.rootPath = QStringLiteral("/");
        root.device = QStringLiteral("/dev/sda2");
        root.isRoot = true;
        volumes.append(root);
        SystemVolume share;
        share.name = QStringLiteral("nas");
        share.rootPath = QStringLiteral("/mnt/nas");
        share.device = QStringLiteral("fileserver:/export");
        share.fileSystemType = QStringLiteral("nfs4");
        volumes.append(share);
        return volumes;
    });
    const auto restore = qScopeGuard([] { SystemVolumes::setEnumerator(nullptr); });

    // A rebuild -- which is what mounting something is -- and then every row
    // read the way a view reads it.
    Mount local;
    local.id = QStringLiteral("root");
    local.displayName = QStringLiteral("Root");
    local.root = VfsUri::fromLocalPath(QStringLiteral("/"));
    local.fileSystem = std::make_shared<LocalFileSystem>();
    m_vfs->addMount(local);
    QVERIFY(m_model->rowCount() > 0);
    asked = 0;

    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex at = m_model->index(row, 0);
        QVERIFY(!m_model->data(at, DriveListModel::VisibilityKeyRole).toString().isEmpty());
        m_model->data(at, DriveListModel::ShownRole);
        m_model->data(at, DriveListModel::DisplayNameRole);
    }
    QCOMPARE(m_model->shownCount(), m_model->rowCount());

    QVERIFY2(asked == 0,
        qPrintable(QStringLiteral("reading the rows asked the operating system %1 times").arg(asked)));

    // And a rebuild asks once, because a stick that has just been plugged in has
    // to be found somewhere.
    Mount second;
    second.id = QStringLiteral("scratch");
    second.displayName = QStringLiteral("Scratch");
    second.root = VfsUri::fromString(QStringLiteral("mem:///"));
    second.fileSystem = std::make_shared<MemoryFileSystem>();
    m_vfs->addMount(second);
    QCOMPARE(asked, 1);
}

void TestDriveListModel::aDriveIsConnectingUntilSomethingHasActuallyAsked()
{
    const QString id = configure(QStringLiteral("Office NAS"));
    QVERIFY(!id.isEmpty());

    // The order the application uses: the question goes out before the mount
    // goes in, so there is no moment at all when the row reads Connected.
    m_model->noteCheckStarted(id);
    connectConfigured(id);
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Connecting);

    m_model->noteCheckResult(id, true, QStringLiteral("Listed 4 entries"));
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Idle);
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::CheckMessageRole).toString(),
        QStringLiteral("Listed 4 entries"));
    QVERIFY2(!m_model->index(0, 0).data(DriveListModel::CheckedAtRole).toString().isEmpty(),
        "\"reachable\" with no when is not something anyone can act on");
}

/// The failing half, and the one the dot exists for. Every state the row passes
/// through is recorded, because "ends Unreachable" would also be true of a row
/// that flashed *available* on the way -- and a filled dot is what gets believed.
void TestDriveListModel::aDriveThatCannotBeReachedKeepsItsMountAndSaysWhy()
{
    const QString id = configure(QStringLiteral("Office NAS"));

    QList<DriveListModel::State> seen;
    const auto record = [this, &seen] { seen.append(stateAt(m_model->index(0, 0))); };
    connect(m_model.get(), &QAbstractItemModel::dataChanged, this, record);
    connect(m_model.get(), &QAbstractItemModel::modelReset, this, record);

    m_model->noteCheckStarted(id);
    connectConfigured(id);
    m_model->noteCheckResult(id, false, QStringLiteral("No route to the server"));

    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Unreachable);
    QVERIFY2(!seen.contains(DriveListModel::State::Idle),
        "a drive that cannot be reached must never have shown as available");
    QVERIFY(seen.contains(DriveListModel::State::Connecting));

    QCOMPARE(m_model->index(0, 0).data(DriveListModel::CheckMessageRole).toString(),
        QStringLiteral("No route to the server"));
    QCOMPARE(
        m_model->index(0, 0).data(DriveListModel::StateSeverityRole).toString(), QStringLiteral("broken"));

    // Still mounted, and still ejectable. The backend is there and a later
    // operation may well work; what failed was a question, and the row says so
    // rather than pretending the drive has gone.
    QVERIFY2(m_model->index(0, 0).data(DriveListModel::CanEjectRole).toBool(),
        "an unreachable drive keeps its mount and can still be browsed");
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::CanConnectRole).toBool(), false);
}

/// A check is not the only thing that finds out. A listing that came back with
/// an error is the plainest evidence there is, and it happens in a tab that has
/// never heard of the sidebar.
void TestDriveListModel::aFailedOperationMovesAConnectedDriveToUnreachable()
{
    const QString id = configure(QStringLiteral("Office NAS"));
    m_model->noteCheckStarted(id);
    connectConfigured(id);
    m_model->noteCheckResult(id, true, QStringLiteral("Listed 4 entries"));
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Idle);

    const VfsUri somewhereOnIt = m_registry->drive(id).rootUri().child(QStringLiteral("reports"));
    m_model->noteFailureAt(somewhereOnIt,
        VfsError::make(VfsError::NetworkError, QStringLiteral("The server stopped answering")));

    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Unreachable);
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::CheckMessageRole).toString(),
        QStringLiteral("The server stopped answering"));
}

/// A local disk that refused a listing has a permission problem, not a
/// reachability one, and calling it unreachable would send the reader looking
/// at their network.
void TestDriveListModel::aFailureOnALocalDiskIsNotADriveProblem()
{
    mountSized(QStringLiteral("disk"), std::make_shared<SizedFileSystem>());
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Idle);

    m_model->noteFailureAt(VfsUri::fromString(QStringLiteral("sized://disk/somewhere")),
        VfsError::make(VfsError::NetworkError, QStringLiteral("The server stopped answering")));

    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Idle);
}

/// The capacity refresh runs against every mount on a timer, and is the obvious
/// thing to reuse for liveness -- and the wrong one. It deliberately says
/// nothing when a backend cannot answer, because unknown capacity is normal for
/// a bucket, so its silence cannot be read as unreachable. Nor does anything
/// else here poll: state changes when something is actually learned.
void TestDriveListModel::nothingSchedulesARepeatingCheck()
{
    const QString id = configure(QStringLiteral("Office NAS"));
    m_model->noteCheckStarted(id);
    connectConfigured(id);
    m_model->noteCheckResult(id, false, QStringLiteral("No route to the server"));

    for (int round = 0; round < 5; ++round) {
        m_model->refreshSpace();
        QVERIFY(waitFor([this] { return m_tasks->activeCount() == 0; }));
    }

    // Unchanged. Nothing asked again, so nothing has been learned, so the row
    // still shows what was last true.
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Unreachable);
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::CheckMessageRole).toString(),
        QStringLiteral("No route to the server"));
}

/// Not every failure is about the drive. Browsing produces NotFound and
/// AccessDenied constantly -- a typed path, a folder somebody else deleted, a
/// directory the account cannot read -- and none of them says the server has
/// gone. Marking the drive unreachable for those would send somebody to look at
/// their network because they mistyped a name, which is worse than saying
/// nothing.
void TestDriveListModel::aMissingFolderIsNotAnUnreachableDrive()
{
    const QString id = configure(QStringLiteral("Office NAS"));
    m_model->noteCheckStarted(id);
    connectConfigured(id);
    m_model->noteCheckResult(id, true, QStringLiteral("Listed 4 entries"));

    const VfsUri onIt = m_registry->drive(id).rootUri().child(QStringLiteral("typo"));
    for (VfsError::Code code : { VfsError::NotFound, VfsError::AccessDenied, VfsError::NotSupported,
             VfsError::Cancelled, VfsError::NotADirectory }) {
        m_model->noteFailureAt(onIt, VfsError::make(code, QStringLiteral("no")));
        QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Idle);
    }

    // What does count: the drive itself stopped answering.
    m_model->noteFailureAt(onIt, VfsError::make(VfsError::NetworkError, QStringLiteral("gone")));
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Unreachable);
}

// ---- what a drive is doing ------------------------------------------------
//
// The dot beside a drive used to mean two different things depending on which row
// it was on, and on a local disk it meant nothing at all: a mount nobody had
// configured was State::Local, which shares the sidebar's grey with a drive nobody
// has connected -- where the grey means *configured, not connected, and could be*.
// A local disk is not doing anything and is not waiting to be connected either.
// See MOLE-161.

void TestDriveListModel::aDriveSomebodyIsOnIsOpenAndOneNobodyIsOnIsIdle()
{
    mountSized(QStringLiteral("disk"), std::make_shared<SizedFileSystem>());
    const QModelIndex row = m_model->index(0, 0);
    QCOMPARE(stateAt(row), DriveListModel::State::Idle);

    // Told rather than found out, and told in terms of *locations*: what the
    // window has open is a uri, and the mount table is what turns it into a drive.
    m_model->noteOpenLocations({ VfsUri::fromString(QStringLiteral("sized://disk/photos")) });
    QCOMPARE(stateAt(row), DriveListModel::State::Open);
    QCOMPARE(row.data(DriveListModel::StateTextRole).toString(), QStringLiteral("Open"));
    QCOMPARE(row.data(DriveListModel::StateSeverityRole).toString(), QStringLiteral("using"));

    // And leaving turns it off.
    m_model->noteOpenLocations({});
    QCOMPARE(stateAt(row), DriveListModel::State::Idle);

    // Two locations on one drive is still one Open, and going from one of them to
    // the other leaves the row where it was.
    m_model->noteOpenLocations({ VfsUri::fromString(QStringLiteral("sized://disk/a")),
        VfsUri::fromString(QStringLiteral("sized://disk/b")) });
    QCOMPARE(stateAt(row), DriveListModel::State::Open);
}

void TestDriveListModel::aLocationOnADriveNobodyMountedOpensNothing()
{
    // A configured drive that is not connected cannot have anything open on it,
    // and a uri on a drive that does not exist must not disturb anybody's row.
    const QString id = configure(QStringLiteral("Office NAS"));
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Disconnected);

    m_model->noteOpenLocations({ m_registry->drive(id).rootUri().child(QStringLiteral("reports")),
        VfsUri::fromString(QStringLiteral("sized://nothing/here")) });
    QCOMPARE(stateAt(m_model->index(0, 0)), DriveListModel::State::Disconnected);
}

void TestDriveListModel::unreachableOutranksOpenAndOpenOutranksTheConnectionStates()
{
    // Highest first: Unreachable → Busy → Open → Connecting → Not connected →
    // Idle. Busy arrives with MOLE-162; the pairs that can occur without it are
    // asserted here.
    const QString id = configure(QStringLiteral("Office NAS"));
    m_model->noteCheckStarted(id);
    connectConfigured(id);
    const QModelIndex row = m_model->index(0, 0);
    QCOMPARE(stateAt(row), DriveListModel::State::Connecting);

    // Open beats Connecting: somebody is already looking at it, which is a more
    // specific statement than "we are still asking whether it is there".
    m_model->noteOpenLocations({ m_registry->drive(id).rootUri() });
    QCOMPARE(stateAt(row), DriveListModel::State::Open);

    // And Unreachable beats Open. A drive nothing can reach reads unreachable
    // whatever is being attempted on it -- the opposite would show a reader the
    // accent colour for a drive that has stopped answering.
    m_model->noteCheckResult(id, false, QStringLiteral("No route to the server"));
    QCOMPARE(stateAt(row), DriveListModel::State::Unreachable);

    // Reachable again, and it goes back to being open rather than to idle.
    m_model->noteCheckResult(id, true, QStringLiteral("Listed 4 entries"));
    QCOMPARE(stateAt(row), DriveListModel::State::Open);
}

void TestDriveListModel::everyStateHasAWordAColourAShapeAndAPulse()
{
    // Six states will not fit in one colour, so the encoding is spread over
    // channels that each carry one idea: hue is the kind, filled against hollow is
    // *here* against *not here yet*, and motion is *happening right now*. The
    // table is the decision, so it is asserted as a table.
    struct Appearance
    {
        DriveListModel::State state;
        const char* word;
        const char* severity;
        bool filled;
        const char* motion;
    };
    static const Appearance table[] = {
        { DriveListModel::State::Unreachable, "Unreachable", "broken", true, "" },
        { DriveListModel::State::Disconnected, "Not connected", "idle", false, "" },
        { DriveListModel::State::Locked, "Locked", "idle", false, "" },
        { DriveListModel::State::Connecting, "Connecting", "idle", false, "waiting" },
        { DriveListModel::State::Idle, "Idle", "idle", true, "" },
        { DriveListModel::State::Open, "Open", "using", true, "" },
        // Work going through, and it neither looks nor moves like being *on* a
        // drive: green, and breathing where `Open` holds still. The word is what
        // the motion means, not what it looks like, which is why it reads the same
        // as the severity here and differs from it for `Connecting`. See both
        // 2026-08-19 revisions in ADR-0052.
        { DriveListModel::State::Busy, "Busy", "working", true, "working" },
    };

    for (const Appearance& row : table) {
        QCOMPARE(DriveListModel::stateText(row.state), QString::fromLatin1(row.word));
        QCOMPARE(DriveListModel::stateSeverity(row.state), QString::fromLatin1(row.severity));
        QCOMPARE(DriveListModel::stateFillsTheDot(row.state), row.filled);
        QCOMPARE(DriveListModel::stateMotion(row.state), QString::fromLatin1(row.motion));
    }

    // The two moving states move differently, which is the whole reason motion is
    // a word and not a flag: waiting for an answer and work going through are not
    // the same thing to look at. They breathe with the same shape now, so the word
    // is the only thing keeping the view from drawing them alike.
    QVERIFY(DriveListModel::stateMotion(DriveListModel::State::Connecting)
        != DriveListModel::stateMotion(DriveListModel::State::Busy));
    // And busy no longer borrows the colour that means "you are on this".
    QVERIFY(DriveListModel::stateSeverity(DriveListModel::State::Busy)
        != DriveListModel::stateSeverity(DriveListModel::State::Open));

    // The two greys are told apart by shape, which is the pair the old one
    // conflated: Idle is filled and Not connected is a ring.
    QCOMPARE(DriveListModel::stateSeverity(DriveListModel::State::Idle),
        DriveListModel::stateSeverity(DriveListModel::State::Disconnected));
    QVERIFY(DriveListModel::stateFillsTheDot(DriveListModel::State::Idle));
    QVERIFY(!DriveListModel::stateFillsTheDot(DriveListModel::State::Disconnected));

    // And no state wears green any more.
    for (const Appearance& row : table)
        QVERIFY(DriveListModel::stateSeverity(row.state) != QStringLiteral("good"));
}

// ---- and what is running on it --------------------------------------------
//
// A drive with a copy running on it looked exactly like one nobody had touched all
// session. "Which of my drives is this transfer actually touching" is a question
// people ask out loud, and the task strip answered it only by naming a task whose
// title contains a path somebody has to read. See MOLE-162.

void TestDriveListModel::aTaskOnTwoDrivesMakesBothOfThemBusy()
{
    mountSized(QStringLiteral("from"), std::make_shared<SizedFileSystem>());
    mountSized(QStringLiteral("to"), std::make_shared<SizedFileSystem>());
    QCOMPARE(stateOfMount(QStringLiteral("from")), DriveListModel::State::Idle);
    QCOMPARE(stateOfMount(QStringLiteral("to")), DriveListModel::State::Idle);

    // A copy has a source and a destination and usually two different drives, so
    // the attribution is from a task's uris to a *set* of drives rather than to
    // one. TransferTask declares both ends for exactly this.
    ScriptedTask* task = heldTask({ VfsUri::fromString(QStringLiteral("sized://from/holiday.mov")),
        VfsUri::fromString(QStringLiteral("sized://to/backup")) });
    QVERIFY(waitFor([this] {
        return stateOfMount(QStringLiteral("from")) == DriveListModel::State::Busy
            && stateOfMount(QStringLiteral("to")) == DriveListModel::State::Busy;
    }));
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::StateTextRole).toString(), QStringLiteral("Busy"));
    QCOMPARE(
        m_model->index(0, 0).data(DriveListModel::StateSeverityRole).toString(), QStringLiteral("working"));
    QVERIFY(m_model->index(0, 0).data(DriveListModel::DotFilledRole).toBool());
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::DotMotionRole).toString(), QStringLiteral("working"));

    // And neither when it finishes.
    m_gate->release();
    QVERIFY(waitForTask(task));
    QVERIFY(waitFor([this] {
        return stateOfMount(QStringLiteral("from")) == DriveListModel::State::Idle
            && stateOfMount(QStringLiteral("to")) == DriveListModel::State::Idle;
    }));
}

void TestDriveListModel::thePerMinuteSpaceQueryLightsNothing()
{
    // The trap this feature would have shipped with. QuerySpaceTask runs per mount
    // every minute -- it is how the sidebar knows how full a drive is -- so if
    // every task lit the dot, every drive in the list would pulse once a minute
    // for ever and the feature would be noise in its first hour. Task::isBackground()
    // is the hook, and QuerySpaceTask sets it.
    mountSized(QStringLiteral("disk"), std::make_shared<SizedFileSystem>());

    auto* housekeeping = new QuerySpaceTask(
        m_vfs->mounts().first().fileSystem, m_vfs->mounts().first().root, QStringLiteral("disk"));
    QVERIFY2(housekeeping->isBackground(), "QuerySpaceTask must stay background, or the dot becomes noise");
    m_tasks->submit(housekeeping);
    QVERIFY(waitForTask(housekeeping));

    QCOMPARE(stateOfMount(QStringLiteral("disk")), DriveListModel::State::Idle);

    // And the same rule while one is running rather than after it: a background
    // task held still lights nothing either.
    ScriptedTask* held = heldTask({ VfsUri::fromString(QStringLiteral("sized://disk/x")) }, true);
    QVERIFY(waitFor([held] { return held->state() == Task::State::Running; }));
    QCOMPARE(stateOfMount(QStringLiteral("disk")), DriveListModel::State::Idle);
    m_gate->release();
    QVERIFY(waitForTask(held));
}

void TestDriveListModel::aTaskThatDeclaresNothingLightsNothing()
{
    // The second guard, and the one that covers what nobody has thought about
    // yet: a metadata read, a thumbnail decode, a table row count. Silence lights
    // nothing, so a task only appears in the sidebar when somebody decided it
    // should.
    mountSized(QStringLiteral("disk"), std::make_shared<SizedFileSystem>());

    ScriptedTask* silent = heldTask({});
    QVERIFY(waitFor([silent] { return silent->state() == Task::State::Running; }));
    QVERIFY2(!silent->isBackground(), "this one is not background: it is simply not saying");
    QVERIFY(silent->touching().isEmpty());
    QCOMPARE(stateOfMount(QStringLiteral("disk")), DriveListModel::State::Idle);
    m_gate->release();
    QVERIFY(waitForTask(silent));
}

void TestDriveListModel::aTaskThatFailsOrIsCancelledStopsTheBusyDot()
{
    // A drive left pulsing after a failed copy is the kind of thing nobody
    // notices in review and everybody notices in use.
    mountSized(QStringLiteral("disk"), std::make_shared<SizedFileSystem>());
    const QList<VfsUri> on { VfsUri::fromString(QStringLiteral("sized://disk/report.pdf")) };

    ScriptedTask* failing = heldTask(on);
    QVERIFY(waitFor([this] { return stateOfMount(QStringLiteral("disk")) == DriveListModel::State::Busy; }));
    failing->fail(VfsError::make(VfsError::IoError, QStringLiteral("the disk gave up")));
    m_gate->release();
    QVERIFY(waitForTask(failing));
    QCOMPARE(failing->state(), Task::State::Failed);
    QVERIFY(waitFor([this] { return stateOfMount(QStringLiteral("disk")) == DriveListModel::State::Idle; }));

    ScriptedTask* cancelled = heldTask(on);
    QVERIFY(waitFor([this] { return stateOfMount(QStringLiteral("disk")) == DriveListModel::State::Busy; }));
    cancelled->requestCancel();
    m_gate->release();
    QVERIFY(waitForTask(cancelled));
    QVERIFY(waitFor([this] { return stateOfMount(QStringLiteral("disk")) == DriveListModel::State::Idle; }));
}

void TestDriveListModel::busyOutranksOpenAndUnreachableOutranksBusy()
{
    const QString id = configure(QStringLiteral("Office NAS"));
    m_model->noteCheckStarted(id);
    connectConfigured(id);
    m_model->noteCheckResult(id, true, QStringLiteral("Listed 4 entries"));
    const VfsUri root = m_registry->drive(id).rootUri();

    // Open, then busy on top of it. Busy is the more specific statement.
    m_model->noteOpenLocations({ root });
    QCOMPARE(stateOfMount(id), DriveListModel::State::Open);

    ScriptedTask* task = heldTask({ root.child(QStringLiteral("archive")) });
    QVERIFY(waitFor([this, &id] { return stateOfMount(id) == DriveListModel::State::Busy; }));

    // And a drive nothing can reach reads unreachable even with work running on
    // it: the work is what found out.
    m_model->noteCheckResult(id, false, QStringLiteral("The server stopped answering"));
    QCOMPARE(stateOfMount(id), DriveListModel::State::Unreachable);
    m_model->noteCheckResult(id, true, QStringLiteral("Answering again"));
    QCOMPARE(stateOfMount(id), DriveListModel::State::Busy);

    // When the task ends it goes back to open, not to idle: somebody is still
    // looking at it.
    m_gate->release();
    QVERIFY(waitForTask(task));
    QVERIFY(waitFor([this, &id] { return stateOfMount(id) == DriveListModel::State::Open; }));
}

void TestDriveListModel::nothingRecomputesBusyOnATimer()
{
    // Learnt from what the application already tells itself -- a task appended, a
    // task changing state -- and from nothing else. A repeating recomputation
    // would be work done for as long as the window is open, for an answer that
    // only changes when a task does.
    mountSized(QStringLiteral("disk"), std::make_shared<SizedFileSystem>());
    m_model->setRefreshInterval(0);

    const QList<QTimer*> timers = m_model->findChildren<QTimer*>();
    for (QTimer* timer : timers) {
        QVERIFY2(
            !timer->isActive(), "the only timer here is the capacity refresh, and this test turned it off");
    }
}

// ---- which drives the sidebar shows -----------------------------------------
//
// Nothing said which ones it shows: Home, then every volume discovery admitted,
// then every configured drive, with no flag anywhere saying *not that one*. See
// MOLE-311.

/// A drive nobody has said anything about is shown.
///
/// Ticking is how you take one out, not how you let one in -- a USB stick plugged
/// in has to appear, and a stick that appeared in the machine and nowhere in Mole
/// until somebody opened a dialog would be a worse fault than the clutter.
void TestDriveListModel::everyDriveIsShownUntilSomebodySaysOtherwise()
{
    mountSized(QStringLiteral("one"), std::make_shared<SizedFileSystem>());
    mountSized(QStringLiteral("two"), std::make_shared<SizedFileSystem>());

    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(m_model->shownCount(), 2);
    for (int row = 0; row < m_model->rowCount(); ++row) {
        QVERIFY2(m_model->index(row, 0).data(DriveListModel::ShownRole).toBool(),
            qPrintable(m_model->index(row, 0).data(DriveListModel::DisplayNameRole).toString()));
        QVERIFY(!m_model->visibilityKeyAt(row).isEmpty());
    }
}

void TestDriveListModel::hidingOneTakesItOutOfTheSidebarAndNothingElse()
{
    const QString driveId = configure(QStringLiteral("archive box"));
    connectConfigured(driveId);
    mountSized(QStringLiteral("local"), std::make_shared<SizedFileSystem>());
    QCOMPARE(m_model->rowCount(), 2);

    // The configured one, found by its id rather than by a row number.
    int row = -1;
    for (int i = 0; i < m_model->rowCount(); ++i) {
        if (m_model->configuredIdAt(i) == driveId)
            row = i;
    }
    QVERIFY(row >= 0);

    QSignalSpy changed(m_model.get(), &QAbstractItemModel::dataChanged);
    m_model->setShown(row, false);

    QVERIFY2(!m_model->index(row, 0).data(DriveListModel::ShownRole).toBool(),
        "the drive was still marked as shown");
    QCOMPARE(m_model->shownCount(), 1);
    QCOMPARE(changed.count(), 1);

    // **And nothing else happened.** The row is still there, the drive is still
    // configured, and it is still mounted and reachable by its uri: hiding is
    // about the sidebar and about nothing else.
    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(m_model->configuredIdAt(row), driveId);
    QVERIFY(m_registry->drive(driveId).isValid());
    const QString rootUri = m_model->index(row, 0).data(DriveListModel::RootUriRole).toString();
    QVERIFY(!rootUri.isEmpty());
    QVERIFY2(
        m_vfs->resolve(VfsUri::fromString(rootUri)) != nullptr, "hiding a drive stopped it being reachable");

    // Ticking it again puts it back.
    m_model->setShown(row, true);
    QVERIFY(m_model->index(row, 0).data(DriveListModel::ShownRole).toBool());
    QCOMPARE(m_model->shownCount(), 2);
}

void TestDriveListModel::theChoiceIsRememberedAcrossARebuild()
{
    // A preference rather than session state, which is what makes it survive a
    // restart -- asserted by building the model again over the same store rather
    // than by restarting anything.
    mountSized(QStringLiteral("keep"), std::make_shared<SizedFileSystem>());
    mountSized(QStringLiteral("hide"), std::make_shared<SizedFileSystem>());

    int hidden = -1;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->index(row, 0).data(DriveListModel::DisplayNameRole).toString()
            == QStringLiteral("hide")) {
            hidden = row;
        }
    }
    QVERIFY(hidden >= 0);
    const QString key = m_model->visibilityKeyAt(hidden);
    m_model->setShown(hidden, false);
    QVERIFY(m_preferences->value(DriveListModel::hiddenPreferenceKey()).toStringList().contains(key));

    DriveListModel rebuilt(m_vfs.get(), m_registry.get(), m_tasks.get(), m_preferences.get());
    rebuilt.setRefreshInterval(0);
    QCOMPARE(rebuilt.rowCount(), 2);
    QCOMPARE(rebuilt.shownCount(), 1);
    for (int row = 0; row < rebuilt.rowCount(); ++row) {
        const bool isTheHiddenOne = rebuilt.visibilityKeyAt(row) == key;
        QCOMPARE(rebuilt.index(row, 0).data(DriveListModel::ShownRole).toBool(), !isTheHiddenOne);
    }
}

void TestDriveListModel::aVolumeHiddenAtOnePathIsVisibleAgainAtAnother()
{
    // The documented direction of the error. A detected volume has no id that
    // outlives the run, so the choice is keyed on the device where there is one
    // and the root path where there is not -- and a removable disk hidden at one
    // path and mounted next time at another comes back. A drive that reappears is
    // a nuisance; a drive that silently does not appear is somebody's files
    // missing with no way to find out why.
    Mount stick;
    stick.id = QStringLiteral("stick");
    stick.displayName = QStringLiteral("STICK");
    stick.root = VfsUri::fromLocalPath(QDir(m_dir->path()).filePath(QStringLiteral("media/a/STICK")));
    stick.fileSystem = std::make_shared<MemoryFileSystem>();
    m_vfs->addMount(stick);
    QCOMPARE(m_model->rowCount(), 1);
    m_model->setShown(0, false);
    QCOMPARE(m_model->shownCount(), 0);

    // Gone, and back somewhere else -- which is what a stick pulled out and put
    // in again looks like to the machine.
    m_vfs->removeMount(QStringLiteral("stick"));
    Mount again = stick;
    again.root = VfsUri::fromLocalPath(QDir(m_dir->path()).filePath(QStringLiteral("media/b/STICK")));
    m_vfs->addMount(again);

    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY2(m_model->index(0, 0).data(DriveListModel::ShownRole).toBool(),
        "a volume that came back at a different path stayed hidden, which is the wrong "
        "direction for this error");
}

void TestDriveListModel::hidingEverythingLeavesNothingToShow()
{
    // A sidebar with every drive taken out has to say so rather than draw an empty
    // strip, and this is the fact it says it from.
    mountSized(QStringLiteral("one"), std::make_shared<SizedFileSystem>());
    mountSized(QStringLiteral("two"), std::make_shared<SizedFileSystem>());
    for (int row = 0; row < m_model->rowCount(); ++row)
        m_model->setShown(row, false);

    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(m_model->shownCount(), 0);
}

// ----------------------- what a drive that stopped answering costs

void TestDriveListModel::onlyOneSpaceQueryIsEverOutPerMount()
{
    // refreshSpace() submitted a QuerySpaceTask per mounted drive every time it
    // was called, with no guard against the previous answer still being out. On a
    // mount that has stopped answering each of those is a pool thread waiting on
    // the same statvfs, and the minute timer adds one more for ever -- which is
    // how a single dead drive in the sidebar took the whole pool in under ten
    // minutes with nobody touching it.
    auto gate = std::make_shared<QSemaphore>();
    auto stalled = std::make_shared<StalledFileSystem>(gate);

    Mount mount;
    mount.id = QStringLiteral("dead");
    mount.displayName = QStringLiteral("The NAS");
    mount.root = VfsUri::fromString(QStringLiteral("sized://dead/"));
    mount.fileSystem = stalled;
    m_vfs->addMount(mount);

    QVERIFY(waitFor([&stalled] { return stalled->timesAsked() == 1; }));

    m_model->refreshSpace();
    m_model->refreshSpace();
    m_model->refreshSpace();
    drainEvents();

    // Still one. The three refreshes found the first question still out.
    QCOMPARE(stalled->timesAsked(), 1);

    // And once it comes back, asking again is allowed: a mount that answered
    // slowly once must not be struck off for ever.
    gate->release(8);
    QVERIFY(waitFor([this] { return m_model->index(0, 0).data(DriveListModel::HasSpaceRole).toBool(); }));
    m_model->refreshSpace();
    QVERIFY(waitFor([&stalled] { return stalled->timesAsked() == 2; }));
}

void TestDriveListModel::addingAMountAsksNothingOfTheDisksAlreadyMeasured()
{
    // Every change to the mount table reloads the list, and reload() asked every
    // drive again. Browsing into a .zip adds an unlisted mount and pruning it
    // removes one, so one gesture fired two rounds of queries at every disk in
    // the sidebar -- including the one that never answers.
    auto measured = std::make_shared<SizedFileSystem>();
    mountSized(QStringLiteral("disk"), measured);

    auto gate = std::make_shared<QSemaphore>();
    auto counted = std::make_shared<StalledFileSystem>(gate);
    gate->release(64); // this one answers; the count is what is being watched

    Mount second;
    second.id = QStringLiteral("second");
    second.displayName = QStringLiteral("Second");
    second.root = VfsUri::fromString(QStringLiteral("sized://second/"));
    second.fileSystem = counted;
    m_vfs->addMount(second);
    QVERIFY(waitFor([&counted] { return counted->timesAsked() == 1; }));

    // A third mount arrives -- what opening an archive does.
    auto third = std::make_shared<SizedFileSystem>();
    Mount archive;
    archive.id = QStringLiteral("third");
    archive.displayName = QStringLiteral("Third");
    archive.root = VfsUri::fromString(QStringLiteral("sized://third/"));
    archive.fileSystem = third;
    m_vfs->addMount(archive);
    QVERIFY(waitFor([this] { return m_model->rowCount() >= 3; }));
    drainEvents();

    // The drive that was already measured was not asked a second time.
    QCOMPARE(counted->timesAsked(), 1);

    // The minute timer is the thing that re-asks, and it asks everything.
    m_model->refreshSpace();
    QVERIFY(waitFor([&counted] { return counted->timesAsked() == 2; }));
}

void TestDriveListModel::everyRowHasAGlyph()
{
    // **The role was empty on every row.** IconTextRole returned
    // `row.mount.iconName`, which only the two overloads of
    // VfsManager::addMount() that build a backend themselves ever set -- and the
    // one that does is used for archive re-mounts, which are `unlisted` and
    // filtered out of this model. So a connected drive, a local disk and a
    // configured drive all answered nothing, and the command palette's drive rows
    // had no glyph while IFileSystemFactory::iconName() sat there to supply one.
    // See MOLE-395.
    // A factory for the scheme, because a glyph has to come from somewhere: the
    // one the factory offers is what every row should end up with, and the point
    // of the fault is that nothing was asking it. IFileSystemFactory::iconName()
    // has a default, so this is what the real application has for every drive
    // kind it can serve.
    m_vfs->registerFactory(std::make_unique<FakeFileSystemFactory>(QStringLiteral("sftp")));

    const QString driveId = configure(QStringLiteral("Fileserver"), false);
    QVERIFY(!driveId.isEmpty());
    connectConfigured(driveId);

    QVERIFY(waitFor([this] { return m_model->rowCount() > 0; }));
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QString glyph = m_model->data(m_model->index(row, 0), DriveListModel::IconTextRole).toString();
        const QString name
            = m_model->data(m_model->index(row, 0), DriveListModel::DisplayNameRole).toString();
        QVERIFY2(!glyph.isEmpty(), qPrintable(QStringLiteral("the row for %1 has no glyph").arg(name)));
    }
}

MOLE_TEST_MAIN(TestDriveListModel)
#include "tst_DriveListModel.moc"
