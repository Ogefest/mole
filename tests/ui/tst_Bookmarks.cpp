#include "support/MoleTestMain.h"
#include "ui/models/BookmarkModel.h"

#include "core/vfs/SystemVolumes.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;

class TestBookmarks : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // --- bookmarks ---
    void obeysTheModelContract();
    void addsWithADerivedName();
    void namesTheRootOfADriveSensibly();
    void refusesDuplicatesAndEmptyUris();
    void removesByUriAndByRow();
    void renamesAnEntry();
    void survivesAReload();
    void corruptFileLeavesAnEmptyList();
    void exposesRolesQmlNeeds();
    void boundsAreChecked();

    // --- system volumes ---
    void keepsRealDrives_data();
    void keepsRealDrives();
    void recognisesNetworkFileSystems();
    void namesVolumes_data();
    void namesVolumes();
    void enumerationAlwaysIncludesRoot();

private:
    QString path() const;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<BookmarkModel> m_model;
};

QString TestBookmarks::path() const
{
    return QDir(m_dir->path()).filePath(QStringLiteral("bookmarks.json"));
}

void TestBookmarks::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_model = std::make_unique<BookmarkModel>(path());
}

void TestBookmarks::cleanup()
{
    m_model.reset();
    m_dir.reset();
}

void TestBookmarks::obeysTheModelContract()
{
    QAbstractItemModelTester tester(m_model.get(), QAbstractItemModelTester::FailureReportingMode::Warning);

    m_model->add(QStringLiteral("file:///home/lg/photos"));
    m_model->add(QStringLiteral("file:///mnt/nas/media"));
    m_model->rename(0, QStringLiteral("Pictures"));
    m_model->removeAt(0);
    QCOMPARE(m_model->rowCount(), 1);
}

void TestBookmarks::addsWithADerivedName()
{
    QSignalSpy countSpy(m_model.get(), &BookmarkModel::countChanged);
    QVERIFY(m_model->add(QStringLiteral("file:///home/lg/photos")));

    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->index(0, 0).data(BookmarkModel::NameRole).toString(), QStringLiteral("photos"));
    QCOMPARE(countSpy.count(), 1);

    // An explicit name wins over the derived one.
    QVERIFY(m_model->add(QStringLiteral("file:///srv/backup"), QStringLiteral("Nightly backup")));
    QCOMPARE(m_model->index(1, 0).data(BookmarkModel::NameRole).toString(), QStringLiteral("Nightly backup"));
}

void TestBookmarks::namesTheRootOfADriveSensibly()
{
    // The root of a mount has no file name to borrow, so fall back to
    // something that still tells the user what it is.
    QVERIFY(m_model->add(QStringLiteral("mem:///")));
    QCOMPARE(m_model->index(0, 0).data(BookmarkModel::NameRole).toString(), QStringLiteral("mem"));

    QVERIFY(m_model->add(QStringLiteral("sftp://nas.local/")));
    QCOMPARE(m_model->index(1, 0).data(BookmarkModel::NameRole).toString(), QStringLiteral("nas.local"));
}

void TestBookmarks::refusesDuplicatesAndEmptyUris()
{
    QVERIFY(m_model->add(QStringLiteral("file:///home/lg")));
    // Pressing Ctrl+D twice must be harmless rather than produce two rows.
    QVERIFY(!m_model->add(QStringLiteral("file:///home/lg")));
    QVERIFY(!m_model->add(QString()));
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(m_model->contains(QStringLiteral("file:///home/lg")));
    QVERIFY(!m_model->contains(QStringLiteral("file:///elsewhere")));
}

void TestBookmarks::removesByUriAndByRow()
{
    m_model->add(QStringLiteral("file:///a"));
    m_model->add(QStringLiteral("file:///b"));

    QVERIFY(m_model->removeUri(QStringLiteral("file:///a")));
    QCOMPARE(m_model->rowCount(), 1);
    QVERIFY(!m_model->removeUri(QStringLiteral("file:///a")));

    m_model->removeAt(0);
    QCOMPARE(m_model->rowCount(), 0);
}

void TestBookmarks::renamesAnEntry()
{
    m_model->add(QStringLiteral("file:///a"));
    QVERIFY(m_model->rename(0, QStringLiteral("  Trimmed  ")));
    QCOMPARE(m_model->index(0, 0).data(BookmarkModel::NameRole).toString(), QStringLiteral("Trimmed"));

    QVERIFY(!m_model->rename(0, QStringLiteral("   ")));
    QVERIFY(!m_model->rename(9, QStringLiteral("nope")));
}

void TestBookmarks::survivesAReload()
{
    m_model->add(QStringLiteral("file:///home/lg/photos"), QStringLiteral("Pictures"));
    m_model->add(QStringLiteral("file:///mnt/nas"));

    // Adding writes through immediately: bookmarks are few and losing one to a
    // crash would be worse than the write.
    BookmarkModel reloaded(path());
    QCOMPARE(reloaded.rowCount(), 2);
    QCOMPARE(reloaded.index(0, 0).data(BookmarkModel::NameRole).toString(), QStringLiteral("Pictures"));
    QCOMPARE(reloaded.index(1, 0).data(BookmarkModel::UriRole).toString(), QStringLiteral("file:///mnt/nas"));
}

void TestBookmarks::corruptFileLeavesAnEmptyList()
{
    QFile file(path());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not json");
    file.close();

    BookmarkModel model(path());
    QCOMPARE(model.rowCount(), 0);
    // And it must still be usable afterwards rather than stuck.
    QVERIFY(model.add(QStringLiteral("file:///a")));
    QCOMPARE(model.rowCount(), 1);
}

