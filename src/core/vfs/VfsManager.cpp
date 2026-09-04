#include "core/vfs/VfsManager.h"

#include "core/diagnostics/LoggingFileSystem.h"
#include "core/vfs/VersionGuard.h"

#include <QMutexLocker>

namespace mole {

VfsManager::VfsManager(QObject* parent)
    : QObject(parent)
{
}

VfsManager::~VfsManager() = default;

void VfsManager::registerFactory(std::unique_ptr<IFileSystemFactory> factory)
{
    if (!factory)
        return;
    QMutexLocker lock(&m_mutex);
    m_factories.push_back(std::move(factory));
}

QList<IFileSystemFactory*> VfsManager::factories() const
{
    QMutexLocker lock(&m_mutex);
    QList<IFileSystemFactory*> out;
    out.reserve(static_cast<qsizetype>(m_factories.size()));
    for (const auto& f : m_factories)
        out.append(f.get());
    return out;
}

IFileSystemFactory* VfsManager::factoryFor(const QString& scheme) const
{
    QMutexLocker lock(&m_mutex);
    for (const auto& f : m_factories) {
        if (f->scheme() == scheme)
            return f.get();
    }
    return nullptr;
}

QString VfsManager::allocateId(const QString& scheme)
{
    return QStringLiteral("%1-%2").arg(scheme).arg(m_nextId++);
}

QString VfsManager::addMount(
    const QString& scheme, const QString& displayName, const QVariantMap& config, QString* errorOut)
{
    IFileSystemFactory* factory = factoryFor(scheme);
    if (!factory) {
        if (errorOut)
            *errorOut = QStringLiteral("No backend registered for scheme '%1'").arg(scheme);
        return {};
    }

    QString error;
    FileSystemPtr fs = factory->create(config, &error);
    if (!fs) {
        if (errorOut)
            *errorOut = error.isEmpty() ? QStringLiteral("Backend rejected the configuration") : error;
        return {};
    }

    Mount m;
    m.displayName = displayName.isEmpty() ? factory->displayName() : displayName;
    m.iconName = factory->iconName();
    m.fileSystem = std::move(fs);
    m.root = VfsUri(scheme, config.value(QStringLiteral("authority")).toString(),
        config.value(QStringLiteral("rootPath"), QStringLiteral("/")).toString());

    return addMount(std::move(m));
}

QString VfsManager::addMount(Mount mount)
{
    if (!mount.fileSystem)
        return {};

    // Every drive gets the same wrapper on the way in, whatever built it and
    // whichever overload it arrived through -- that is what makes the log the
    // same for local disk, for a plugin's backend, and for one written later.
    // Two wrappers, innermost first. The guard refuses a uri naming a version of
    // a file to a drive that does not know what one is; the log sits outside it
    // so a refusal is a line like any other call. See ADR-0077 and ADR-0064.
    mount.fileSystem = withLogging(withVersionGuard(std::move(mount.fileSystem)), mount.displayName);

    // The glyph, from the factory that serves this scheme, for a mount that
    // arrived without one. Only the two overloads that build a backend
    // themselves used to set it, and every other route -- a configured drive
    // being connected, a test's in-memory drive, a plugin's own mount -- left it
    // empty. DriveListModel has an iconText role that was therefore always
    // blank, so the command palette's drive rows had no glyph while
    // IFileSystemFactory::iconName() sat there to supply one. Filled here rather
    // than at each call site, which is how it came to be missed. See MOLE-395.
    if (mount.iconName.isEmpty()) {
        for (IFileSystemFactory* factory : factories()) {
            if (factory && factory->scheme() == mount.root.scheme()) {
                mount.iconName = factory->iconName();
                break;
            }
        }
    }

    QString id;
    {
        QMutexLocker lock(&m_mutex);
        if (mount.id.isEmpty())
            mount.id = allocateId(mount.root.scheme());
        id = mount.id;
        m_mounts.append(std::move(mount));
    }

    emit mountAdded(id);
    emit mountsChanged();
    return id;
}

QString VfsManager::remountFor(const VfsUri& uri)
{
    if (!uri.isValid())
        return {};
    // Already there is the ordinary case, and saying which is more useful than
    // saying nothing: a caller that wanted a mount now has one either way.
    //
    // **Under the lock, and matched on the whole root.** This read the mount
    // table with no lock while every other accessor takes one, and it compared
    // the scheme and the authority alone -- so two SFTP drives on one host at
    // different roots made each other's uris answer "already mounted", and the
    // caller then handed that uri to resolve(), which refuses it. See MOLE-359.
    {
        QMutexLocker lock(&m_mutex);
        for (const Mount& mount : m_mounts) {
            if (uri.isWithin(mount.root))
                return mount.id;
        }
    }

    IFileSystemFactory* factory = factoryFor(uri.scheme());
    if (!factory)
        return {};

    const VfsUri root(uri.scheme(), uri.authority(), QStringLiteral("/"));
    const QVariantMap config = factory->configForRoot(root);
    if (config.isEmpty())
        return {};

    QString error;
    FileSystemPtr inside = factory->create(config, &error);
    if (!inside)
        return {};

    Mount mount;
    // Named after the file rather than the whole path: a breadcrumb and a title
    // want "reports.zip", and the path is in the uri for anything that needs it.
    mount.displayName = root.fileName().isEmpty() ? uri.fileName() : root.fileName();
    mount.iconName = factory->iconName();
    mount.root = root;
    mount.fileSystem = std::move(inside);
    // Rebuilt to be stood in, and it goes away again the same way the first one
    // did -- see Mount::unlisted.
    mount.unlisted = true;
    return addMount(std::move(mount));
}

QStringList VfsManager::reapUnlistedMounts(const QList<VfsUri>& openLocations)
{
    QStringList gone;
    {
        QMutexLocker lock(&m_mutex);
        for (Mount& mount : m_mounts) {
            if (!mount.unlisted)
                continue;
            bool inUse = false;
            for (const VfsUri& where : openLocations) {
                if (where.scheme() == mount.root.scheme() && where.authority() == mount.root.authority()) {
                    inUse = true;
                    break;
                }
            }
            if (inUse) {
                mount.wasEntered = true;
                continue;
            }
            if (mount.wasEntered)
                gone.append(mount.id);
        }
    }

    // Collected first, and removed outside the lock: removing a mount emits
    // mountsChanged, and something reacting to that while this walks the list is
    // how a walk over a container ends up reading a container that has moved.
    for (const QString& id : gone)
        removeMount(id);
    return gone;
}

void VfsManager::removeMount(const QString& id)
{
    bool removed = false;
    {
        QMutexLocker lock(&m_mutex);
        for (qsizetype i = 0; i < m_mounts.size(); ++i) {
            if (m_mounts.at(i).id == id) {
                m_mounts.removeAt(i);
                removed = true;
                break;
            }
        }
    }

    if (removed) {
        emit mountRemoved(id);
        emit mountsChanged();
    }
}

QList<Mount> VfsManager::mounts() const
{
    QMutexLocker lock(&m_mutex);
    return m_mounts;
}

Mount VfsManager::mount(const QString& id) const
{
    QMutexLocker lock(&m_mutex);
    for (const Mount& m : m_mounts) {
        if (m.id == id)
            return m;
    }
    return {};
}

Mount VfsManager::mountForUri(const VfsUri& uri) const
{
    QMutexLocker lock(&m_mutex);

    // Longest matching root wins, so nesting a bucket inside a broader mount
    // resolves to the more specific one.
    const Mount* best = nullptr;
    for (const Mount& m : m_mounts) {
        if (!uri.isWithin(m.root))
            continue;
        if (!best || m.root.path().size() > best->root.path().size())
            best = &m;
    }
    return best ? *best : Mount {};
}

FileSystemPtr VfsManager::resolve(const VfsUri& uri) const
{
    return mountForUri(uri).fileSystem;
}

} // namespace mole
