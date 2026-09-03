#include "support/MoleTestMain.h"

#include "core/vfs/SystemVolumes.h"

using namespace mole;

Q_DECLARE_METATYPE(mole::HostPlatform)

/// What the sidebar calls a drive, asked of three machines none of us has.
///
/// isInteresting() and displayName() take strings and touch no hardware -- they
/// were written that way on purpose -- and until this file nothing tested either
/// of them. The result was a rule made of three Linux mount prefixes that
/// returned nothing at all on Windows and one row called "Root" on macOS.
///
/// The platform is an argument, so every case here runs on every machine.
class TestSystemVolumes : public QObject
{
    Q_OBJECT

private slots:
    void aLinuxMachine_data();
    void aLinuxMachine();

    void aWindowsMachine_data();
    void aWindowsMachine();

    void aMacMachine_data();
    void aMacMachine();

    void theMachineTheRuleWasWrongOn_data();
    void theMachineTheRuleWasWrongOn();
    void aDiskOutsideEveryConventionIsStillADisk_data();
    void aDiskOutsideEveryConventionIsStillADisk();
    void networkSharesAreDrivesWhereverTheyAreMounted();
    void aMountTableIsReadWithoutTouchingAnyOfTheFilesystemsInIt();
    void nothingIsAskedAboutCapacityWhileTheDrivesAreBeingFound();
    void aNameIsNeverEmpty();
};

void TestSystemVolumes::aLinuxMachine_data()
{
    QTest::addColumn<QString>("rootPath");
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("device");
    QTest::addColumn<bool>("expected");

    QTest::newRow("the root") << "/" << "ext4" << "/dev/nvme0n1p2" << true;
    QTest::newRow("a plugged-in disk") << "/media/ann/BACKUP" << "ext4" << "/dev/sdb1" << true;
    QTest::newRow("udisks2 removable") << "/run/media/ann/STICK" << "vfat" << "/dev/sdc1" << true;
    QTest::newRow("a second disk under /mnt") << "/mnt/data" << "xfs" << "/dev/sdd1" << true;

    // The pile a modern Linux box mounts and nobody wants listed.
    QTest::newRow("a snap loopback") << "/snap/firefox/1234" << "squashfs" << "/dev/loop7" << false;
    QTest::newRow("a loopback elsewhere") << "/mnt/whatever" << "ext4" << "/dev/loop3" << false;
    QTest::newRow("cgroups") << "/sys/fs/cgroup" << "cgroup2" << "cgroup2" << false;
    QTest::newRow("proc") << "/proc" << "proc" << "proc" << false;
    QTest::newRow("efivars") << "/sys/firmware/efi/efivars" << "efivarfs" << "efivarfs" << false;
    QTest::newRow("a zfs dataset in the middle") << "/var/lib/dpkg" << "zfs" << "rpool/dpkg" << false;
    QTest::newRow("an empty root path") << "" << "ext4" << "/dev/sde1" << false;
    // The prefix has to have something under it. Asked with a dataset rather
    // than a disk, so it is the prefix rule being tested and not the one below
    // it that admits any real disk outside the operating system.
    QTest::newRow("the prefix itself is not a mount under it") << "/media/" << "zfs" << "pool/media" << false;
}

void TestSystemVolumes::aLinuxMachine()
{
    QFETCH(QString, rootPath);
    QFETCH(QString, type);
    QFETCH(QString, device);
    QFETCH(bool, expected);

    QCOMPARE(SystemVolumes::isInteresting(rootPath, type, device, HostPlatform::Posix), expected);
}

