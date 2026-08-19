#include "ui/ThumbnailSource.h"

#include "sdk/IThumbnailer.h"

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
        + QString::number(size) + QStringLiteral("&mtime=") + QString::number(mtime);
}

QString ThumbnailKey::urlFor(const VfsUri& uri, int size, qint64 mtime)
{
    ThumbnailKey key;
    key.uri = uri;
    key.size = size;
    key.mtime = mtime;
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
    return key;
}

// ------------------------------------------------------------------------ task

ThumbnailTask::ThumbnailTask(PluginServices services, ThumbnailKey key, QObject* parent)
    : Task(QStringLiteral("Thumbnail of %1").arg(key.uri.fileName()), parent)
    , m_services(services)
    , m_key(std::move(key))
{
    noteTouching(m_key.uri);
}

void ThumbnailTask::run()
{
    m_ranOn = QThread::currentThread();
    if (!m_key.isValid() || !m_services.thumbnails)
        return;

    // Built from the url rather than from a stat(): a listing already knows a
    // file's name and date, and one extra round trip per tile on a remote drive
    // is the cost this whole feature exists to avoid.
    FileEntry entry;
    entry.uri = m_key.uri;
    entry.name = m_key.uri.fileName();
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
    if (!m_image.isNull())
        m_answeredBy = thumbnailer->id();
}

// ------------------------------------------------------------------------ pump

ThumbnailPump::ThumbnailPump(PluginServices services, QObject* parent)
    : QObject(parent)
    , m_services(services)
{
}

void ThumbnailPump::startFor(QObject* response, const QString& id)
{
    if (!response)
        return;
    if (m_running.contains(response))
        return; // asked twice for the same response, which is not a second picture

    const ThumbnailKey key = ThumbnailKey::parse(id);
    if (!key.isValid() || !m_services.tasks || !m_services.thumbnails) {
        deliver(response, QImage());
        return;
    }

    auto* task = new ThumbnailTask(m_services, key, this);
    m_running.insert(response, task);
    connect(task, &Task::finished, this, [this, response, task] {
        // The answer, whatever happened: a cancelled or failed decode is a null
        // image and the tile keeps its icon.
        if (!m_running.contains(response))
            return;
        m_running.remove(response);
        deliver(response, task->state() == Task::State::Succeeded ? task->image() : QImage());
    });
    m_services.tasks->submit(task);
}

void ThumbnailPump::cancelFor(QObject* response)
{
    const auto found = m_running.constFind(response);
    if (found == m_running.constEnd())
        return; // already answered, and the pointer may be gone with it

    if (ThumbnailTask* task = found.value())
        task->requestCancel();
    // The task's own finished handler still delivers, because Qt expects a
    // cancelled response to finish rather than to hang.
}

void ThumbnailPump::deliver(QObject* response, const QImage& image)
{
    emit ready(response, image);
}

} // namespace mole
