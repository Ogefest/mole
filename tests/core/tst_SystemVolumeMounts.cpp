#include "support/MoleTestMain.h"

#include "core/vfs/SystemVolumeMounts.h"
#include "core/vfs/SystemVolumes.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;

/// A drive plugged in or pulled out while Mole is running, held offline.
///
/// No hardware and no mount namespace: SystemVolumes::setEnumerator() stands in
/// for the mount table, so a case hands the diff one table and then another and
/// looks at what the Vfs holds afterwards. The table goes through the same
/// filter the real one does, which is what lets the snap case mean something.
/// See MOLE-429.
class TestSystemVolumeMounts : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aDrivePluggedInAppearsAndOneUnpluggedGoes();
    void aDriveThatDidNotChangeKeepsItsMount();
    void aSnapLoopbackAppearingProducesNoDrive();
    void anotherDiskAtTheSamePlaceIsAnotherDrive();
    void theVolumeCarryingHomeIsLeftToTheHomeRow();
    void onlyTheMountsItMadeAreItsToRemove();

    // --- what a disk is, rather than where it is mounted (MOLE-430) ---------
    void aStickPluggedBackInElsewhereIsTheSameDriveUnderItsOwnName();
    void aStickMovedWithoutBeingSeenToGoKeepsItsMountId();
    void twoSticksWithTheSameLabelAreTwoDrives();
    void aDiskWithNoUuidFallsBackToItsMountPoint();

private:
    static SystemVolume volume(
        const QString& rootPath, const QString& device, const QString& type = QStringLiteral("ext4"));
    /// What the next sync will find mounted.
    void tableIs(const QList<SystemVolume>& volumes);
    QStringList mountedRoots() const;

    QList<SystemVolume> m_table;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<SystemVolumeMounts> m_volumes;
    /// A /dev and a /dev/disk of the test's own, so what a disk is does not
    /// depend on the disks of whoever runs it. Empty unless a case adds to it.
    std::unique_ptr<QTemporaryDir> m_disks;
    /// A device node in the fake /dev, with the links udev would make to it.
    QString plugIn(const QString& node, const QString& uuid, const QString& label = {});
    int m_made = 0;
};

namespace {
const QString kHome = QStringLiteral("/home/tester");
}

SystemVolume TestSystemVolumeMounts::volume(
    const QString& rootPath, const QString& device, const QString& type)
{
    SystemVolume out;
    out.rootPath = rootPath;
    out.device = device;
    out.fileSystemType = type;
    return out;
}

void TestSystemVolumeMounts::tableIs(const QList<SystemVolume>& volumes)
{
    m_table = volumes;
}

QStringList TestSystemVolumeMounts::mountedRoots() const
{
    QStringList out;
    for (const Mount& mount : m_vfs->mounts())
        out.append(mount.root.toLocalPath());
    return out;
}

void TestSystemVolumeMounts::init()
{
    m_table.clear();
    m_made = 0;
    SystemVolumes::setEnumerator([this] { return m_table; });
    m_disks = std::make_unique<QTemporaryDir>();
    QVERIFY(m_disks->isValid());
    for (const char* dir : { "dev", "disk/by-uuid", "disk/by-label" })
        QVERIFY(QDir(m_disks->path()).mkpath(QLatin1String(dir)));
    SystemVolumes::setDiskDirectory(m_disks->filePath(QStringLiteral("disk")));
    m_vfs = std::make_unique<VfsManager>();
    m_volumes = std::make_unique<SystemVolumeMounts>(
        m_vfs.get(),
        [this](const QString&) {
            ++m_made;
            return std::make_shared<MemoryFileSystem>();
        },
        kHome);
}

void TestSystemVolumeMounts::cleanup()
{
    m_volumes.reset();
    m_vfs.reset();
    SystemVolumes::setEnumerator(nullptr);
    SystemVolumes::setDiskDirectory({});
    m_disks.reset();
}

