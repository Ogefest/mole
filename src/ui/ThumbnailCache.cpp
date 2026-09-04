#include "ui/ThumbnailCache.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QStandardPaths>

#include <algorithm>

namespace mole {
namespace {

    /// What a thumbnail costs in memory, which is what the cap is in.
    qint64 bytesOf(const QImage& image)
    {
        return image.isNull() ? 0 : qint64(image.sizeInBytes());
    }

    /// The key a cache file records, so an entry can be explained rather than
    /// being an unreadable name in a directory.
    QString uriTextKey()
    {
        return QStringLiteral("mole.uri");
    }

} // namespace

QString ThumbnailCache::defaultDirectory()
{
    const QByteArray override = qgetenv("MOLE_THUMBNAILS_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    // CacheLocation, not AppDataLocation: this is the first store here that holds
    // something recomputable, and deleting it must cost nothing but time.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(dir).filePath(QStringLiteral("thumbnails"));
}

ThumbnailCache::ThumbnailCache(QString directory, qint64 memoryCap, qint64 diskCap)
    : m_directory(std::move(directory))
    , m_memoryCap(qMax<qint64>(0, memoryCap))
    , m_diskCap(qMax<qint64>(0, diskCap))
{
}

QString ThumbnailCache::fileNameFor(const ThumbnailKey& key)
{
    // The uri, the size and the date together: the date is what makes an edited
    // file produce a new picture rather than the one from before.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(key.uri.toString().toUtf8());
    hash.addData(QByteArrayLiteral("\0"));
    hash.addData(QByteArray::number(key.size));
    hash.addData(QByteArrayLiteral("\0"));
    hash.addData(QByteArray::number(key.mtime));
    // And the size, when the drive said one. **A date on its own is not enough
    // to tell two files apart.** `cp -p`, `rsync -a`, a tar extract and a photo
    // re-exported over itself by a tool that keeps the mtime all replace a file
    // and preserve its date, and two edits inside one second are
    // indistinguishable by seconds -- so the old picture stayed until eviction.
    // ADR-0059 chose the date over a content hash, which is right; it did not
    // discuss the size, which is free and already on the key. Left out when it
    // is zero, so a caller that has no size hint still finds what a caller with
    // one stored. See MOLE-385.
    if (key.bytes > 0) {
        hash.addData(QByteArrayLiteral("\0"));
        hash.addData(QByteArray::number(key.bytes));
    }
    // Half the digest is 128 bits, which is a name nobody will collide and a
    // directory listing a person can still read.
    return QString::fromLatin1(hash.result().toHex().left(32));
}

QImage ThumbnailCache::inMemory(const ThumbnailKey& key)
{
    if (!key.isValid())
        return {};
    const QString name = fileNameFor(key);

    QMutexLocker lock(&m_mutex);
    const auto found = m_memory.constFind(name);
    if (found == m_memory.constEnd())
        return {};
    m_memoryOrder.removeAll(name);
    m_memoryOrder.append(name);
    return *found;
}

QImage ThumbnailCache::onDisk(const ThumbnailKey& key)
{
    if (!key.isValid() || m_diskCap <= 0)
        return {};
    const QString name = fileNameFor(key);
    const QString path = QDir(m_directory).filePath(name);

    QImageReader reader(path);
    const QImage image = reader.read();
    if (image.isNull()) {
        // A truncated or corrupt entry is a miss, not a crash and not a broken
        // tile: the next store overwrites it.
        QMutexLocker lock(&m_mutex);
        if (const auto stale = m_disk.constFind(name); stale != m_disk.constEnd()) {
            m_diskBytes = qMax<qint64>(0, m_diskBytes - stale->bytes);
            m_disk.erase(stale);
        }
        return {};
    }

    QMutexLocker lock(&m_mutex);
    // Stamped on read, because eviction is least recently *read* first.
    const QDateTime now = QDateTime::currentDateTime();
    if (QFile file(path); file.open(QIODevice::ReadOnly))
        file.setFileTime(now, QFileDevice::FileModificationTime);
    if (const auto entry = m_disk.find(name); entry != m_disk.end())
        entry->readAt = now.toSecsSinceEpoch();
    rememberInMemory(name, image);
    return image;
}

void ThumbnailCache::store(const ThumbnailKey& key, const QImage& image)
{
    if (!key.isValid() || image.isNull())
        return;
    const QString name = fileNameFor(key);

    {
        QMutexLocker lock(&m_mutex);
        rememberInMemory(name, image);
    }
    if (m_diskCap <= 0)
        return;

    QDir directory(m_directory);
    if (!directory.exists() && !QDir().mkpath(m_directory))
        return; // a cache that cannot be written is a cache that is not used

    // JPEG where there is nothing to keep, PNG where there is: a cache costing
    // 200 kB an entry is a cache nobody wants, and a transparent thumbnail
    // flattened onto black is a wrong picture rather than a smaller one.
    const bool transparent = image.hasAlphaChannel();
    QByteArray encoded;
    {
        QBuffer buffer(&encoded);
        if (!buffer.open(QIODevice::WriteOnly))
            return;
        QImageWriter writer(&buffer, transparent ? QByteArrayLiteral("png") : QByteArrayLiteral("jpeg"));
        if (!transparent)
            writer.setQuality(82);
        // So a file in this directory can be explained rather than being a hash.
        writer.setText(uriTextKey(), key.uri.toString());
        if (!writer.write(image))
            return;
    }

    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    if (file.write(encoded) != encoded.size()) {
        file.close();
        QFile::remove(path); // a half-written entry would read back as a miss for ever
        return;
    }
    file.close();

    QMutexLocker lock(&m_mutex);
    measureDirectory();
    DiskEntry entry;
    entry.bytes = encoded.size();
    entry.readAt = QDateTime::currentSecsSinceEpoch();
    if (const auto previous = m_disk.constFind(name); previous != m_disk.constEnd())
        m_diskBytes -= previous->bytes;
    m_disk.insert(name, entry);
    m_diskBytes += entry.bytes;
    // Our own write moved the directory's mtime, and it is accounted for here --
    // so the reconciliation above is about somebody else's writes and not ours.
    m_measuredMtime = QFileInfo(m_directory).lastModified().toSecsSinceEpoch();
    evictWhileOverCap();
}

void ThumbnailCache::measureDirectory()
{
    const QDir directory(m_directory);
    // **Measured again when somebody else has written here.** ADR-0059 says
    // "one cache per window", and two windows share one directory: each
    // accounted only its own writes, so the 256 MB cap was per instance and
    // diskBytes() was a guess that only ever grew. The directory's own mtime
    // moves whenever an entry is added or removed, which is the cheap signal
    // for it -- and the reconciliation is rate-limited, because our own writes
    // move it too and a full re-read per stored tile would cost more than the
    // tile. See MOLE-385.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 changedAt = QFileInfo(m_directory).lastModified().toSecsSinceEpoch();
    const bool somebodyElseWrote
        = m_measured && changedAt != m_measuredMtime && now - m_measuredAt >= kReconcileSeconds;
    if (m_measured && !somebodyElseWrote)
        return;

    m_measured = true;
    m_measuredAt = now;
    m_measuredMtime = changedAt;
    m_disk.clear();
    m_diskBytes = 0;

    if (!directory.exists())
        return;
    const QFileInfoList files = directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& info : files) {
        DiskEntry entry;
        entry.bytes = info.size();
        entry.readAt = info.lastModified().toSecsSinceEpoch();
        m_disk.insert(info.fileName(), entry);
        m_diskBytes += entry.bytes;
    }
}

void ThumbnailCache::evictWhileOverCap()
{
    if (m_diskBytes <= m_diskCap)
        return;

    QStringList byAge = m_disk.keys();
    std::sort(byAge.begin(), byAge.end(), [this](const QString& a, const QString& b) {
        return m_disk.value(a).readAt < m_disk.value(b).readAt;
    });
    for (const QString& name : byAge) {
        if (m_diskBytes <= m_diskCap)
            break;
        const DiskEntry entry = m_disk.take(name);
        m_diskBytes = qMax<qint64>(0, m_diskBytes - entry.bytes);
        QFile::remove(QDir(m_directory).filePath(name));
    }
}

void ThumbnailCache::rememberInMemory(const QString& name, const QImage& image)
{
    const qint64 cost = bytesOf(image);
    if (m_memoryCap <= 0 || cost > m_memoryCap)
        return; // one picture that will not fit is not worth emptying the tier for

    if (const auto previous = m_memory.constFind(name); previous != m_memory.constEnd())
        m_memoryBytes -= bytesOf(*previous);
    m_memory.insert(name, image);
    m_memoryOrder.removeAll(name);
    m_memoryOrder.append(name);
    m_memoryBytes += cost;

    while (m_memoryBytes > m_memoryCap && !m_memoryOrder.isEmpty()) {
        const QString oldest = m_memoryOrder.takeFirst();
        m_memoryBytes -= bytesOf(m_memory.take(oldest));
    }
    m_memoryBytes = qMax<qint64>(0, m_memoryBytes);
}

qint64 ThumbnailCache::memoryBytes() const
{
    QMutexLocker lock(&m_mutex);
    return m_memoryBytes;
}

qint64 ThumbnailCache::diskBytes()
{
    QMutexLocker lock(&m_mutex);
    measureDirectory();
    return m_diskBytes;
}

void ThumbnailCache::clear()
{
    QMutexLocker lock(&m_mutex);
    m_memory.clear();
    m_memoryOrder.clear();
    m_memoryBytes = 0;
    for (auto entry = m_disk.constBegin(); entry != m_disk.constEnd(); ++entry)
        QFile::remove(QDir(m_directory).filePath(entry.key()));
    m_disk.clear();
    m_diskBytes = 0;
    m_measured = false;
}

} // namespace mole
