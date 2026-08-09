#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/models/DriveListModel.h"

#include "core/CoreMetaTypes.h"
#include "core/credentials/SecretStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/RemoteRegistry.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

namespace {

/// A drive that claims a capacity, so the sidebar has something to draw
/// without depending on whatever the machine running the tests happens to
/// have mounted.
class SizedFileSystem final : public IFileSystem
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

} // namespace

class TestDriveListModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

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
    void aMountNobodyConfiguredIsLocalAndUnchanged();

private:
    /// Adds a mount and waits for its space answer to arrive, if one is coming.
    void mountSized(const QString& id, std::shared_ptr<SizedFileSystem> fs);
    /// Puts a configured drive in the registry and returns its stored id.
    QString configure(const QString& name, bool withSecret = false);
    /// What connectDrive() does, minus the factory: the mount takes the drive's
    /// own id, which is the join the whole model rests on.
    void connectConfigured(const QString& driveId);

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<RemoteRegistry> m_registry;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<DriveListModel> m_model;
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
    m_model = std::make_unique<DriveListModel>(m_vfs.get(), m_registry.get(), m_tasks.get());
    // The periodic refresh would keep submitting work behind the assertions.
    m_model->setRefreshInterval(0);
}

void TestDriveListModel::cleanup()
{
    m_model.reset();
    m_tasks.reset();
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
    QCOMPARE(row.data(DriveListModel::StateRole).value<DriveListModel::State>(),
        DriveListModel::State::Disconnected);
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
    QCOMPARE(beta.data(DriveListModel::StateRole).value<DriveListModel::State>(),
        DriveListModel::State::Connected);
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
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::StateRole).value<DriveListModel::State>(),
        DriveListModel::State::Connected);

    m_model->unmount(0);

    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::StateRole).value<DriveListModel::State>(),
        DriveListModel::State::Disconnected);
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
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::StateRole).value<DriveListModel::State>(),
        DriveListModel::State::Disconnected);

    // Shutting the store is not a change to any drive, but it changes what can
    // be said about one -- so the list has to hear about it rather than wait for
    // something else to happen.
    QSignalSpy relisted(m_model.get(), &QAbstractItemModel::modelReset);
    m_secrets->lock();
    QCOMPARE(relisted.count(), 1);

    QCOMPARE(m_model->index(0, 0).data(DriveListModel::StateRole).value<DriveListModel::State>(),
        DriveListModel::State::Locked);
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::StateTextRole).toString(), QStringLiteral("Locked"));
    QVERIFY2(!m_model->index(0, 0).data(DriveListModel::CanConnectRole).toBool(),
        "offering connect here would fail every time instead of saying what is wrong");

    QVERIFY(m_secrets->unlock(QStringLiteral("phrase")));
    QCOMPARE(relisted.count(), 2);
    QCOMPARE(m_model->index(0, 0).data(DriveListModel::StateRole).value<DriveListModel::State>(),
        DriveListModel::State::Disconnected);
    QVERIFY(m_model->index(0, 0).data(DriveListModel::CanConnectRole).toBool());
}

/// The rows that were there before this change: a local disk, an open archive,
/// the scratch space. Nothing connects or ejects them in the drive sense, and
/// the sidebar must go on showing them exactly as it did.
void TestDriveListModel::aMountNobodyConfiguredIsLocalAndUnchanged()
{
    auto fs = std::make_shared<SizedFileSystem>();
    mountSized(QStringLiteral("disk"), fs);

    const QModelIndex row = m_model->index(0, 0);
    QCOMPARE(
        row.data(DriveListModel::StateRole).value<DriveListModel::State>(), DriveListModel::State::Local);
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

MOLE_TEST_MAIN(TestDriveListModel)
#include "tst_DriveListModel.moc"