QString TestSystemVolumeMounts::plugIn(const QString& node, const QString& uuid, const QString& label)
{
    const QString device = m_disks->filePath(QStringLiteral("dev/") + node);
    QFile file(device);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    // Replaced rather than added, the way udev moves a link when the same disk
    // comes back as another node.
    const auto relink = [&](const QString& kind, const QString& name) {
        const QString at = m_disks->filePath(QStringLiteral("disk/") + kind + QLatin1Char('/') + name);
        QFile::remove(at);
        QFile::link(QStringLiteral("../../dev/") + node, at);
    };
    if (!uuid.isEmpty())
        relink(QStringLiteral("by-uuid"), uuid);
    if (!label.isEmpty())
        relink(QStringLiteral("by-label"), label);
    return device;
}

void TestSystemVolumeMounts::aDrivePluggedInAppearsAndOneUnpluggedGoes()
{
    const SystemVolume root = volume(QStringLiteral("/"), QStringLiteral("/dev/sda2"));
    const SystemVolume stick
        = volume(QStringLiteral("/media/tester/BACKUP"), QStringLiteral("/dev/sdb1"), QStringLiteral("vfat"));

    tableIs({ root });
    m_volumes->sync();
    QCOMPARE(mountedRoots(), QStringList { QStringLiteral("/") });

    QSignalSpy changed(m_volumes.get(), &SystemVolumeMounts::changed);
    tableIs({ root, stick });
    m_volumes->sync();
    // Exactly one drive more, and it is the stick, under the name a person reads.
    QCOMPARE(mountedRoots(), (QStringList { QStringLiteral("/"), QStringLiteral("/media/tester/BACKUP") }));
    QCOMPARE(m_vfs->mount(m_volumes->mountIdFor(stick.rootPath)).displayName, QStringLiteral("BACKUP"));
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.at(0).at(0).toStringList(), QStringList { stick.rootPath });
    QVERIFY(changed.at(0).at(1).toStringList().isEmpty());

    tableIs({ root });
    m_volumes->sync();
    QCOMPARE(mountedRoots(), QStringList { QStringLiteral("/") });
    QCOMPARE(changed.size(), 2);
    QCOMPARE(changed.at(1).at(1).toStringList(), QStringList { stick.rootPath });
    QVERIFY(m_volumes->mountIdFor(stick.rootPath).isEmpty());
}

void TestSystemVolumeMounts::aDriveThatDidNotChangeKeepsItsMount()
{
    // A pane standing on a drive holds a uri into its mount. Remounting a drive
    // that did not change, because something else did, is what would pull it out
    // from under the pane.
    const SystemVolume root = volume(QStringLiteral("/"), QStringLiteral("/dev/sda2"));
    const SystemVolume data = volume(QStringLiteral("/mnt/data"), QStringLiteral("/dev/sdc1"));
    tableIs({ root, data });
    m_volumes->sync();

    const QString id = m_volumes->mountIdFor(data.rootPath);
    const VfsUri inside = VfsUri::fromLocalPath(QStringLiteral("/mnt/data/projects/report.txt"));
    const FileSystemPtr before = m_vfs->resolve(inside);
    QVERIFY(before);
    const int madeBefore = m_made;

    tableIs({ root, data,
        volume(QStringLiteral("/media/tester/STICK"), QStringLiteral("/dev/sdb1"), QStringLiteral("vfat")) });
    m_volumes->sync();
    tableIs({ root, data });
    m_volumes->sync();

    QCOMPARE(m_volumes->mountIdFor(data.rootPath), id);
    QCOMPARE(m_vfs->resolve(inside).get(), before.get());
    QCOMPARE(m_vfs->mountForUri(inside).id, id);
    // One filesystem built, for the stick, and none rebuilt for what was there.
    QCOMPARE(m_made, madeBefore + 1);
}

void TestSystemVolumeMounts::aSnapLoopbackAppearingProducesNoDrive()
{
    // What `snap install` does in the background: a squashfs on a loop device,
    // under /snap. SystemVolumes' own filter is what keeps it out, so this feeds
    // it through the seam rather than reimplementing the rule.
    const SystemVolume root = volume(QStringLiteral("/"), QStringLiteral("/dev/sda2"));
    tableIs({ root });
    m_volumes->sync();

    QSignalSpy changed(m_volumes.get(), &SystemVolumeMounts::changed);
    tableIs({ root,
        volume(QStringLiteral("/snap/hello/42"), QStringLiteral("/dev/loop7"), QStringLiteral("squashfs")) });
    m_volumes->sync();

    QCOMPARE(mountedRoots(), QStringList { QStringLiteral("/") });
    QCOMPARE(changed.size(), 0);
}

