#include "core/vfs/SystemVolumes.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStorageInfo>

namespace mole {
namespace {

    /// Kernel bookkeeping, not storage. Anything mounted with one of these is
    /// never somewhere the user keeps files.
    const QSet<QString>& pseudoFileSystems()
    {
        static const QSet<QString> types {
            QStringLiteral("proc"),
            QStringLiteral("sysfs"),
            QStringLiteral("devtmpfs"),
            QStringLiteral("devpts"),
            QStringLiteral("cgroup"),
            QStringLiteral("cgroup2"),
            QStringLiteral("pstore"),
            QStringLiteral("securityfs"),
            QStringLiteral("debugfs"),
            QStringLiteral("tracefs"),
            QStringLiteral("configfs"),
            QStringLiteral("fusectl"),
            QStringLiteral("bpf"),
            QStringLiteral("hugetlbfs"),
            QStringLiteral("mqueue"),
            QStringLiteral("autofs"),
            QStringLiteral("binfmt_misc"),
            QStringLiteral("efivarfs"),
            QStringLiteral("ramfs"),
            QStringLiteral("nsfs"),
            QStringLiteral("rpc_pipefs"),
            QStringLiteral("selinuxfs"),
            QStringLiteral("squashfs"),
            // macOS has its own bookkeeping mounts, and they are not storage
            // either. Harmless on Linux, which has no filesystem by these names.
            QStringLiteral("devfs"),
            QStringLiteral("map"),
        };
        return types;
    }

    /// "C:/" -> "C:". What a Windows user calls the drive when it has no label.
    QString driveLetterOf(const QString& rootPath)
    {
        QString trimmed = rootPath;
        while (trimmed.endsWith(QLatin1Char('/')) || trimmed.endsWith(QLatin1Char('\\')))
            trimmed.chop(1);
        return trimmed;
    }

} // namespace

/// Network shares are drives wherever they happen to be mounted.
bool SystemVolumes::isNetworkFileSystem(const QString& fileSystemType)
{
    static const QSet<QString> network {
        QStringLiteral("nfs"),
        QStringLiteral("nfs4"),
        QStringLiteral("cifs"),
        QStringLiteral("smbfs"),
        QStringLiteral("smb3"),
        QStringLiteral("sshfs"),
        QStringLiteral("davfs"),
        QStringLiteral("ftpfs"),
        QStringLiteral("afs"),
        QStringLiteral("ceph"),
        QStringLiteral("glusterfs"),
        QStringLiteral("9p"),
    };
    const QString lower = fileSystemType.toLower();
    return network.contains(lower) || lower.startsWith(QLatin1String("fuse."));
}

bool SystemVolumes::isInteresting(
    const QString& rootPath, const QString& fileSystemType, const QString& device, HostPlatform platform)
{
    if (rootPath.isEmpty())
        return false;
    if (pseudoFileSystems().contains(fileSystemType.toLower()))
        return false;

    // Every installed snap is a read-only loopback mount. Dozens of them, none
    // of them anything a person calls a drive. Matches nothing off Linux.
    if (device.startsWith(QLatin1String("/dev/loop")))
        return false;

    // On Windows there is nothing to filter. QStorageInfo reports C:/, D:/ and
    // so on, every one of them a drive, and nothing there mounts a filesystem
    // per package directory -- which is the problem the allowlist below exists
    // to solve and Windows does not have. The old rule dropped every drive on
    // the machine, because none of them is "/" and none begins with /media/.
    if (platform == HostPlatform::Windows)
        return true;

    if (rootPath == QLatin1String("/"))
        return true;

    // A network share is a drive wherever it is mounted.
    if (isNetworkFileSystem(fileSystemType))
        return true;

    // Otherwise: only the conventional places disks get mounted.
    //
    // The alternative -- listing every mount that is not obviously plumbing --
    // falls apart on a ZFS or Btrfs machine, where /var/lib/dpkg, /var/games
    // and /boot/grub are all separate filesystems. They are real mounts and
    // utterly uninteresting, and no blocklist keeps up with them. An allowlist
    // of mount roots does.
    //
    // The conventions differ by system, which is the whole of this fault: all
    // three of the Linux ones are Linux ones, and on macOS everything that is
    // not the boot volume is under /Volumes/ -- so an external disk or a mounted
    // image never appeared at all.
    const QList<QLatin1String> prefixes = platform == HostPlatform::MacOS
        ? QList<QLatin1String> { QLatin1String("/Volumes/") }
        : QList<QLatin1String> { QLatin1String("/media/"), QLatin1String("/run/media/"),
              QLatin1String("/mnt/") };
    for (const QLatin1String prefix : prefixes) {
        if (rootPath.startsWith(prefix) && rootPath.size() > prefix.size())
            return true;
    }

    return false;
}

QString SystemVolumes::displayName(
    const QString& rootPath, const QString& volumeName, const QString& device, HostPlatform platform)
{
    if (platform == HostPlatform::Windows) {
        // The label the disk carries, and the drive letter when it has none.
        // Never "Root": there is no such thing on Windows, and calling C: that
        // would be one row wrong on every machine.
        return volumeName.isEmpty() ? driveLetterOf(rootPath) : volumeName;
    }

    // The disk's own name wins over the generic one. On macOS the boot volume
    // is "/" and is called something -- "Macintosh HD" -- so answering "Root"
    // there threw away the only name the user recognises.
    if (platform == HostPlatform::MacOS && !volumeName.isEmpty())
        return volumeName;

    if (rootPath == QLatin1String("/"))
        return QStringLiteral("Root");

    if (!volumeName.isEmpty())
        return volumeName;

    // Mount points like /media/user/BACKUP or /mnt/data already carry the name
    // the user gave the disk.
    const QString leaf = QFileInfo(rootPath).fileName();
    if (!leaf.isEmpty())
        return leaf;

    const QString deviceLeaf = QFileInfo(device).fileName();
    return deviceLeaf.isEmpty() ? rootPath : deviceLeaf;
}

QList<SystemVolume> SystemVolumes::enumerate()
{
    QList<SystemVolume> out;
    QSet<QString> seenRoots;

    const QList<QStorageInfo> mounted = QStorageInfo::mountedVolumes();
    for (const QStorageInfo& storage : mounted) {
        if (!storage.isValid() || !storage.isReady())
            continue;

        const QString rootPath = storage.rootPath();
        const QString type = QString::fromLatin1(storage.fileSystemType());
        const QString device = QString::fromLatin1(storage.device());

        if (!isInteresting(rootPath, type, device))
            continue;
        // Bind mounts and overlays can report the same root twice.
        if (seenRoots.contains(rootPath))
            continue;
        seenRoots.insert(rootPath);

        SystemVolume volume;
        volume.name = displayName(rootPath, storage.name(), device);
        volume.rootPath = rootPath;
        volume.device = device;
        volume.fileSystemType = type;
        volume.totalBytes = storage.bytesTotal();
        volume.freeBytes = storage.bytesAvailable();
        volume.isReadOnly = storage.isReadOnly();
        volume.isRoot = rootPath == QLatin1String("/");
        out.append(volume);
    }

    // Root first, then alphabetically: the list should not reshuffle itself
    // every time something is plugged in.
    std::sort(out.begin(), out.end(), [](const SystemVolume& a, const SystemVolume& b) {
        if (a.isRoot != b.isRoot)
            return a.isRoot;
        return a.name.localeAwareCompare(b.name) < 0;
    });

    return out;
}

} // namespace mole