void TestSystemVolumes::aWindowsMachine_data()
{
    QTest::addColumn<QString>("rootPath");
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("device");
    QTest::addColumn<bool>("expected");

    // Every ready volume is a drive. There is nothing to filter: nothing on
    // Windows mounts a filesystem per installed package, which is the problem
    // the Linux allowlist exists to solve.
    QTest::newRow("the system drive") << "C:/" << "NTFS" << "\\\\?\\Volume{aaaa}" << true;
    QTest::newRow("a second drive") << "D:/" << "NTFS" << "\\\\?\\Volume{bbbb}" << true;
    QTest::newRow("a memory stick") << "E:/" << "FAT32" << "\\\\?\\Volume{cccc}" << true;
    QTest::newRow("a mapped share") << "Z:/" << "NTFS" << "\\\\server\\share" << true;

    // The old rule answered false to every one of those, because none of them
    // is "/" and none begins with /media/.
    QTest::newRow("an empty root path") << "" << "NTFS" << "" << false;
}

void TestSystemVolumes::aWindowsMachine()
{
    QFETCH(QString, rootPath);
    QFETCH(QString, type);
    QFETCH(QString, device);
    QFETCH(bool, expected);

    QCOMPARE(SystemVolumes::isInteresting(rootPath, type, device, HostPlatform::Windows), expected);

    // The old rule dropped the lot. Stated as its own claim so a change that
    // reintroduced an allowlist here fails loudly.
    if (!rootPath.isEmpty())
        QVERIFY(SystemVolumes::isInteresting(rootPath, type, device, HostPlatform::Windows));
}

void TestSystemVolumes::aMacMachine_data()
{
    QTest::addColumn<QString>("rootPath");
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("device");
    QTest::addColumn<bool>("expected");

    QTest::newRow("the boot volume") << "/" << "apfs" << "/dev/disk1s5" << true;
    QTest::newRow("an external disk") << "/Volumes/Backup" << "apfs" << "/dev/disk4s2" << true;
    QTest::newRow("a mounted image") << "/Volumes/Installer" << "hfs" << "/dev/disk6s1" << true;

    // Inert on Linux and real here.
    QTest::newRow("devfs") << "/dev" << "devfs" << "devfs" << false;
    QTest::newRow("an automount map") << "/System/Volumes/Data/home" << "map" << "map auto_home" << false;
    QTest::newRow("autofs") << "/net" << "autofs" << "map -hosts" << false;
    // The Linux conventions are not conventions here: a dataset mounted at
    // /mnt/data on a Mac is admitted by nothing.
    QTest::newRow("a linux prefix means nothing here") << "/mnt/data" << "zfs" << "pool/data" << false;
    // A real disk is a real disk on any system, though, wherever it is mounted.
    QTest::newRow("a real disk outside /Volumes") << "/mnt/data" << "apfs" << "/dev/disk9s1" << true;
}

void TestSystemVolumes::aMacMachine()
{
    QFETCH(QString, rootPath);
    QFETCH(QString, type);
    QFETCH(QString, device);
    QFETCH(bool, expected);

    QCOMPARE(SystemVolumes::isInteresting(rootPath, type, device, HostPlatform::MacOS), expected);
}

