#include "support/MoleTestMain.h"

#include "core/vfs/SystemVolumeMounts.h"
#include "core/vfs/SystemVolumes.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QScopeGuard>
#include <QSignalSpy>

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

private:
    static SystemVolume volume(
        const QString& rootPath, const QString& device, const QString& type = QStringLiteral("ext4"));
    /// What the next sync will find mounted.
    void tableIs(const QList<SystemVolume>& volumes);
    QStringList mountedRoots() const;

    QList<SystemVolume> m_table;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<SystemVolumeMounts> m_volumes;
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

MOLE_TEST_MAIN(TestSystemVolumeMounts)
#include "tst_SystemVolumeMounts.moc"
