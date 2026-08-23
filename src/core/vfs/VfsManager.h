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
    /// A place somebody is standing in that the sidebar does not offer.
    ///
    /// **A third thing, and not `internal`.** An archive opened for browsing is
    /// somewhere a person walks around in -- so it is not internal, whose contract
    /// is *not a place anybody can go* -- and it is not a drive either: nobody
    /// asked for a row per zip they looked inside, and MOLE-301 made that a row
    /// per .jar, .deb, .rpm and .whl as well.
    ///
    /// Unlisted rather than hidden, because the difference is the lifetime: this
    /// mount **goes away when nobody is inside it**, which is what stops forty
    /// invisible mounts each holding a file handle and a cached central directory
    /// with no way to eject one. AppController::refreshOpenDrives() is where that
    /// is decided, at the moments the answer can change.
    ///
    /// It is reused rather than duplicated, unlike an internal mount: opening the
    /// same archive twice is one mount, and the preview's own mount of a single
    /// compressed member is still skipped by that. See MOLE-310 and ADR-0083.
    bool unlisted = false;
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

    /// Rebuilds the mount `uri` needs, when the uri says enough to do it.
    ///
    /// Returns the mount's id, the id of the mount that was already there, or an
    /// empty string when nothing can be rebuilt -- which is the ordinary answer
    /// for every backend except one whose root carries its own address. See
    /// IFileSystemFactory::configForRoot() and Mount::unlisted.
    ///
    /// **On the thread that owns the mount table, like every other mutation
    /// here.** A worker thread that finds nothing behind a uri has found the
    /// ordinary error for a file that is not there; it is navigation that
    /// rebuilds, and navigation happens where the window is.
    QString remountFor(const VfsUri& uri);

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
