#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/ThumbnailCache.h"

#include <QDir>
#include <QImageReader>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// The two tiers, and the ceiling. Every visit to a folder of photographs used to
/// decode every photograph again, and so did every scroll back up the same
/// folder. See MOLE-141.
class TestThumbnailCache : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theDirectoryComesFromTheEnvironmentAndNowhereElse();
    void aPictureStoredIsFoundInMemoryAndOnDisk();
    void aNewCacheOverTheSameDirectoryReadsWhatTheLastOneWrote();
    void aDifferentDateIsADifferentPicture();
    void aReplacementThatKeptTheOldDateIsStillADifferentPicture();
    void anEntryRecordsWhereItCameFrom();
    void aTruncatedEntryIsAMissAndIsOverwritten();
    void writingPastTheDiskCapEvictsTheLeastRecentlyRead();
    void theMemoryTierIsCappedInBytes();
    void aPictureTooLargeForTheTierIsNotWorthEmptyingItFor();

private:
    ThumbnailKey keyFor(const QString& name, int size = 64, qint64 mtime = 1000) const;
    static QImage pictureOf(int side, QColor colour = Qt::red);
    /// A picture that does not compress to nothing, so an entry on disk has a size
    /// worth capping. Deterministic, because a test that varies its own fixture
    /// varies its own answer.
    static QImage texturedPicture(int side, int seed);
    /// What one `texturedPicture(96, ...)` costs on disk, measured rather than
    /// guessed: how well a picture compresses is not something a test should
    /// predict.
    qint64 measureOneEntry() const;

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_cacheDir;
};

void TestThumbnailCache::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_cacheDir = QDir(m_dir->path()).filePath(QStringLiteral("thumbs"));
}

void TestThumbnailCache::cleanup()
{
    m_dir.reset();
}

ThumbnailKey TestThumbnailCache::keyFor(const QString& name, int size, qint64 mtime) const
{
    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/%1").arg(name));
    key.size = size;
    key.mtime = mtime;
    return key;
}

QImage TestThumbnailCache::pictureOf(int side, QColor colour)
{
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(colour);
    return image;
}

QImage TestThumbnailCache::texturedPicture(int side, int seed)
{
    QImage image(side, side, QImage::Format_RGB32);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const int mixed = (x * 37 + y * 91 + seed * 13) % 251;
            image.setPixel(x, y, qRgb(mixed, (mixed * 7) % 251, (mixed * 11) % 251));
        }
    }
    return image;
}

qint64 TestThumbnailCache::measureOneEntry() const
{
    const QString probeDir = QDir(m_dir->path()).filePath(QStringLiteral("probe"));
    ThumbnailCache probe(probeDir);
    probe.store(keyFor(QStringLiteral("probe.jpg")), texturedPicture(96, 0));
    return probe.diskBytes();
}

/// Everything else in this list is worthless if the suite is writing into the
/// developer's own cache directory.
void TestThumbnailCache::theDirectoryComesFromTheEnvironmentAndNowhereElse()
{
    const QByteArray was = qgetenv("MOLE_THUMBNAILS_PATH");
    qputenv("MOLE_THUMBNAILS_PATH", m_cacheDir.toLocal8Bit());
    QCOMPARE(ThumbnailCache::defaultDirectory(), m_cacheDir);

    // And without it, the cache location rather than the data location: this is
    // the first store here holding something that can be recomputed, and deleting
    // it must cost nothing but time.
    qunsetenv("MOLE_THUMBNAILS_PATH");
    const QString fallback = ThumbnailCache::defaultDirectory();
    QVERIFY2(!fallback.isEmpty(), qPrintable(fallback));
    QVERIFY2(fallback.contains(QStringLiteral("cache"), Qt::CaseInsensitive), qPrintable(fallback));

    if (!was.isEmpty())
        qputenv("MOLE_THUMBNAILS_PATH", was);
}

void TestThumbnailCache::aPictureStoredIsFoundInMemoryAndOnDisk()
{
    ThumbnailCache cache(m_cacheDir);
    const ThumbnailKey key = keyFor(QStringLiteral("a.jpg"));

    QVERIFY(cache.inMemory(key).isNull());
    QVERIFY(cache.onDisk(key).isNull());

    cache.store(key, pictureOf(64));

    const QImage remembered = cache.inMemory(key);
    QVERIFY(!remembered.isNull());
    QCOMPARE(remembered.size(), QSize(64, 64));
    QVERIFY(cache.memoryBytes() > 0);
    QVERIFY(cache.diskBytes() > 0);

    // A different size is a different picture, because the same file in the small
    // grid and in the gallery are two of them.
    QVERIFY(cache.inMemory(keyFor(QStringLiteral("a.jpg"), 200)).isNull());
}

