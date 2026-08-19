#include "plugins/builtin/thumbnails/ImageThumbnailer.h"
#include "support/ImageFixtures.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QElapsedTimer>
#include <QImageReader>

using namespace mole;
using namespace mole::test;

/// A picture of a picture, which is the case the gallery exists for. Two calls on
/// QImageReader are the whole of the quality of this, and both are asserted here:
/// scaled decoding, and the auto-transform that is always the missing line.
/// See MOLE-140.
class TestImageThumbnailer : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void picturesComeBackBoundedWithTheirAspectKept();
    void aPortraitPhotographComesBackPortrait();
    void aLargeJpegIsDecodedScaledRatherThanInFull();
    void aFileNothingCanDecodeIsAnOrdinaryAnswer();
    void claimsWhatThisBuildCanRead();
    void cancellationIsHonouredBeforeTheDecode();

private:
    /// Writes `bytes` into the memory drive and thumbnails it at `size`.
    QImage thumbnailOf(
        const QString& name, const QByteArray& bytes, int size, const CancelToken& cancel = CancelToken {});
    FileEntry entryFor(const QString& name) const;

    std::shared_ptr<MemoryFileSystem> m_fs;
    std::unique_ptr<VfsManager> m_vfs;
    PluginServices m_services;
    ImageThumbnailer m_thumbnailer;
};

void TestImageThumbnailer::init()
{
    m_fs = std::make_shared<MemoryFileSystem>();
    m_vfs = std::make_unique<VfsManager>();
    Mount mount;
    mount.displayName = QStringLiteral("scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = m_fs;
    QVERIFY(!m_vfs->addMount(mount).isEmpty());

    m_services = PluginServices {};
    m_services.vfs = m_vfs.get();
}

void TestImageThumbnailer::cleanup()
{
    m_vfs.reset();
    m_fs.reset();
}

FileEntry TestImageThumbnailer::entryFor(const QString& name) const
{
    FileEntry entry;
    entry.uri = VfsUri::fromString(QStringLiteral("mem:///%1").arg(name));
    entry.name = name;
    return entry;
}

QImage TestImageThumbnailer::thumbnailOf(
    const QString& name, const QByteArray& bytes, int size, const CancelToken& cancel)
{
    m_fs->addFile(QStringLiteral("/%1").arg(name), bytes);
    return m_thumbnailer.thumbnail(entryFor(name), size, m_services, cancel);
}

void TestImageThumbnailer::picturesComeBackBoundedWithTheirAspectKept()
{
    // One of each of the three formats every build has, so the claim is about the
    // thumbnailer and not about one image plugin.
    struct Case
    {
        QString name;
        QByteArray bytes;
    };
    QList<Case> cases {
        { QStringLiteral("wide.jpg"), jpegWithExif(QSize(800, 400), QByteArray()) },
        { QStringLiteral("clear.png"), pngOf(QSize(400, 800), QImage::Format_ARGB32) },
    };
    if (QImageReader::supportedImageFormats().contains(QByteArray("webp"))) {
        QImage source(QSize(600, 300), QImage::Format_RGB32);
        source.fill(Qt::magenta);
        QByteArray encoded;
        QBuffer buffer(&encoded);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImageWriter writer(&buffer, "webp");
        QVERIFY(writer.write(source));
        cases.append({ QStringLiteral("shot.webp"), encoded });
    }

    for (const Case& one : cases) {
        const QImage thumbnail = thumbnailOf(one.name, one.bytes, 160);
        QVERIFY2(!thumbnail.isNull(), qPrintable(one.name));
        // Bounded, and exactly: the longest edge is what was asked for.
        QCOMPARE(qMax(thumbnail.width(), thumbnail.height()), 160);
        // And the shape is the file's, not a square.
        QVERIFY2(qMin(thumbnail.width(), thumbnail.height()) < 160, qPrintable(one.name));
    }
}

/// The test that fails without setAutoTransform. Every picture from a phone is on
/// its side without it, and this is the line that is always missing.
void TestImageThumbnailer::aPortraitPhotographComesBackPortrait()
{
    // A landscape file that says it should be shown rotated a quarter turn, which
    // is what a camera held upright writes.
    ExifBuilder exif(false);
    exif.addShort(0x0112, 6); // orientation: rotated 90 degrees clockwise
    const QByteArray upright = jpegWithExif(QSize(640, 480), exif.build());

    const QImage thumbnail = thumbnailOf(QStringLiteral("upright.jpg"), upright, 120);
    QVERIFY(!thumbnail.isNull());
    QVERIFY2(thumbnail.height() > thumbnail.width(),
        qPrintable(QStringLiteral("came back %1x%2, so the orientation was ignored")
                       .arg(thumbnail.width())
                       .arg(thumbnail.height())));
    QCOMPARE(qMax(thumbnail.width(), thumbnail.height()), 120);
}

/// A 24-megapixel photograph decoded in full to make a 160-pixel tile is the
/// difference between a gallery that opens and one that swaps.
void TestImageThumbnailer::aLargeJpegIsDecodedScaledRatherThanInFull()
{
    const QByteArray big = jpegWithExif(QSize(4000, 3000), QByteArray());

    QElapsedTimer scaled;
    scaled.start();
    const QImage thumbnail = thumbnailOf(QStringLiteral("big.jpg"), big, 160);
    const qint64 scaledMs = scaled.elapsed();
    QVERIFY(!thumbnail.isNull());
    QCOMPARE(qMax(thumbnail.width(), thumbnail.height()), 160);

    // The same file decoded whole, for something honest to compare against: a
    // bare number of milliseconds would say nothing about this machine.
    QElapsedTimer whole;
    whole.start();
    QImage full;
    {
        QBuffer buffer;
        buffer.setData(big);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer);
        full = reader.read();
    }
    const qint64 wholeMs = whole.elapsed();
    QCOMPARE(full.size(), QSize(4000, 3000));

    // Half is a wide margin on purpose: what is being held is that the DCT scaler
    // was reached at all, not a ratio that would differ per machine.
    QVERIFY2(scaledMs * 2 <= wholeMs + 1,
        qPrintable(QStringLiteral("scaled decode took %1 ms against %2 ms for the whole file")
                       .arg(scaledMs)
                       .arg(wholeMs)));
}

