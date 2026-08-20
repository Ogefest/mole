#include "ui/ThumbnailSource.h"

#include "sdk/IThumbnailer.h"
#include "ui/ThumbnailCache.h"

#include "core/tasks/TaskManager.h"

#include <QThread>
#include <QUrlQuery>

namespace mole {

QString ThumbnailKey::toId() const
{
    // Concatenated rather than built with arg(): the encoded uri is full of
    // percent signs and arg() would read %2F as a placeholder and substitute into
    // the middle of the path.
    return QString::fromLatin1(QUrl::toPercentEncoding(uri.toString())) + QStringLiteral("?size=")
        + QString::number(size) + QStringLiteral("&mtime=") + QString::number(mtime)
        + QStringLiteral("&bytes=") + QString::number(bytes);
}

QString ThumbnailKey::urlFor(const VfsUri& uri, int size, qint64 mtime, qint64 bytes)
{
    ThumbnailKey key;
    key.uri = uri;
    key.size = size;
    key.mtime = mtime;
    key.bytes = bytes;
    return QStringLiteral("image://") + providerName() + QLatin1Char('/') + key.toId();
}

ThumbnailKey ThumbnailKey::parse(const QString& id)
{
    ThumbnailKey key;
    const qsizetype split = id.indexOf(QLatin1Char('?'));
    if (split < 0)
        return key;

    // Percent-encoded whole, so nothing in a path -- a colon, a slash, a question
    // mark in a file name -- can be mistaken for part of the url's own structure.
    key.uri = VfsUri::fromString(QUrl::fromPercentEncoding(id.left(split).toUtf8()));

    const QUrlQuery query(id.mid(split + 1));
    key.size = query.queryItemValue(QStringLiteral("size")).toInt();
    key.mtime = query.queryItemValue(QStringLiteral("mtime")).toLongLong();
    key.bytes = query.queryItemValue(QStringLiteral("bytes")).toLongLong();
    return key;
}

// ------------------------------------------------------------------------ task

ThumbnailTask::ThumbnailTask(
    PluginServices services, ThumbnailKey key, ThumbnailCache* cache, QObject* parent)
    : Task(QStringLiteral("Thumbnail of %1").arg(key.uri.fileName()), parent)
    , m_services(services)
    , m_key(std::move(key))
    , m_cache(cache)
{
    // One of a crowd: a folder of three hundred photographs is three hundred of
    // these, and none of them is a job anybody remembers starting. Not background
    // though -- opening that folder is exactly asking for them, so they stay in the
    // task strip's list. See Task::isOneOfMany() and ADR-0064.
    setOneOfMany(true);
    noteTouching(m_key.uri);
}

void ThumbnailTask::run()
{
    m_ranOn = QThread::currentThread();
    if (!m_key.isValid())
        return;

    // The disk tier is asked here rather than before the task, because reading it
    // is I/O and the UI thread never touches storage. The memory tier is asked on
    // the way in, where it costs nothing.
    if (m_cache) {
        if (QImage kept = m_cache->onDisk(m_key); !kept.isNull()) {
            m_image = std::move(kept);
            m_fromCache = true;
            return;
        }
    }
    if (isCancelRequested() || !m_services.thumbnails)
        return;

    // Built from the url rather than from a stat(): a listing already knows a
    // file's name and date, and one extra round trip per tile on a remote drive
    // is the cost this whole feature exists to avoid.
    FileEntry entry;
    entry.uri = m_key.uri;
    entry.name = m_key.uri.fileName();
    entry.size = m_key.bytes;
    if (m_key.mtime > 0)
        entry.modified = QDateTime::fromSecsSinceEpoch(m_key.mtime);

    IThumbnailer* thumbnailer = m_services.thumbnails->thumbnailerFor(entry);
    if (!thumbnailer)
        return; // nothing here can make a picture of this, which is ordinary

    // A thumbnailer that throws costs its own tile and nobody else's -- the same
    // promise every other extension point makes.
    try {
        m_image = thumbnailer->thumbnail(entry, m_key.size, m_services, cancelToken());
    } catch (...) {
        m_image = QImage();
    }
    if (m_image.isNull())
        return;
    m_answeredBy = thumbnailer->id();
    // Written after a successful decode, so the next visit to this folder costs a
    // read rather than a decode -- and a cancelled one is not written, because
    // what it produced is not the whole picture.
    if (m_cache && !isCancelRequested())
        m_cache->store(m_key, m_image);
}

// ------------------------------------------------------------------------ pump

ThumbnailPump::ThumbnailPump(PluginServices services, ThumbnailCache* cache, QObject* parent)
    : QObject(parent)
    , m_services(services)
    , m_cache(cache)
{
}

void ThumbnailPump::startFor(QObject* response, const QString& id)
{
    if (!response)
        return;
    if (m_keyOf.contains(response))
        return; // asked twice for the same response, which is not a second picture

    const ThumbnailKey key = ThumbnailKey::parse(id);
    if (!key.isValid() || !m_services.tasks) {
        deliver(response, QImage());
        return;
    }

    // Already in memory, which is what a scroll back up a folder looks like: the
    // answer without a task, on the spot.
    if (m_cache) {
        if (QImage kept = m_cache->inMemory(key); !kept.isNull()) {
            deliver(response, kept);
            return;
        }
    }

    const QString slot = key.toId();
    m_keyOf.insert(response, slot);

    // Somebody is already making this picture, or it is already in the queue. Two
    // panes showing one folder ask at the same moment, and decoding it twice is
    // twice the work for one answer.
    if (const auto pending = m_pending.find(slot); pending != m_pending.end()) {
        pending->waiting.append(response);
        return;
    }
    if (const auto waiting = m_waitingFor.find(slot); waiting != m_waitingFor.end()) {
        waiting->append(response);
        // Newly asked for again, so it goes back to the front: whatever brought it
        // up a second time is what the viewport is looking at now.
        m_queue.removeAll(slot);
        m_queue.prepend(slot);
        return;
    }

    m_waitingFor.insert(slot, { response });
    m_queue.prepend(slot);
    pumpQueue();
}

int ThumbnailPump::defaultConcurrency()
{
    // The task pool itself is cores - 2, bounded to [2, 8]; taking half of that
    // for pictures leaves the listing, the search and the copy their own room.
    return qBound(1, QThread::idealThreadCount() / 2 - 1, 4);
}

void ThumbnailPump::setConcurrency(int decodes)
{
    m_concurrency = qMax(1, decodes);
    pumpQueue();
}

void ThumbnailPump::pumpQueue()
{
    while (!m_queue.isEmpty() && int(m_pending.size()) < m_concurrency) {
        const QString slot = m_queue.takeFirst();
        const QList<QObject*> waiting = m_waitingFor.take(slot);
        if (waiting.isEmpty())
            continue; // everybody who wanted it has gone

        const ThumbnailKey key = ThumbnailKey::parse(slot);
        auto* task = new ThumbnailTask(m_services, key, m_cache, this);
        Pending fresh;
        fresh.task = task;
        fresh.waiting = waiting;
        m_pending.insert(slot, fresh);

        connect(task, &Task::finished, this, [this, slot, task] {
            // The answer, whatever happened: a cancelled or failed decode is a
            // null image and the tile keeps its icon.
            settle(slot, task->state() == Task::State::Succeeded ? task->image() : QImage());
            pumpQueue();
        });
        m_services.tasks->submit(task);
    }
}

void ThumbnailPump::settle(const QString& slot, const QImage& answer)
{
    const auto pending = m_pending.constFind(slot);
    if (pending == m_pending.constEnd())
        return;
    const QList<QObject*> waiting = pending->waiting;
    m_pending.erase(pending);
    for (QObject* response : waiting) {
        m_keyOf.remove(response);
        deliver(response, answer);
    }
}

void ThumbnailPump::cancelFor(QObject* response)
{
    const auto found = m_keyOf.constFind(response);
    if (found == m_keyOf.constEnd())
        return; // already answered, and the pointer may be gone with it

    const QString slot = *found;
    m_keyOf.erase(found);

    if (const auto pending = m_pending.find(slot); pending != m_pending.end()) {
        pending->waiting.removeAll(response);
        // Only when nobody wants it any more: the other pane showing this folder
        // is still waiting for the same picture.
        if (pending->waiting.isEmpty() && pending->task)
            pending->task->requestCancel();
    } else if (const auto waiting = m_waitingFor.find(slot); waiting != m_waitingFor.end()) {
        // Not started yet, so it costs nothing at all: leaving a folder has to
        // take its queue with it, or walking through five folders leaves five
        // folders' worth of decoding behind the one on screen.
        waiting->removeAll(response);
        if (waiting->isEmpty()) {
            m_waitingFor.erase(waiting);
            m_queue.removeAll(slot);
        }
    }

    // Qt expects a cancelled response to finish rather than to hang, so this one
    // gets its answer now; the running task still delivers to whoever is left.
    deliver(response, QImage());
}

void ThumbnailPump::deliver(QObject* response, const QImage& image)
{
    emit ready(response, image);
}

} // namespace mole