void TestBookmarks::exposesRolesQmlNeeds()
{
    m_model->add(QStringLiteral("file:///a/b"));
    const QHash<int, QByteArray> roles = m_model->roleNames();
    QVERIFY(roles.values().contains(QByteArray("name")));
    QVERIFY(roles.values().contains(QByteArray("uri")));
    QCOMPARE(m_model->uriAt(0), QStringLiteral("file:///a/b"));
}

void TestBookmarks::boundsAreChecked()
{
    QVERIFY(m_model->uriAt(-1).isEmpty());
    QVERIFY(m_model->uriAt(5).isEmpty());
    m_model->removeAt(-1);
    m_model->removeAt(99);
    QCOMPARE(m_model->rowCount(), 0);
}

// ------------------------------------------------------------ system volumes

void TestBookmarks::keepsRealDrives_data()
{
    QTest::addColumn<QString>("rootPath");
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("device");
    QTest::addColumn<bool>("expected");

    QTest::newRow("root") << "/" << "ext4" << "/dev/sda2" << true;
    QTest::newRow("usb stick") << "/media/lg/BACKUP" << "vfat" << "/dev/sdb1" << true;
    QTest::newRow("run media") << "/run/media/lg/STICK" << "exfat" << "/dev/sdc1" << true;
    QTest::newRow("manual mount") << "/mnt/SG4T" << "ext4" << "/dev/sdd1" << true;
    QTest::newRow("nfs share") << "/mnt/nas" << "nfs4" << "nas:/export" << true;
    QTest::newRow("nfs anywhere") << "/srv/shared" << "nfs" << "nas:/export" << true;
    QTest::newRow("sshfs anywhere") << "/home/lg/remote" << "fuse.sshfs" << "user@host:" << true;

    // The cases that made the naive filter useless on a ZFS or Btrfs machine:
    // every one of these is a real, separate filesystem, and none is a drive.
    QTest::newRow("zfs dataset for apt") << "/var/lib/apt" << "zfs" << "rpool/var/apt" << false;
    QTest::newRow("zfs dataset for games") << "/var/games" << "zfs" << "rpool/var/games" << false;
    QTest::newRow("separate boot") << "/boot" << "zfs" << "bpool/BOOT" << false;
    QTest::newRow("efi partition") << "/boot/grub" << "vfat" << "/dev/sda1" << false;
    QTest::newRow("usr local") << "/usr/local" << "zfs" << "rpool/usr/local" << false;
    QTest::newRow("separate home") << "/home/lg" << "zfs" << "rpool/home/lg" << false;

    QTest::newRow("proc") << "/proc" << "proc" << "proc" << false;
    QTest::newRow("cgroup") << "/sys/fs/cgroup" << "cgroup2" << "cgroup2" << false;
    QTest::newRow("snap loopback") << "/snap/firefox/1234" << "squashfs" << "/dev/loop12" << false;
    QTest::newRow("run tmpfs") << "/run/lock" << "tmpfs" << "tmpfs" << false;
    QTest::newRow("empty path") << "" << "ext4" << "/dev/sda1" << false;
}

void TestBookmarks::keepsRealDrives()
{
    QFETCH(QString, rootPath);
    QFETCH(QString, type);
    QFETCH(QString, device);
    QFETCH(bool, expected);

    QCOMPARE(SystemVolumes::isInteresting(rootPath, type, device), expected);
}

void TestBookmarks::recognisesNetworkFileSystems()
{
    QVERIFY(SystemVolumes::isNetworkFileSystem(QStringLiteral("nfs4")));
    QVERIFY(SystemVolumes::isNetworkFileSystem(QStringLiteral("CIFS")));
    QVERIFY(SystemVolumes::isNetworkFileSystem(QStringLiteral("fuse.sshfs")));
    QVERIFY(!SystemVolumes::isNetworkFileSystem(QStringLiteral("ext4")));
    QVERIFY(!SystemVolumes::isNetworkFileSystem(QStringLiteral("zfs")));
}

void TestBookmarks::namesVolumes_data()
{
    QTest::addColumn<QString>("rootPath");
    QTest::addColumn<QString>("volumeName");
    QTest::addColumn<QString>("device");
    QTest::addColumn<QString>("expected");

    QTest::newRow("root") << "/" << "" << "/dev/sda2" << "Root";
    QTest::newRow("label wins") << "/mnt/x" << "Backup Disk" << "/dev/sdb1" << "Backup Disk";
    QTest::newRow("mount point") << "/media/lg/PHOTOS" << "" << "/dev/sdc1" << "PHOTOS";
    QTest::newRow("device fallback") << "/" << "" << "" << "Root";
}

void TestBookmarks::namesVolumes()
{
    QFETCH(QString, rootPath);
    QFETCH(QString, volumeName);
    QFETCH(QString, device);
    QFETCH(QString, expected);

    QCOMPARE(SystemVolumes::displayName(rootPath, volumeName, device), expected);
}

void TestBookmarks::enumerationAlwaysIncludesRoot()
{
    const QList<SystemVolume> volumes = SystemVolumes::enumerate();
    QVERIFY2(!volumes.isEmpty(), "every machine has at least a root filesystem");

    // Root leads, so the list does not reshuffle when something is plugged in.
    QVERIFY(volumes.first().isRoot);
    QCOMPARE(volumes.first().rootPath, QStringLiteral("/"));

    for (const SystemVolume& volume : volumes)
        QVERIFY2(!volume.name.isEmpty(), qPrintable(volume.rootPath));
}

MOLE_TEST_MAIN(TestBookmarks)
#include "tst_Bookmarks.moc"