/// The tier that makes the *second visit* to a folder cost nothing.
void TestThumbnailCache::aNewCacheOverTheSameDirectoryReadsWhatTheLastOneWrote()
{
    const ThumbnailKey key = keyFor(QStringLiteral("a.jpg"));
    {
        ThumbnailCache writing(m_cacheDir);
        writing.store(key, pictureOf(64, Qt::green));
    }

    // A fresh cache, which is what a restart looks like: nothing in memory, and
    // the answer on disk.
    ThumbnailCache reading(m_cacheDir);
    QVERIFY2(reading.inMemory(key).isNull(), "a new run starts with an empty memory tier");
    const QImage kept = reading.onDisk(key);
    QVERIFY(!kept.isNull());
    QCOMPARE(kept.size(), QSize(64, 64));
    // And reading it promotes it, so the scroll afterwards costs nothing either.
    QVERIFY(!reading.inMemory(key).isNull());
}

/// The date is in the key so that editing a file produces a new picture rather
/// than the one from before.
void TestThumbnailCache::aDifferentDateIsADifferentPicture()
{
    ThumbnailCache cache(m_cacheDir);
    const ThumbnailKey before = keyFor(QStringLiteral("a.jpg"), 64, 1000);
    cache.store(before, pictureOf(64, Qt::red));

    const ThumbnailKey after = keyFor(QStringLiteral("a.jpg"), 64, 2000);
    QVERIFY2(cache.inMemory(after).isNull(), "a touched file must not answer with the old picture");
    QVERIFY(cache.onDisk(after).isNull());

    cache.store(after, pictureOf(64, Qt::blue));
    QCOMPARE(cache.inMemory(after).pixelColor(0, 0), QColor(Qt::blue));
    // And the stale one does not come back: it is a different entry, so what a
    // sweep does with it is eviction's business and not a wrong answer.
    QCOMPARE(cache.inMemory(before).pixelColor(0, 0), QColor(Qt::red));
}

/// A file replaced with its old date kept the old picture.
///
/// The key was the uri, the requested size and the mtime -- and `cp -p`,
/// `rsync -a`, a tar extract and a photo re-exported over itself by a tool that
/// keeps the mtime all replace a file and preserve its date. Two edits inside
/// one second are indistinguishable by seconds as well. ADR-0059 chose the date
/// over a content hash, which is right; it did not discuss the size, which is
/// free and was already sitting on the key. See MOLE-385.
void TestThumbnailCache::aReplacementThatKeptTheOldDateIsStillADifferentPicture()
{
    ThumbnailCache cache(m_cacheDir);

    ThumbnailKey before = keyFor(QStringLiteral("holiday.jpg"), 64, 1000);
    before.bytes = 240000;
    cache.store(before, pictureOf(64, Qt::red));
    QCOMPARE(cache.inMemory(before).pixelColor(0, 0), QColor(Qt::red));

    // The same name, the same date -- rsync kept it -- and a different length.
    ThumbnailKey after = before;
    after.bytes = 310000;
    QVERIFY2(cache.inMemory(after).isNull(), "a replaced file answered with the picture of the old one");
    QVERIFY(cache.onDisk(after).isNull());

    cache.store(after, pictureOf(64, Qt::blue));
    QCOMPARE(cache.inMemory(after).pixelColor(0, 0), QColor(Qt::blue));
    QCOMPARE(cache.inMemory(before).pixelColor(0, 0), QColor(Qt::red));

    // And a caller with no size hint still finds what a caller with one stored:
    // the size joins the key when it is there, and a nought is not a length.
    ThumbnailKey noHint = before;
    noHint.bytes = 0;
    cache.store(noHint, pictureOf(64, Qt::green));
    QCOMPARE(cache.inMemory(noHint).pixelColor(0, 0), QColor(Qt::green));
}