void TestSystemVolumeMounts::anotherDiskAtTheSamePlaceIsAnotherDrive()
{
    // Pull one stick out and put another in, and the automounter may well give
    // it the same directory. Same place, different disk: the old mount goes and
    // a new one comes, rather than the first disk's mount pointing at the second.
    const SystemVolume first
        = volume(QStringLiteral("/media/tester/disk"), QStringLiteral("/dev/sdb1"), QStringLiteral("vfat"));
    tableIs({ first });
    m_volumes->sync();
    const QString firstId = m_volumes->mountIdFor(first.rootPath);

    tableIs({ volume(
        QStringLiteral("/media/tester/disk"), QStringLiteral("/dev/sdd1"), QStringLiteral("exfat")) });
    m_volumes->sync();

    QVERIFY(!m_volumes->mountIdFor(first.rootPath).isEmpty());
    QVERIFY(m_volumes->mountIdFor(first.rootPath) != firstId);
    QCOMPARE(m_vfs->mounts().size(), 1);
}

void TestSystemVolumeMounts::theVolumeCarryingHomeIsLeftToTheHomeRow()
{
    // Where /home is its own dataset, the volume and the Home row are the same
    // place; it is mounted once, as Home, by whoever made the Home row.
    tableIs({ volume(QStringLiteral("/"), QStringLiteral("/dev/sda2")),
        volume(kHome, QStringLiteral("rpool/home"), QStringLiteral("zfs")) });
    m_volumes->sync();
    QCOMPARE(mountedRoots(), QStringList { QStringLiteral("/") });
}

void TestSystemVolumeMounts::onlyTheMountsItMadeAreItsToRemove()
{
    // Home, a configured drive, an archive somebody is inside: none of them is in
    // the mount table, and none of them is this class's to take away.
    Mount home;
    home.displayName = QStringLiteral("Home");
    home.root = VfsUri::fromLocalPath(kHome);
    home.fileSystem = std::make_shared<MemoryFileSystem>();
    const QString homeId = m_vfs->addMount(home);

    tableIs({ volume(
        QStringLiteral("/media/tester/STICK"), QStringLiteral("/dev/sdb1"), QStringLiteral("vfat")) });
    m_volumes->sync();
    tableIs({});
    m_volumes->sync();

    QCOMPARE(m_vfs->mounts().size(), 1);
    QCOMPARE(m_vfs->mounts().first().id, homeId);
}

void TestSystemVolumeMounts::aStickPluggedBackInElsewhereIsTheSameDriveUnderItsOwnName()
{
    // Pulled out while another stick was in, and put back: the automounter
    // gives it BACKUP1, and the kernel may give it another node. It is the same
    // disk, so it is the same mount id -- which is what the drive list's row
    // and a pane that was on it go by -- and it reads BACKUP, the label it
    // carries, rather than the directory it landed in.
    const SystemVolume root = volume(QStringLiteral("/"), QStringLiteral("/dev/sda2"));
    const QString first
        = plugIn(QStringLiteral("sdb1"), QStringLiteral("0A1B-2C3D"), QStringLiteral("BACKUP"));
    tableIs({ root, volume(QStringLiteral("/media/tester/BACKUP"), first, QStringLiteral("vfat")) });
    m_volumes->sync();
    const QString id = m_volumes->mountIdFor(QStringLiteral("/media/tester/BACKUP"));
    QCOMPARE(id, QStringLiteral("volume:0A1B-2C3D"));
    QCOMPARE(m_vfs->mount(id).displayName, QStringLiteral("BACKUP"));

    tableIs({ root });
    m_volumes->sync();
    QVERIFY(m_vfs->mount(id).id.isEmpty());

    const QString again
        = plugIn(QStringLiteral("sdc1"), QStringLiteral("0A1B-2C3D"), QStringLiteral("BACKUP"));
    tableIs({ root, volume(QStringLiteral("/media/tester/BACKUP1"), again, QStringLiteral("vfat")) });
    m_volumes->sync();
    QCOMPARE(m_volumes->mountIdFor(QStringLiteral("/media/tester/BACKUP1")), id);
    QCOMPARE(m_vfs->mount(id).displayName, QStringLiteral("BACKUP"));
    QCOMPARE(m_vfs->mount(id).root.toLocalPath(), QStringLiteral("/media/tester/BACKUP1"));
}

