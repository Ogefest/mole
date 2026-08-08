#pragma once

#include "core/vfs/VfsUri.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

namespace mole {

/// The application's nervous system.
///
/// Anything that happens -- a drive appears, a directory changes on disk, a
/// scan finishes, an operation fails -- is announced here, and whoever cares
/// reacts. Without it, every new feature would need direct wiring into every
/// other one; with it, a plugin can watch for events it was never introduced
/// to.
///
/// THREADING
/// ---------
/// The post*() methods are safe to call from any thread: each marshals onto
/// the bus's own thread before emitting, so subscribers always receive events
/// on the UI thread and never have to think about locking. Never emit the
/// signals directly -- always go through post*().
class EventBus : public QObject
{
    Q_OBJECT

public:
    enum class Severity { Info, Warning, Error };
    Q_ENUM(Severity)

    explicit EventBus(QObject* parent = nullptr);
    ~EventBus() override;

    // ---- publishing (any thread) ----------------------------------------

    /// The set of mounted drives changed.
    void postMountsChanged();
    /// The contents of `directory` changed and open views should refresh.
    void postDirectoryChanged(const VfsUri& directory);
    /// A file or directory was created, removed or renamed.
    void postEntryCreated(const VfsUri& entry);
    void postEntryRemoved(const VfsUri& entry);
    void postEntryRenamed(const VfsUri& from, const VfsUri& to);
    /// A scan finished writing to the index.
    void postIndexUpdated(qint64 volumeId, qint64 entryCount);
    /// Something worth telling the user about.
    void postNotification(Severity severity, const QString& title, const QString& detail = {});
    /// A plugin-defined event. `topic` should be namespaced, e.g.
    /// "org.example.gitlab/branchChanged", so plugins do not collide.
    void postCustom(const QString& topic, const QVariantMap& payload = {});

signals:
    void mountsChanged();
    void directoryChanged(const mole::VfsUri& directory);
    void entryCreated(const mole::VfsUri& entry);
    void entryRemoved(const mole::VfsUri& entry);
    void entryRenamed(const mole::VfsUri& from, const mole::VfsUri& to);
    void indexUpdated(qint64 volumeId, qint64 entryCount);
    void notificationPosted(mole::EventBus::Severity severity, const QString& title, const QString& detail);
    void customEvent(const QString& topic, const QVariantMap& payload);

private:
    /// Runs `fn` on the bus's thread, immediately if we are already on it.
    void dispatch(std::function<void()> fn);
};

} // namespace mole
