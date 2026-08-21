#pragma once

#include "core/platform/HostPlatform.h"

#include <QList>
#include <QString>

namespace mole {

/// One filesystem the operating system currently has mounted.
struct SystemVolume
{
    QString name; ///< label, or something readable derived from the path
    QString rootPath; ///< native mount point
    QString device; ///< e.g. /dev/sda2
    QString fileSystemType;
    qint64 totalBytes = 0;
    qint64 freeBytes = 0;
    bool isReadOnly = false;
    /// True for the volume the user's home directory lives on.
    bool isRoot = false;
};

/// Lists what is actually mounted, filtered down to what a person would call a
/// drive.
///
/// A modern Linux box mounts sixty-odd pseudo filesystems -- cgroups, tmpfs
/// under /run, snap loopbacks, one per installed package. Listing them all is
/// technically accurate and completely useless, so this filters to the ones a
/// user would recognise as somewhere their files might be.
class SystemVolumes
{
public:
    static QList<SystemVolume> enumerate();

    /// Whether a mount point is worth showing. Exposed so the rule itself can
    /// be tested without needing a machine that has such a mount.
    ///
    /// `platform` is an argument rather than an `#ifdef` for the same reason it
    /// is one in VfsUri: the suite runs on Linux, so a compile-time switch puts
    /// the Windows and macOS answers in branches no test has ever entered --
    /// which is how this came to return nothing at all on Windows and one row
    /// called "Root" on macOS. See ADR-0068.
    static bool isInteresting(const QString& rootPath, const QString& fileSystemType, const QString& device,
        HostPlatform platform = hostPlatform());

    static bool isNetworkFileSystem(const QString& fileSystemType);

    /// "Root", "Data (sdb1)", "usb-stick", "C:" -- never an empty string, and
    /// never "Root" for something that is not the system's own root.
    static QString displayName(const QString& rootPath, const QString& volumeName, const QString& device,
        HostPlatform platform = hostPlatform());
};

} // namespace mole
