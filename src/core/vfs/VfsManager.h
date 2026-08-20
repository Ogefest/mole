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
    /// A mount that exists so something can be *read*, not a place anybody can
    /// go. It is left out of the sidebar and out of anything that offers a drive
    /// to the user, and whoever created it removes it again.
    ///
    /// The preview tab is what needed it: a file compressed on its own is a
    /// wrapper around one member, and showing the member means resolving a uri
    /// inside the wrapper -- which needs a mount, because that is how every
    /// reader in Mole reaches a file. Pressing F3 along a folder of two hundred
    /// `.gz` files must not leave two hundred drives behind, or flash one into
    /// the sidebar for each. See MOLE-219.
    bool internal = false;
    /// The size this mount reports, instead of the one measured underneath it.
    ///
    /// Invalid by default, which means "ask the backend" -- the ordinary case. It
    /// is set for a drive configured through `MOLE_DRIVES`, so a window
    /// photographed for the user guide shows a plausible fixed capacity rather
    /// than whatever the machine that took the picture happens to have free. The
    /// names were already fixed for that reason; the figures were not, and they
    /// moved between one regeneration and the next, which is what made
    /// `make guide-images` rewrite most of the guide with nothing changed.
    /// See MOLE-255 and mountDefaultDrives().
    SpaceInfo declaredSpace;

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
