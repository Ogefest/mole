#include "plugins/builtin/ImageFormats.h"
#include "plugins/builtin/thumbnails/ImageThumbnailer.h"
#include "support/FaultyFileSystem.h"
#include "support/ImageFixtures.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>

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

    void aRemoteJpegWithAThumbnailInsideItCostsOneBoundedRead();
    void aPortraitPhotographOnARemoteDriveAlsoComesBackPortrait();
    void aRemoteFileOverTheCeilingGetsNothingAndIsNotFetched();
    void aLocalFileIsDecodedInFullAndLooksIt();
    void anExifOffsetPointingOutsideThePrefixCostsThatThumbnailOnly();
    void aFolderOfRemotePhotographsCostsKilobytesEach();

private:
    /// Writes `bytes` into the memory drive and thumbnails it at `size`.
    QImage thumbnailOf(
        const QString& name, const QByteArray& bytes, int size, const CancelToken& cancel = CancelToken {});
    /// A listing's entry: the uri and the size, because a real one has both and
    /// the remote ceiling is decided on the size.
    FileEntry entryFor(const QString& name, qint64 bytes = 0) const;

    /// A camera JPEG: `size` pixels of picture with a real, complete JPEG
    /// thumbnail spliced into IFD1 of its EXIF block, the way a camera writes one.
    static QByteArray cameraJpeg(QSize size, QSize thumbnail);
    /// The same drive, wrapped in something that counts what was read through it.
    FaultyFileSystem* countingDrive();

    std::shared_ptr<MemoryFileSystem> m_fs;
    std::shared_ptr<FaultyFileSystem> m_counting;
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
    m_counting.reset();
    m_fs.reset();
}

FaultyFileSystem* TestImageThumbnailer::countingDrive()
{
    m_counting = std::make_shared<FaultyFileSystem>(m_fs);
    Mount mount;
    mount.displayName = QStringLiteral("counted");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = m_counting;
    // Replaces the plain mount: the same root, so the same uris resolve here.
    const QList<Mount> mounts = m_vfs->mounts();
    for (const Mount& existing : mounts)
        m_vfs->removeMount(existing.id);
    m_vfs->addMount(mount);
    return m_counting.get();
}

QByteArray TestImageThumbnailer::cameraJpeg(QSize size, QSize thumbnail)
{
    // A real JPEG for the thumbnail, because the whole claim is that what comes
    // out of IFD1 is a complete file that decodes on its own.
    QImage small(thumbnail, QImage::Format_RGB32);
    small.fill(Qt::magenta);
    QByteArray inner;
    {
        QBuffer buffer(&inner);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "jpeg");
        writer.write(small);
    }

    ExifBuilder exif(false);
    exif.addAscii(0x010f, "Mole");
    exif.addThumbnail(inner);
    // Textured, because a test about what a file costs to read needs a file that
    // costs something: two flat rectangles at 2400x1600 compress to nothing.
    return jpegOf(texturedImage(size), exif.build());
}

FileEntry TestImageThumbnailer::entryFor(const QString& name, qint64 bytes) const
{
    FileEntry entry;
    entry.uri = VfsUri::fromString(QStringLiteral("mem:///%1").arg(name));
    entry.name = name;
    entry.size = bytes;
    return entry;
}

