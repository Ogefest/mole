#pragma once

#include "core/vfs/IFileSystemFactory.h"

#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

#include <memory>

namespace mole {

/// A mount is what the user perceives as a "drive" in the sidebar. It binds a
/// configured backend instance to a root uri and a display name. Local disks
/// and remote shares are the same thing here on purpose -- that is what makes
/// every operation identical regardless of where the bytes live.
struct Mount
{
    QString id;
    QString displayName;
    QString iconName;
    VfsUri root;
    FileSystemPtr fileSystem;

    bool isValid() const { return !id.isEmpty() && fileSystem != nullptr; }
};

/// Owns the backend registry and the mount table. There is exactly one of
/// these per application.
///
/// Thread safety: every public method is safe to call from any thread. Worker
/// threads mostly call resolve(); the UI thread mutates the mount table.
class VfsManager : public QObject
{
    Q_OBJECT

public:
    explicit VfsManager(QObject* parent = nullptr);
    ~VfsManager() override;

    // ---- Backend registry ----------------------------------------------

    void registerFactory(std::unique_ptr<IFileSystemFactory> factory);
    /// Non-owning view, ordered by registration.
    QList<IFileSystemFactory*> factories() const;
    IFileSystemFactory* factoryFor(const QString& scheme) const;

    // ---- Mount table -----------------------------------------------------

    /// Builds a backend from `config` and mounts it. Returns the new mount id,
    /// or an empty string with `errorOut` filled in on failure.
    QString addMount(const QString& scheme, const QString& displayName, const QVariantMap& config,
        QString* errorOut = nullptr);

    /// Mounts an already-built backend (used for the always-present local disk).
    QString addMount(Mount mount);

    void removeMount(const QString& id);

    QList<Mount> mounts() const;
    Mount mount(const QString& id) const;

    /// Finds the backend that owns `uri`. Null when nothing is mounted for it.
    FileSystemPtr resolve(const VfsUri& uri) const;
    /// The mount `uri` belongs to; invalid Mount when there is none.
    Mount mountForUri(const VfsUri& uri) const;

signals:
    void mountAdded(const QString& id);
    void mountRemoved(const QString& id);
    void mountsChanged();

private:
    QString allocateId(const QString& scheme);

    mutable QMutex m_mutex;
    std::vector<std::unique_ptr<IFileSystemFactory>> m_factories;
    QList<Mount> m_mounts;
    int m_nextId = 1;
};

} // namespace mole
