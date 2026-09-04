#pragma once

#include "ui/ThumbnailSource.h"

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QString>

namespace mole {

/// Thumbnails, kept so the same picture is not decoded twice.
///
/// Two tiers, because they answer two different questions.
///
/// **In memory, for the scroll.** A `GridView` destroys a delegate that leaves
/// its cache buffer and builds a new one when it comes back, so without this the
/// second look at a picture costs exactly what the first one did -- and on a
/// folder of five hundred that is the difference between a view that feels
/// instant and one that flickers grey squares for ever. Capped in **bytes**
/// rather than in count, because a folder of 4K panoramas and a folder of icons
/// must not have wildly different footprints.
///
/// **On disk, for the next visit.** Encoded thumbnails in a directory, written
/// after a successful decode and read before one is attempted.
///
/// The disk tier lives under `QStandardPaths::CacheLocation` and not
/// `AppDataLocation`, which is where the other seven stores are. Every one of
/// those holds something that cannot be recomputed; this is the first that can,
/// and deleting it must cost nothing but time.
///
/// See docs/adr/0059-thumbnails-are-cached-in-two-tiers.md.
class ThumbnailCache
{
public:
    /// Where the disk tier lives. `MOLE_THUMBNAILS_PATH` overrides it, exactly as
    /// the other stores' variables do -- a test that writes into the real cache
    /// directory is a test that changes the machine it runs on.
    static QString defaultDirectory();

    /// A few tens of megabytes buys a folder of several hundred tiles held across
    /// a scroll to the bottom and back, which is the gesture this exists for.
    static constexpr qint64 kDefaultMemoryCap = 32 * 1024 * 1024;
    /// And a quarter of a gigabyte on disk holds a few thousand tiles: enough for
    /// the handful of folders somebody actually lives in, and small enough that
    /// nobody finds it by wondering where their disk went. A cache with no ceiling
    /// is a bug report in six months.
    static constexpr qint64 kDefaultDiskCap = 256 * 1024 * 1024;

    explicit ThumbnailCache(QString directory = defaultDirectory(), qint64 memoryCap = kDefaultMemoryCap,
        qint64 diskCap = kDefaultDiskCap);

    /// The picture for `key` if it is already in memory. **No I/O**, so it is safe
    /// on the UI thread -- which is the whole reason the two tiers are asked
    /// separately.
    QImage inMemory(const ThumbnailKey& key);

    /// The picture for `key` from the disk tier, promoted into memory. Null on a
    /// miss, which includes a corrupt or truncated entry: this is a directory
    /// anybody can delete half of.
    ///
    /// Touches storage, so worker threads only.
    QImage onDisk(const ThumbnailKey& key);

    /// Records `image` for `key` in both tiers. Writing past the disk cap evicts
    /// least-recently-read first, checked here rather than by a sweep nobody
    /// triggers.
    ///
    /// Touches storage, so worker threads only.
    void store(const ThumbnailKey& key, const QImage& image);

    /// Bytes held in memory and on disk. For a test, and for anybody wondering
    /// what the caps are actually holding.
    qint64 memoryBytes() const;
    qint64 diskBytes();

    /// Forgets everything, on disk as well. Nothing is lost but time.
    void clear();

private:
    struct DiskEntry
    {
        qint64 bytes = 0;
        /// The cache file's own modification time, which is stamped on every read
        /// -- so "oldest" is least recently *read* and not least recently written.
        qint64 readAt = 0;
    };

    /// The file name for a key: a hash of the uri, the size and the date, so a
    /// name is short and a directory stays flat.
    static QString fileNameFor(const ThumbnailKey& key);
    /// Reads the directory once, so a write does not have to scan it. Called with
    /// the mutex held.
    void measureDirectory();
    /// Drops entries until the disk tier is under its cap. Mutex held.
    void evictWhileOverCap();
    /// Mutex held.
    void rememberInMemory(const QString& name, const QImage& image);

    mutable QMutex m_mutex;
    QString m_directory;
    qint64 m_memoryCap = 0;
    qint64 m_diskCap = 0;

    /// Name to picture, with the order they were last used. A list rather than a
    /// timestamp because the memory tier is asked often enough that a comparison
    /// per entry would show.
    QHash<QString, QImage> m_memory;
    QStringList m_memoryOrder; ///< most recently used last
    qint64 m_memoryBytes = 0;

    QHash<QString, DiskEntry> m_disk;
    qint64 m_diskBytes = 0;
    bool m_measured = false;
    /// When the directory was last read, and the mtime it had then. Together
    /// they answer "has another window written here since?" without a full
    /// re-read per store. See measureDirectory() and MOLE-385.
    qint64 m_measuredAt = 0;
    qint64 m_measuredMtime = 0;
    /// The shortest gap between two reconciliations. The cap is honoured within
    /// this rather than instantly, which is the trade for not reading the whole
    /// directory on every tile.
    static constexpr qint64 kReconcileSeconds = 10;
};

} // namespace mole