/// A directory of hashes nobody can explain is a directory nobody dares delete
/// from.
void TestThumbnailCache::anEntryRecordsWhereItCameFrom()
{
    ThumbnailCache cache(m_cacheDir);
    const ThumbnailKey key = keyFor(QStringLiteral("holiday.jpg"));
    cache.store(key, pictureOf(48));

    const QFileInfoList files = QDir(m_cacheDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(files.size(), 1);
    QImageReader reader(files.first().absoluteFilePath());
    QCOMPARE(reader.text(QStringLiteral("mole.uri")), key.uri.toString());
}

void TestThumbnailCache::aTruncatedEntryIsAMissAndIsOverwritten()
{
    ThumbnailCache cache(m_cacheDir);
    const ThumbnailKey key = keyFor(QStringLiteral("a.jpg"));
    cache.store(key, pictureOf(64));

    const QFileInfoList files = QDir(m_cacheDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(files.size(), 1);
    const QString path = files.first().absoluteFilePath();
    {
        // Half an entry, which is what a directory anybody can delete half of
        // eventually contains.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(file.size() / 3));
    }

    ThumbnailCache reading(m_cacheDir);
    QVERIFY2(reading.onDisk(key).isNull(), "a truncated entry is a miss, not a broken tile");

    // And writing over it works, rather than leaving a file that misses for ever.
    reading.store(key, pictureOf(64, Qt::green));
    ThumbnailCache again(m_cacheDir);
    const QImage rewritten = again.onDisk(key);
    QVERIFY(!rewritten.isNull());
    QCOMPARE(rewritten.size(), QSize(64, 64));
    // JPEG, so not the exact colour: near enough that it is plainly the picture
    // that was just written and not the one before it.
    const QColor corner = rewritten.pixelColor(0, 0);
    QVERIFY2(corner.green() > 200 && corner.red() < 60, qPrintable(corner.name()));
}

/// A hundred thousand pictures is a hundred thousand entries. A cache with no
/// ceiling is a bug report in six months.
void TestThumbnailCache::writingPastTheDiskCapEvictsTheLeastRecentlyRead()
{
    // A cap that holds two entries and not three, so the third write has to evict
    // and the test is about which one goes rather than about whether any does.
    const qint64 entry = measureOneEntry();
    QVERIFY2(entry > 0, "the probe wrote nothing, so this test measures nothing");
    const qint64 cap = entry * 2 + entry / 2;
    ThumbnailCache cache(m_cacheDir, ThumbnailCache::kDefaultMemoryCap, cap);

    const ThumbnailKey first = keyFor(QStringLiteral("first.jpg"));
    const ThumbnailKey second = keyFor(QStringLiteral("second.jpg"));
    cache.store(first, texturedPicture(96, 1));
    cache.store(second, texturedPicture(96, 2));
    QVERIFY(cache.diskBytes() <= cap);

    // Read the first, so the second is the least recently read of the two. Waited
    // on the clock's own granularity: the stamp is in seconds, so two reads in the
    // same second are not in an order.
    QTest::qWait(1100);
    QVERIFY(!cache.onDisk(first).isNull());

    const ThumbnailKey third = keyFor(QStringLiteral("third.jpg"));
    cache.store(third, texturedPicture(96, 3));

    QVERIFY2(cache.diskBytes() <= cap,
        qPrintable(QStringLiteral("the cap is %1 and the cache holds %2").arg(cap).arg(cache.diskBytes())));

    // The cap holds across a restart, because the directory is the state.
    ThumbnailCache after(m_cacheDir, ThumbnailCache::kDefaultMemoryCap, cap);
    QVERIFY(after.diskBytes() <= cap);
    QVERIFY2(!after.onDisk(third).isNull(), "what was just written is still there");
    QVERIFY2(after.onDisk(second).isNull(), "the least recently read is the one that goes");
    QVERIFY2(!after.onDisk(first).isNull(), "and the one that was read is kept");
}

void TestThumbnailCache::theMemoryTierIsCappedInBytes()
{
    // One 64x64 ARGB picture is 16 kB, so a 40 kB tier holds two and not three.
    ThumbnailCache cache(m_cacheDir, 40 * 1024, 0);

    const ThumbnailKey first = keyFor(QStringLiteral("first.jpg"));
    const ThumbnailKey second = keyFor(QStringLiteral("second.jpg"));
    const ThumbnailKey third = keyFor(QStringLiteral("third.jpg"));
    cache.store(first, pictureOf(64));
    cache.store(second, pictureOf(64));
    // Touched, so the first is not the oldest any more.
    QVERIFY(!cache.inMemory(first).isNull());
    cache.store(third, pictureOf(64));

    QVERIFY2(cache.memoryBytes() <= 40 * 1024,
        qPrintable(QStringLiteral("the tier holds %1 bytes").arg(cache.memoryBytes())));
    QVERIFY(!cache.inMemory(third).isNull());
    QVERIFY(!cache.inMemory(first).isNull());
    QVERIFY2(cache.inMemory(second).isNull(), "least recently used is what goes");

    // With no disk tier at all, nothing was written: a cache directory that
    // appears when the cap is nought would be a surprise.
    QVERIFY(!QDir(m_cacheDir).exists());
}

void TestThumbnailCache::aPictureTooLargeForTheTierIsNotWorthEmptyingItFor()
{
    ThumbnailCache cache(m_cacheDir, 20 * 1024, 0);
    const ThumbnailKey small = keyFor(QStringLiteral("small.jpg"));
    cache.store(small, pictureOf(32));
    const qint64 held = cache.memoryBytes();
    QVERIFY(held > 0);

    // Larger than the whole tier. Taking it would empty everything else for one
    // picture that is evicted the moment the next arrives.
    cache.store(keyFor(QStringLiteral("panorama.jpg")), pictureOf(512));
    QCOMPARE(cache.memoryBytes(), held);
    QVERIFY2(!cache.inMemory(small).isNull(), "the tier was not emptied for it");
}

MOLE_TEST_MAIN(TestThumbnailCache)
#include "tst_ThumbnailCache.moc"
