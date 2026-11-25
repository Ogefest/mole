#pragma once

#include <QString>

namespace mole {

/// What a disk is, as opposed to where it happens to be mounted.
///
/// A removable disk's mount point is not stable: unplug a stick while another
/// is in and plug it back, and the automounter may give it /media/user/BACKUP1
/// rather than /media/user/BACKUP. A name and an identity taken from the mount
/// point made that a second drive, with a new row and a hidden tick that no
/// longer applied. See MOLE-430.
///
/// The mount table names the device and nothing more, but the device is enough.
/// udev keeps /dev/disk/by-uuid/ and /dev/disk/by-label/ as symbolic links to
/// it, so the filesystem's UUID and the label written on the disk are one
/// readlink away. Nothing is mounted, opened or asked of the filesystem itself,
/// which matters for the reason SystemVolumes reads the table as text (MOLE-361).
struct DiskIdentity
{
    /// The filesystem's UUID, as udev spells it. Empty where there is none: a
    /// bind mount, a network share, a filesystem type without one, or a platform
    /// with no /dev/disk.
    QString uuid;
    /// The label the disk carries, decoded. Empty where it has none.
    QString label;
};

/// The identity of `device`, found by following the links under `diskDirectory`
/// (normally /dev/disk) back to it.
///
/// The directory is a parameter so a test can point it at a tree of links it
/// made rather than at this machine's disks. A device that nothing links to
/// gets an empty identity, and the caller falls back to the mount point.
DiskIdentity diskIdentityOf(
    const QString& device, const QString& diskDirectory = QStringLiteral("/dev/disk"));

/// A name from /dev/disk/by-label/ as the disk spells it. udev escapes
/// anything awkward as \xHH, so a label "MY STICK" is linked as "MY\x20STICK".
QString decodeDiskLabel(const QString& linkName);

} // namespace mole
