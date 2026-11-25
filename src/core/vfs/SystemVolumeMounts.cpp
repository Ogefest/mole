#include "core/vfs/SystemVolumeMounts.h"

#include "core/vfs/SystemVolumes.h"
#include "core/vfs/VfsManager.h"

#include <QSet>

namespace mole {

SystemVolumeMounts::SystemVolumeMounts(
    VfsManager* vfs, MakeFileSystem make, QString homePath, QObject* parent)
    : QObject(parent)
    , m_vfs(vfs)
    , m_make(std::move(make))
    , m_home(std::move(homePath))
{
}

void SystemVolumeMounts::sync()
{
    if (!m_vfs || !m_make)
        return;

    // In enumerate()'s order, which is the order the sidebar shows: the system's
    // own volume first.
    QStringList ordered;
    QHash<QString, SystemVolume> now;
    for (const SystemVolume& volume : SystemVolumes::enumerate()) {
        // The volume carrying home is the same place as the Home row, so it gets
        // one row, under the name people look for.
        if (volume.rootPath.isEmpty() || volume.rootPath == m_home)
            continue;
        // A place mounted over twice is listed twice, and the later line is the
        // one on top, so it is the one that counts. It keeps the first one's
        // position.
        if (!now.contains(volume.rootPath))
            ordered.append(volume.rootPath);
        now.insert(volume.rootPath, volume);
    }

    // Gone first: a disk swapped for another at the same mount point is the old
    // one going and the new one arriving, in that order.
    QStringList gone;
    for (auto held = m_mounted.begin(); held != m_mounted.end();) {
        const auto found = now.constFind(held.key());
        const bool same = found != now.constEnd() && found->device == held->device;
        if (same) {
            ++held;
            continue;
        }
        gone.append(held.key());
        m_vfs->removeMount(held->mountId);
        held = m_mounted.erase(held);
    }

    QStringList appeared;
    for (const QString& rootPath : std::as_const(ordered)) {
        if (m_mounted.contains(rootPath))
            continue;
        const SystemVolume volume = now.value(rootPath);
        FileSystemPtr fs = m_make(volume.rootPath);
        if (!fs)
            continue;
        Mount mount;
        mount.displayName = volume.name;
        mount.root = VfsUri::fromLocalPath(volume.rootPath);
        mount.fileSystem = std::move(fs);
        const QString id = m_vfs->addMount(std::move(mount));
        if (id.isEmpty())
            continue;
        m_mounted.insert(volume.rootPath, Held { volume.device, id });
        appeared.append(volume.rootPath);
    }

    if (!appeared.isEmpty() || !gone.isEmpty())
        emit changed(appeared, gone);
}

QString SystemVolumeMounts::mountIdFor(const QString& rootPath) const
{
    return m_mounted.value(rootPath).mountId;
}

} // namespace mole
