#pragma once

#include "core/vfs/IFileSystem.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

namespace mole {

class VfsManager;

/// Keeps the drives the operating system has mounted in step with the mounts the
/// Vfs holds for them.
///
/// What is mounted used to be asked once, at startup, and never again, so a
/// stick plugged in afterwards could only be reached by typing its path.
/// MountTableWatch says when the table changed. This is what acts on it: it asks
/// SystemVolumes::enumerate() again, compares the answer with what it mounted
/// last time, and mounts what appeared and unmounts what went. See MOLE-429.
///
/// **Only the difference.** A drive that did not change keeps its mount, the
/// same object under the same id, because a pane standing on it holds a uri
/// into that mount. The drive list reloads on the Vfs's mountsChanged and asks
/// how full a drive is only for mounts it has no figure for (MOLE-362), so a
/// change asks about the new drive and nothing else.
///
/// **The filter is SystemVolumes', not this class's.** What enumerate() returns
/// is already the list a person would call drives, so a snap package's loopback
/// mount appearing produces no row here because it never reaches here.
///
/// It touches only the mounts it made. Home, a configured drive, an archive
/// somebody is inside and a fixed MOLE_DRIVES list are none of its business.
class SystemVolumeMounts : public QObject
{
    Q_OBJECT

public:
    /// Builds the filesystem for a newly appeared drive, given its mount point.
    using MakeFileSystem = std::function<FileSystemPtr(const QString& rootPath)>;

    /// `homePath` is mounted separately, under its own name, so the volume
    /// carrying it is skipped here rather than listed twice.
    SystemVolumeMounts(VfsManager* vfs, MakeFileSystem make, QString homePath, QObject* parent = nullptr);

    /// Reads what is mounted and brings the Vfs into line with it. The first call
    /// mounts everything; every later one mounts and unmounts the difference.
    void sync();

    /// The mount this has made for `rootPath`, or an empty string.
    QString mountIdFor(const QString& rootPath) const;
    /// How many drives this currently holds a mount for.
    int count() const { return int(m_mounted.size()); }

signals:
    /// After a sync that changed something: the mount points that appeared and
    /// the ones that went.
    void changed(const QStringList& appeared, const QStringList& gone);

private:
    struct Held
    {
        QString device;
        QString mountId;
    };

    VfsManager* m_vfs = nullptr;
    MakeFileSystem m_make;
    QString m_home;
    /// Mount point to what is mounted there. Keyed on the place and checked
    /// against the device, so another disk mounted at the same place is a
    /// different drive rather than the same one.
    QHash<QString, Held> m_mounted;
};

} // namespace mole