/// The layout of a real Ubuntu-on-ZFS machine, which is where the old rule was
/// found answering wrongly about the system it was written for. Three mount
/// prefixes are one Linux convention out of several, and asking "where is this
/// mounted" is not the same question as "is this somewhere I keep files".
void TestSystemVolumes::theMachineTheRuleWasWrongOn_data()
{
    QTest::addColumn<QString>("rootPath");
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("device");
    QTest::addColumn<bool>("expected");

    // A real disk holding everything the user owns, named by no convention at
    // all. The old rule said no, and it was the worst answer on the list.
    QTest::newRow("a separate /home dataset") << "/home/ann" << "zfs" << "pool/USERDATA/ann" << true;

    // A ramdisk, listed by the old rule purely because of where it sits.
    QTest::newRow("a ramdisk under /mnt") << "/mnt/ramdisk" << "tmpfs" << "tmpfs" << false;
    QTest::newRow("a ramdisk anywhere else") << "/scratch" << "tmpfs" << "tmpfs" << false;

    // Right before and right after.
    QTest::newRow("a disk under /mnt") << "/mnt/SG4T" << "ext4" << "/dev/sda" << true;
    QTest::newRow("an nfs export under /mnt") << "/mnt/nas" << "nfs4" << "server:/export" << true;
    QTest::newRow("udisks2 removable media") << "/run/media/ann/STICK" << "vfat" << "/dev/sdc1" << true;
    QTest::newRow("the root") << "/" << "zfs" << "pool/ROOT/ubuntu" << true;

    // The pile the allowlist comment is about: separate filesystems, real
    // mounts, utterly uninteresting, and no blocklist would keep up with them.
    // They stay out because a dataset name is not a backing device.
    for (const char* dataset : { "/var/lib/dpkg", "/var/games", "/usr/local", "/srv", "/var/log" }) {
        QTest::newRow(dataset) << QString::fromLatin1(dataset) << "zfs"
                               << QStringLiteral("pool/ROOT/ubuntu%1").arg(QString::fromLatin1(dataset))
                               << false;
    }

    // Twenty-odd of these on an ordinary machine.
    QTest::newRow("a snap loopback") << "/snap/firefox/8736" << "squashfs" << "/dev/loop8" << false;

    // On a real partition, and not anybody's files. Without the
    // system-directory test, admitting a mount on a real disk admits these two.
    QTest::newRow("the efi partition") << "/boot/efi" << "vfat" << "/dev/nvme0n1p1" << false;
    QTest::newRow("grub on the same partition") << "/boot/grub" << "vfat" << "/dev/nvme0n1p1" << false;
    // The zfs keystore: a real device-mapper node, and still the operating
    // system's own business rather than somewhere anybody keeps files.
    QTest::newRow("the zfs keystore")
        << "/run/keystore/rpool" << "ext4" << "/dev/mapper/keystore-rpool" << false;
}

void TestSystemVolumes::theMachineTheRuleWasWrongOn()
{
    QFETCH(QString, rootPath);
    QFETCH(QString, type);
    QFETCH(QString, device);
    QFETCH(bool, expected);

    QCOMPARE(SystemVolumes::isInteresting(
                 rootPath, type, device, HostPlatform::Posix, QStringLiteral("/home/ann")),
        expected);
}

void TestSystemVolumes::aDiskOutsideEveryConventionIsStillADisk_data()
{
    QTest::addColumn<QString>("rootPath");
    QTest::addColumn<bool>("expected");

    // Where a great many people actually mount a second disk, named by none of
    // the three prefixes. The allowlist is a signal now rather than the only
    // gate, so a real disk that is not part of the operating system counts.
    QTest::newRow("/storage") << "/storage" << true;
    QTest::newRow("/data") << "/data" << true;
    QTest::newRow("/pool") << "/pool" << true;
    QTest::newRow("/games") << "/games" << true;

    // Still not, because these are the operating system.
    QTest::newRow("/usr") << "/usr" << false;
    QTest::newRow("/var/lib") << "/var/lib" << false;
    QTest::newRow("/boot") << "/boot" << false;
    QTest::newRow("/tmp") << "/tmp" << false;
}

void TestSystemVolumes::aDiskOutsideEveryConventionIsStillADisk()
{
    QFETCH(QString, rootPath);
    QFETCH(bool, expected);

    QCOMPARE(SystemVolumes::isInteresting(rootPath, QStringLiteral("ext4"), QStringLiteral("/dev/sdb1"),
                 HostPlatform::Posix, QStringLiteral("/home/ann")),
        expected);
}

void TestSystemVolumes::networkSharesAreDrivesWhereverTheyAreMounted()
{
    // Already portable and needing nothing: cifs and nfs mean the same thing
    // wherever they are mounted.
    for (HostPlatform platform : { HostPlatform::Posix, HostPlatform::MacOS, HostPlatform::Windows }) {
        QVERIFY(SystemVolumes::isInteresting(QStringLiteral("/home/ann/nas"), QStringLiteral("nfs4"),
            QStringLiteral("nas:/export"), platform));
        QVERIFY(SystemVolumes::isInteresting(
            QStringLiteral("/x/share"), QStringLiteral("cifs"), QStringLiteral("//server/share"), platform));
    }
}