void TestImageThumbnailer::aFileNothingCanDecodeIsAnOrdinaryAnswer()
{
    // Corrupt, empty, and a format with no decoder. None of these is an error and
    // none of them may put anything on screen.
    QVERIFY(
        thumbnailOf(QStringLiteral("truncated.jpg"), jpegWithExif(QSize(320, 240), QByteArray()).left(40), 96)
            .isNull());
    QVERIFY(thumbnailOf(QStringLiteral("empty.png"), QByteArray(), 96).isNull());
    QVERIFY(thumbnailOf(QStringLiteral("notes.txt"), QByteArray("not a picture"), 96).isNull());

    // And a file that is not there at all, which is what a race with a delete
    // looks like from here.
    QVERIFY(m_thumbnailer.thumbnail(entryFor(QStringLiteral("gone.jpg")), 96, m_services, CancelToken {})
                .isNull());
}

void TestImageThumbnailer::claimsWhatThisBuildCanRead()
{
    QVERIFY(m_thumbnailer.canThumbnail(entryFor(QStringLiteral("a.png"))));
    QVERIFY(m_thumbnailer.canThumbnail(entryFor(QStringLiteral("a.JPG")))); // asked case-blind
    QVERIFY(!m_thumbnailer.canThumbnail(entryFor(QStringLiteral("a.txt"))));

    FileEntry folder = entryFor(QStringLiteral("pictures"));
    folder.isDir = true;
    QVERIFY2(!m_thumbnailer.canThumbnail(folder), "a folder has no picture of its own");

    // Asked of Qt rather than hard-coded, so a build without the WebP plugin does
    // not claim WebP.
    QVERIFY(ImageThumbnailer::imageSuffixes().contains(QStringLiteral("png")));
    QCOMPARE(ImageThumbnailer::imageSuffixes().contains(QStringLiteral("webp")),
        QImageReader::supportedImageFormats().contains(QByteArray("webp")));
}

void TestImageThumbnailer::cancellationIsHonouredBeforeTheDecode()
{
    CancelToken cancelled;
    cancelled.cancel();
    QVERIFY2(
        thumbnailOf(QStringLiteral("late.jpg"), jpegWithExif(QSize(640, 480), QByteArray()), 160, cancelled)
            .isNull(),
        "a folder scrolled past leaves its decodes pointless");
}

MOLE_TEST_MAIN(TestImageThumbnailer)
#include "tst_ImageThumbnailer.moc"
