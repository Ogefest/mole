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

QString SystemVolumeMounts::identityOf(const SystemVolume& volume)
{
    if (!volume.uuid.isEmpty())
        return QStringLiteral("uuid:") + volume.uuid;
    // The fallback: a bind mount, a share, a filesystem with no UUID, a platform
    // with no /dev/disk. Known by where it is, as every drive was before.
    return QStringLiteral("path:") + volume.rootPath;
}

QString SystemVolumeMounts::mountIdOf(const SystemVolume& volume)
{
    return volume.uuid.isEmpty() ? QString() : QStringLiteral("volume:") + volume.uuid;
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
        // One disk mounted in two places, or one place mounted over twice: the
        // later line is the one that counts, and it keeps the first one's
        // position in the list.
        const QString identity = identityOf(volume);
        if (!now.contains(identity))
            ordered.append(identity);
        now.insert(identity, volume);
    }

    // Gone first: a disk swapped for another at the same mount point is the old
    // one going and the new one arriving, in that order. A disk that is still
    // here but at another place is remounted there, under the same id.
    QStringList gone;
    for (auto held = m_mounted.begin(); held != m_mounted.end();) {
        const auto found = now.constFind(held.key());
        const bool same
            = found != now.constEnd() && found->device == held->device && found->rootPath == held->rootPath;
        if (same) {
            ++held;
            continue;
        }
        gone.append(held->rootPath);
        m_vfs->removeMount(held->mountId);
        held = m_mounted.erase(held);
    }

    QStringList appeared;
    for (const QString& identity : std::as_const(ordered)) {
        if (m_mounted.contains(identity))
            continue;
        const SystemVolume volume = now.value(identity);
        FileSystemPtr fs = m_make(volume.rootPath);
        if (!fs)
            continue;
        Mount mount;
        // Stable for a disk with a UUID, so a stick plugged back in is the mount
        // it was -- and a pane that was on it can find it again. Left to the Vfs
        // when something else already holds that id, which a disk mounted twice
        // would otherwise collide on.
        mount.id = mountIdOf(volume);
        if (!mount.id.isEmpty() && !m_vfs->mount(mount.id).id.isEmpty())
            mount.id.clear();
        mount.displayName = volume.name;
        mount.root = VfsUri::fromLocalPath(volume.rootPath);
        mount.fileSystem = std::move(fs);
        const QString id = m_vfs->addMount(std::move(mount));
        if (id.isEmpty())
            continue;
        m_mounted.insert(identity, Held { volume.rootPath, volume.device, id });
        appeared.append(volume.rootPath);
    }

    if (!appeared.isEmpty() || !gone.isEmpty())
        emit changed(appeared, gone);
}

QString SystemVolumeMounts::mountIdFor(const QString& rootPath) const
{
    for (const Held& held : m_mounted) {
        if (held.rootPath == rootPath)
            return held.mountId;
    }
    return {};
}

} // namespace mole
