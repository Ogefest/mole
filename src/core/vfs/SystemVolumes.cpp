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
        };
        return types;
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
    const QString& rootPath, const QString& fileSystemType, const QString& device)
{
    if (rootPath.isEmpty())
        return false;
    if (pseudoFileSystems().contains(fileSystemType.toLower()))
        return false;

    // Every installed snap is a read-only loopback mount. Dozens of them, none
    // of them anything a person calls a drive.
    if (device.startsWith(QLatin1String("/dev/loop")))
        return false;

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
    for (const QLatin1String prefix :
        { QLatin1String("/media/"), QLatin1String("/run/media/"), QLatin1String("/mnt/") }) {
        if (rootPath.startsWith(prefix) && rootPath.size() > prefix.size())
            return true;
    }

    return false;
}

QString SystemVolumes::displayName(const QString& rootPath, const QString& volumeName, const QString& device)
{
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