void TestSystemVolumeMounts::aStickMovedWithoutBeingSeenToGoKeepsItsMountId()
{
    // Out and in between two reads of the table, so the diff never sees it
    // missing: it is still the same disk, now somewhere else, and its mount is
    // moved there rather than left pointing at a directory that is gone.
    const QString device
        = plugIn(QStringLiteral("sdb1"), QStringLiteral("0A1B-2C3D"), QStringLiteral("BACKUP"));
    tableIs({ volume(QStringLiteral("/media/tester/BACKUP"), device, QStringLiteral("vfat")) });
    m_volumes->sync();

    tableIs({ volume(QStringLiteral("/media/tester/BACKUP1"), device, QStringLiteral("vfat")) });
    m_volumes->sync();
    QCOMPARE(m_vfs->mounts().size(), 1);
    QCOMPARE(m_vfs->mounts().first().id, QStringLiteral("volume:0A1B-2C3D"));
    QCOMPARE(m_vfs->mounts().first().root.toLocalPath(), QStringLiteral("/media/tester/BACKUP1"));
}

void TestSystemVolumeMounts::twoSticksWithTheSameLabelAreTwoDrives()
{
    // Two sticks that both say BACKUP. udev's by-label link can only point at one
    // of them, so at most one reads BACKUP; what keeps them apart is that they are
    // two disks, not what either is called.
    const QString one = plugIn(QStringLiteral("sdb1"), QStringLiteral("1111-1111"), QStringLiteral("BACKUP"));
    const QString two = plugIn(QStringLiteral("sdc1"), QStringLiteral("2222-2222"), QStringLiteral("BACKUP"));
    tableIs({ volume(QStringLiteral("/media/tester/BACKUP"), one, QStringLiteral("vfat")),
        volume(QStringLiteral("/media/tester/BACKUP1"), two, QStringLiteral("vfat")) });
    m_volumes->sync();

    QCOMPARE(m_vfs->mounts().size(), 2);
    QCOMPARE(
        m_volumes->mountIdFor(QStringLiteral("/media/tester/BACKUP")), QStringLiteral("volume:1111-1111"));
    QCOMPARE(
        m_volumes->mountIdFor(QStringLiteral("/media/tester/BACKUP1")), QStringLiteral("volume:2222-2222"));
}

void TestSystemVolumeMounts::aDiskWithNoUuidFallsBackToItsMountPoint()
{
    // A bind mount, a share, a filesystem with no UUID, a platform with no
    // /dev/disk: known by where it is, under a name from its mount point, and
    // given whatever mount id the Vfs allocates -- which is exactly what every
    // drive had before MOLE-430. Named here so the fallback cannot disappear in
    // a rewrite without a test noticing.
    const QString device = plugIn(QStringLiteral("sdb1"), {});
    const SystemVolume plain = volume(QStringLiteral("/mnt/scratch"), device);
    tableIs({ plain });
    m_volumes->sync();

    SystemVolume read = SystemVolumes::enumerate().first();
    QVERIFY(read.uuid.isEmpty());
    QCOMPARE(SystemVolumeMounts::identityOf(read), QStringLiteral("path:/mnt/scratch"));
    QVERIFY(SystemVolumeMounts::mountIdOf(read).isEmpty());
    const QString id = m_volumes->mountIdFor(QStringLiteral("/mnt/scratch"));
    QVERIFY(!id.isEmpty());
    QVERIFY(!id.startsWith(QStringLiteral("volume:")));
    QCOMPARE(m_vfs->mount(id).displayName, QStringLiteral("scratch"));
}

MOLE_TEST_MAIN(TestSystemVolumeMounts)
#include "tst_SystemVolumeMounts.moc"