QImage TestImageThumbnailer::thumbnailOf(
    const QString& name, const QByteArray& bytes, int size, const CancelToken& cancel)
{
    m_fs->addFile(QStringLiteral("/%1").arg(name), bytes);
    return m_thumbnailer.thumbnail(entryFor(name, bytes.size()), size, m_services, cancel);
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
///
/// **Asserted by what can and cannot be decoded, not by which was quicker.** It
/// used to time the two decodes and require the scaled one to take under half as
/// long, which is a comparison a scheduling pause inside either measurement can
/// invert -- and `make asan` is two to three times slower than the build the
/// number was chosen on. Qt refuses to allocate an image over
/// QImageReader::allocationLimit(), so lowering that limit to less than the full
/// picture makes the two paths answerable rather than merely faster and slower:
/// only a reader that was told to scale can produce anything at all. See
/// MOLE-400.
void TestImageThumbnailer::aLargeJpegIsDecodedScaledRatherThanInFull()
{
    const QByteArray big = jpegWithExif(QSize(4000, 3000), QByteArray());

    // 4000x3000 in ARGB32 is 48 MiB, so a whole-file decode needs more than this
    // and a decode bounded to 160 pixels needs about a hundredth of it. Put back
    // afterwards, because the limit is process-wide.
    const int wasAllowed = QImageReader::allocationLimit();
    QImageReader::setAllocationLimit(16);

    const QImage thumbnail = thumbnailOf(QStringLiteral("big.jpg"), big, 160);

    // The same bytes read whole, which is what this would have had to do without
    // setScaledSize(): refused, and that refusal is the assertion.
    QImage full;
    QString whyNot;
    {
        QBuffer buffer;
        buffer.setData(big);
        const bool opened = buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        full = opened ? reader.read() : QImage();
        whyNot = reader.errorString();
    }
    QImageReader::setAllocationLimit(wasAllowed);

    QVERIFY2(!thumbnail.isNull(), "a bounded decode has to fit inside the limit");
    QCOMPARE(qMax(thumbnail.width(), thumbnail.height()), 160);
    QVERIFY2(full.isNull(),
        qPrintable(QStringLiteral("the whole file decoded inside a 16 MiB limit, so this proves "
                                  "nothing about scaling: %1")
                       .arg(whyNot)));
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
    QVERIFY(
        m_thumbnailer.thumbnail(entryFor(QStringLiteral("gone.jpg"), 4096), 96, m_services, CancelToken {})
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
    QVERIFY(imageSuffixes().contains(QStringLiteral("png")));
    QCOMPARE(imageSuffixes().contains(QStringLiteral("webp")),
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

// ------------------------------------------------ what a remote drive costs
//
// A folder of five hundred photographs on a network drive is four gigabytes.
// Making 200-pixel tiles by fetching all of it would be the most expensive thing
// this application has ever done, and on a metered bucket it is billed. Every
// camera JPEG already carries a thumbnail in its first 64 kB. See MOLE-143.

void TestImageThumbnailer::aRemoteJpegWithAThumbnailInsideItCostsOneBoundedRead()
{
    const QByteArray photograph = cameraJpeg(QSize(2400, 1600), QSize(160, 120));
    QVERIFY2(photograph.size() > 200000, "the file has to be big enough for the saving to mean something");
    m_fs->addFile(QStringLiteral("/holiday.jpg"), photograph);

    FaultyFileSystem* counted = countingDrive();
    const QImage tile = m_thumbnailer.thumbnail(
        entryFor(QStringLiteral("holiday.jpg"), photograph.size()), 200, m_services, CancelToken {});

    QVERIFY2(!tile.isNull(), "the picture inside the file is a tile");
    // Softer than the tile, because 160x120 is what the camera wrote -- shown
    // rather than nothing, which is the whole job.
    QVERIFY2(qMax(tile.width(), tile.height()) <= 200, "and still bounded by what was asked for");

    QVERIFY2(counted->bytesRead() <= thumbnails::kPrefixBytes,
        qPrintable(QStringLiteral("read %1 bytes of a %2-byte file")
                       .arg(counted->bytesRead())
                       .arg(photograph.size())));
    QCOMPARE(counted->openReadCount(), 1);
}

/// The same photograph answered two ways depending on the drive.
///
/// The embedded path returns IFD1's JPEG as it stands, and the orientation is
/// IFD0 tag 0x0112 -- not in the thumbnail's own stream, so the autotransform on
/// the decoded path cannot see it either. So a portrait phone picture came back
/// sideways from a remote drive and upright from a local one, which is the worst
/// shape for a fault: it looks like the drive's doing. See MOLE-385.
void TestImageThumbnailer::aPortraitPhotographOnARemoteDriveAlsoComesBackPortrait()
{
    // A camera JPEG whose embedded thumbnail is landscape and whose IFD0 says to
    // turn it a quarter turn -- which is what a phone held upright writes.
    QImage small(QSize(160, 120), QImage::Format_RGB32);
    small.fill(Qt::magenta);
    QByteArray inner;
    {
        QBuffer buffer(&inner);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "jpeg");
        writer.write(small);
    }

    ExifBuilder exif(false);
    exif.addShort(0x0112, 6); // rotated 90 degrees clockwise
    exif.addThumbnail(inner);
    const QByteArray photograph = jpegOf(texturedImage(QSize(2400, 1600)), exif.build());
    m_fs->addFile(QStringLiteral("/portrait.jpg"), photograph);

    FaultyFileSystem* counted = countingDrive();
    const QImage tile = m_thumbnailer.thumbnail(
        entryFor(QStringLiteral("portrait.jpg"), photograph.size()), 200, m_services, CancelToken {});

    QVERIFY2(!tile.isNull(), "the picture inside the file is a tile");
    QVERIFY2(tile.height() > tile.width(),
        qPrintable(QStringLiteral("came back %1x%2, so the orientation was ignored")
                       .arg(tile.width())
                       .arg(tile.height())));

    // And it is still the cheap path: turning the tile upright must not cost the
    // whole file.
    QVERIFY2(counted->bytesRead() <= thumbnails::kPrefixBytes,
        qPrintable(QStringLiteral("read %1 bytes of a %2-byte file")
                       .arg(counted->bytesRead())
                       .arg(photograph.size())));
}

void TestImageThumbnailer::aRemoteFileOverTheCeilingGetsNothingAndIsNotFetched()
{
    // No EXIF at all, so there is nothing inside it to find -- and past the
    // ceiling, so fetching it is not worth what it costs.
    const QByteArray plain = jpegWithExif(QSize(800, 600), QByteArray());
    m_fs->addFile(QStringLiteral("/raw.jpg"), plain);

    FaultyFileSystem* counted = countingDrive();
    const qint64 claimed = ImageThumbnailer::kRemoteCeiling + 1;
    const QImage tile
        = m_thumbnailer.thumbnail(entryFor(QStringLiteral("raw.jpg"), claimed), 200, m_services, {});

    QVERIFY2(tile.isNull(), "an icon tile is a correct answer, not a failure");
    // The prefix was looked at, because that is where a camera's own thumbnail
    // would have been; the body was not.
    QVERIFY2(counted->bytesRead() <= thumbnails::kPrefixBytes,
        qPrintable(QStringLiteral("read %1 bytes").arg(counted->bytesRead())));
    QVERIFY2(counted->bytesRead() < claimed / 2, "nothing like the whole file was fetched");
}

void TestImageThumbnailer::aLocalFileIsDecodedInFullAndLooksIt()
{
    QTemporaryDir local;
    QVERIFY(local.isValid());
    const QString path = QDir(local.path()).filePath(QStringLiteral("holiday.jpg"));
    {
        // The same file: a big picture with a small thumbnail inside it. On a local
        // drive the decode is the answer, because reading it costs nothing.
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray photograph = cameraJpeg(QSize(1200, 900), QSize(160, 120));
        QVERIFY(file.write(photograph) == photograph.size());
    }

    auto local_fs = std::make_shared<LocalFileSystem>();
    Mount mount;
    mount.displayName = QStringLiteral("disk");
    mount.root = VfsUri::fromLocalPath(local.path());
    mount.fileSystem = local_fs;
    QVERIFY(!m_vfs->addMount(mount).isEmpty());

    FileEntry entry;
    entry.uri = VfsUri::fromLocalPath(path);
    entry.name = QStringLiteral("holiday.jpg");
    entry.size = QFileInfo(path).size();
    QVERIFY(!thumbnails::isRemote(entry.uri));

    const QImage tile = m_thumbnailer.thumbnail(entry, 200, m_services, CancelToken {});
    QVERIFY(!tile.isNull());
    // The embedded one is 160x120 and would come back at 160 on its longest edge.
    // A full decode of a 1200x900 file asked for 200 comes back at 200, which is
    // how the two are told apart.
    QCOMPARE(qMax(tile.width(), tile.height()), 200);
}

/// An offset in a file is a claim, not a promise.
void TestImageThumbnailer::anExifOffsetPointingOutsideThePrefixCostsThatThumbnailOnly()
{
    ExifBuilder exif(false);
    exif.addAscii(0x010f, "Mole");
    exif.addThumbnail(QByteArray("\xff\xd8not really a jpeg", 20));
    // The thumbnail's own offset, pointed a long way outside the block.
    exif.pointThumbnailAt(0x7fffffff);
    const QByteArray bent = jpegWithExif(QSize(640, 480), exif.build());
    m_fs->addFile(QStringLiteral("/bent.jpg"), bent);

    FaultyFileSystem* counted = countingDrive();
    // Under the ceiling, so the fallback decode is allowed and is what answers.
    const QImage tile
        = m_thumbnailer.thumbnail(entryFor(QStringLiteral("bent.jpg"), bent.size()), 120, m_services, {});

    // No crash, no unbounded read, and the tile still comes from the file itself.
    QVERIFY(!tile.isNull());
    QCOMPARE(qMax(tile.width(), tile.height()), 120);
    QVERIFY2(counted->bytesRead() < thumbnails::kPrefixBytes + bent.size() + 1,
        qPrintable(QStringLiteral("read %1 bytes").arg(counted->bytesRead())));
}

void TestImageThumbnailer::aFolderOfRemotePhotographsCostsKilobytesEach()
{
    const int photographs = 20;
    qint64 onTheDrive = 0;
    for (int i = 0; i < photographs; ++i) {
        const QByteArray photograph = cameraJpeg(QSize(2000, 1500), QSize(160, 120));
        onTheDrive += photograph.size();
        m_fs->addFile(QStringLiteral("/photos/shot-%1.jpg").arg(i), photograph);
    }
    QVERIFY2(onTheDrive > 20 * 100000, "the folder has to be worth not downloading");

    FaultyFileSystem* counted = countingDrive();
    for (int i = 0; i < photographs; ++i) {
        FileEntry entry;
        entry.uri = VfsUri::fromString(QStringLiteral("mem:///photos/shot-%1.jpg").arg(i));
        entry.name = entry.uri.fileName();
        entry.size = onTheDrive / photographs;
        QVERIFY(!m_thumbnailer.thumbnail(entry, 200, m_services, CancelToken {}).isNull());
    }

    // Kilobytes each, not megabytes: the whole point of the ticket, counted rather
    // than argued.
    const qint64 perPhotograph = counted->bytesRead() / photographs;
    QVERIFY2(perPhotograph <= thumbnails::kPrefixBytes,
        qPrintable(QStringLiteral("%1 bytes each against %2 on the drive")
                       .arg(perPhotograph)
                       .arg(onTheDrive / photographs)));
    QVERIFY2(counted->bytesRead() * 4 < onTheDrive,
        qPrintable(QStringLiteral("read %1 of %2 bytes").arg(counted->bytesRead()).arg(onTheDrive)));
}

MOLE_TEST_MAIN(TestImageThumbnailer)
#include "tst_ImageThumbnailer.moc"
