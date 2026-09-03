#include "core/events/EventBus.h"

#include <QThread>
#include <QVariantMap>

namespace mole {

EventBus::EventBus(QObject* parent)
    : QObject(parent)
{
}

EventBus::~EventBus() = default;

void EventBus::dispatch(std::function<void()> fn)
{
    if (QThread::currentThread() == thread()) {
        // Same thread: deliver synchronously so a UI-initiated event does not
        // arrive a frame late.
        fn();
        return;
    }
    QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
}

void EventBus::postMountsChanged()
{
    dispatch([this] { emit mountsChanged(); });
}

void EventBus::postDriveNeeded(const VfsUri& target)
{
    dispatch([this, target] { emit driveNeeded(target); });
}

void EventBus::postDirectoryChanged(const VfsUri& directory)
{
    dispatch([this, directory] { emit directoryChanged(directory); });
}

void EventBus::postEntryCreated(const VfsUri& entry)
{
    dispatch([this, entry] {
        emit entryCreated(entry);
        emit directoryChanged(entry.parent());
    });
}

void EventBus::postEntryRemoved(const VfsUri& entry)
{
    dispatch([this, entry] {
        emit entryRemoved(entry);
        emit directoryChanged(entry.parent());
    });
}

void EventBus::postEntryRenamed(const VfsUri& from, const VfsUri& to)
{
    dispatch([this, from, to] {
        emit entryRenamed(from, to);
        emit directoryChanged(from.parent());
        if (to.parent() != from.parent())
            emit directoryChanged(to.parent());
    });
}

void EventBus::postIndexUpdated(qint64 volumeId, qint64 entryCount)
{
    dispatch([this, volumeId, entryCount] { emit indexUpdated(volumeId, entryCount); });
}

void EventBus::postOperationFailed(const VfsUri& target, const VfsError& error)
{
    dispatch([this, target, error] { emit operationFailed(target, error); });
}

void EventBus::postNotification(Severity severity, const QString& title, const QString& detail)
{
    dispatch([this, severity, title, detail] { emit notificationPosted(severity, title, detail); });
}

void EventBus::postCustom(const QString& topic, const QVariantMap& payload)
{
    dispatch([this, topic, payload] { emit customEvent(topic, payload); });
}

} // namespace mole