void TestSystemVolumes::aNameIsNeverEmpty()
{
    const QString none;

    // Windows: the label, or the drive letter -- and never "Root", which names
    // nothing on that system.
    QCOMPARE(SystemVolumes::displayName(
                 QStringLiteral("C:/"), QStringLiteral("Windows"), none, HostPlatform::Windows),
        QStringLiteral("Windows"));
    QCOMPARE(SystemVolumes::displayName(QStringLiteral("D:/"), none, none, HostPlatform::Windows),
        QStringLiteral("D:"));
    QVERIFY(SystemVolumes::displayName(QStringLiteral("C:/"), none, none, HostPlatform::Windows)
        != QStringLiteral("Root"));

    // macOS: the boot volume has a name of its own and it is the one the user
    // recognises, so it wins over the generic word.
    QCOMPARE(SystemVolumes::displayName(
                 QStringLiteral("/"), QStringLiteral("Macintosh HD"), none, HostPlatform::MacOS),
        QStringLiteral("Macintosh HD"));
    QCOMPARE(SystemVolumes::displayName(
                 QStringLiteral("/Volumes/Backup"), QStringLiteral("Backup"), none, HostPlatform::MacOS),
        QStringLiteral("Backup"));
    QCOMPARE(SystemVolumes::displayName(QStringLiteral("/"), none, none, HostPlatform::MacOS),
        QStringLiteral("Root"));

    // Linux: exactly what it did before.
    QCOMPARE(
        SystemVolumes::displayName(QStringLiteral("/"), QStringLiteral("ignored"), none, HostPlatform::Posix),
        QStringLiteral("Root"));
    QCOMPARE(SystemVolumes::displayName(QStringLiteral("/media/ann/BACKUP"), none, none, HostPlatform::Posix),
        QStringLiteral("BACKUP"));
    QCOMPARE(SystemVolumes::displayName(
                 QStringLiteral("/mnt/data"), QStringLiteral("Archive"), none, HostPlatform::Posix),
        QStringLiteral("Archive"));

    // Whatever is asked, something readable comes back.
    for (HostPlatform platform : { HostPlatform::Posix, HostPlatform::MacOS, HostPlatform::Windows }) {
        QVERIFY(!SystemVolumes::displayName(
            QStringLiteral("/some/mount"), none, QStringLiteral("/dev/sdz1"), platform)
                     .isEmpty());
    }
}

/// The table, read as text -- which is the whole of the fix.
///
/// The list used to come from QStorageInfo::mountedVolumes(), which constructs
/// one QStorageInfo per entry, and every one of those is a statvfs. On the mount
/// that has stopped answering -- the stale NFS or SMB mount the network plugin
/// exists so nobody needs -- a statvfs blocks for the kernel's timeout. So Mole
/// hung at startup with no window and no message, and again on every sidebar
/// refresh. Reading the table tells us everything the sidebar needs and touches
/// none of the filesystems in it. See MOLE-361.
///
/// The fixture is a machine nobody here has: a ZFS root whose device is a
/// dataset, a mount point with a space in its name, a snap loopback, and the
/// dead share.
void TestSystemVolumes::aMountTableIsReadWithoutTouchingAnyOfTheFilesystemsInIt()
{
    const QByteArray table = "rpool/ROOT/ubuntu / zfs rw,relatime,xattr 0 0\n"
                             "/dev/nvme0n1p1 /boot/efi vfat ro,relatime,fmask=0077 0 0\n"
                             "/dev/sdb1 /media/alice/My\\040Backup ext4 rw,relatime 0 0\n"
                             "/dev/loop14 /snap/firefox/4793 squashfs ro,nodev,relatime 0 0\n"
                             "fileserver:/export /mnt/nas nfs4 rw,relatime,hard 0 0\n"
                             "tmpfs /run/user/1000 tmpfs rw,nosuid,nodev,size=3223996k 0 0\n";

    const QList<SystemVolume> read = SystemVolumes::parseMountTable(table);
    QCOMPARE(read.size(), 6);

    QHash<QString, SystemVolume> byRoot;
    for (const SystemVolume& volume : read)
        byRoot.insert(volume.rootPath, volume);

    // A space in a mount point arrives as \040, and a reader that did not put it
    // back would answer with a path no file has -- which for a disk called "My
    // Backup" is the whole drive missing.
    QVERIFY2(byRoot.contains(QStringLiteral("/media/alice/My Backup")),
        "a mount point with a space in it is one path");
    QCOMPARE(byRoot.value(QStringLiteral("/media/alice/My Backup")).device, QStringLiteral("/dev/sdb1"));
    QVERIFY(!byRoot.value(QStringLiteral("/media/alice/My Backup")).isReadOnly);

    // The root of a ZFS machine: the device is a dataset name and not a node
    // under /dev, which is the distinction that keeps the rest of the pile out.
    QCOMPARE(byRoot.value(QStringLiteral("/")).fileSystemType, QStringLiteral("zfs"));
    QCOMPARE(byRoot.value(QStringLiteral("/")).device, QStringLiteral("rpool/ROOT/ubuntu"));

    // Read-only comes from the options, because nothing else here may be asked.
    QVERIFY(byRoot.value(QStringLiteral("/boot/efi")).isReadOnly);
    QVERIFY(byRoot.value(QStringLiteral("/snap/firefox/4793")).isReadOnly);

    // And nothing carries a capacity: reading the table cannot know one, and
    // that is the point rather than an omission.
    for (const SystemVolume& volume : read) {
        QCOMPARE(volume.totalBytes, qint64(0));
        QCOMPARE(volume.freeBytes, qint64(0));
    }

    // The rules already tested above then filter it, and the filtering is what
    // the sidebar shows: the root, the backup disk and the share.
    QStringList drives;
    for (const SystemVolume& volume : read) {
        if (SystemVolumes::isInteresting(volume.rootPath, volume.fileSystemType, volume.device,
                HostPlatform::Posix, QStringLiteral("/home/alice")))
            drives.append(volume.rootPath);
    }
    drives.sort();
    QCOMPARE(drives,
        QStringList(
            { QStringLiteral("/"), QStringLiteral("/media/alice/My Backup"), QStringLiteral("/mnt/nas") }));
}

/// Capacity is QuerySpaceTask's, and enumerating must not go looking for it.
///
/// ARCHITECTURE.md's "Capacity" section says why that task exists: QStorageInfo
/// blocks on an unreachable mount, so asking from the interface would freeze the
/// window. Enumerating the volumes asked anyway -- bytesTotal() and
/// bytesAvailable() per entry -- and then threw the answers away, which is the
/// worst of both. Asserted on the real machine, because the claim is about what
/// this code does rather than about what any particular disk holds. See
/// MOLE-361.
void TestSystemVolumes::nothingIsAskedAboutCapacityWhileTheDrivesAreBeingFound()
{
    const QList<SystemVolume> volumes = SystemVolumes::enumerate();
    QVERIFY2(!volumes.isEmpty(), "every machine has at least a root filesystem");

    for (const SystemVolume& volume : volumes) {
        QVERIFY2(volume.totalBytes == 0,
            qPrintable(QStringLiteral("%1 came back with a capacity").arg(volume.rootPath)));
        QVERIFY2(volume.freeBytes == 0,
            qPrintable(QStringLiteral("%1 came back with a free figure").arg(volume.rootPath)));
        // And what it does answer is still there.
        QVERIFY(!volume.rootPath.isEmpty());
        QVERIFY(!volume.name.isEmpty());
    }
}

MOLE_TEST_MAIN(TestSystemVolumes)
#include "tst_SystemVolumes.moc"
