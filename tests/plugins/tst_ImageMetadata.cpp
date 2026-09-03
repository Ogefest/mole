#include "plugins/builtin/previews/ImageMetadata.h"
#include "support/ImageFixtures.h"
#include "support/MoleTestMain.h"

#include <QBuffer>
#include <QImage>
#include <QImageWriter>
#include <QPainter>

#include <cstring>
#include <memory>

using namespace mole;
using namespace mole::test;

namespace {

QString factNamed(const QList<FileFact>& facts, const QString& label)
{
    for (const FileFact& fact : facts) {
        if (fact.label == label)
            return fact.value;
    }
    return {};
}

/// An EXIF block with one of everything worth showing.
QByteArray fullExif(bool bigEndian)
{
    ExifBuilder exif(bigEndian);
    exif.addAscii(0x010f, "Canon");
    exif.addAscii(0x0110, "Canon EOS 5D Mark IV");
    exif.addAscii(0x0131, "Mole 1.0");
    exif.addShort(0x0112, 6);
    exif.addExifAscii(0xa434, "EF 35mm f/1.4L II USM");
    exif.addExifRational(0x829a, 1, 250); // exposure
    exif.addExifRational(0x829d, 28, 10); // f/2.8
    exif.addExifShort(0x8827, 400); // ISO
    exif.addExifRational(0x920a, 35, 1); // focal length
    exif.addExifSignedRational(0x9204, -3, 10); // exposure bias
    exif.addExifShort(0x9209, 0x0009); // flash fired
    exif.addExifAscii(0x9003, "2026:03:14 09:31:07");
    exif.addGpsAscii(0x0001, "N");
    exif.addGpsCoordinate(0x0002, 52, 13, 46.9);
    exif.addGpsAscii(0x0003, "E");
    exif.addGpsCoordinate(0x0004, 21, 0, 43.9);
    exif.addGpsRational(0x0006, 113, 1);
    return exif.build();
}

} // namespace

/// What a photograph says about itself, read out of its header and never out of
/// a decode.
class TestImageMetadata : public QObject
{
    Q_OBJECT

private slots:
    void readsEveryTagACameraWrote_data();
    void readsEveryTagACameraWrote();
    void anOffsetPastTheEndCostsThatTagAndNothingElse();
    void aPhotographWithNoExifStillHasDimensions();
    void aTruncatedFileReportsWhatIsThereAndInventsNothing();
    void aPrefixEndingInsideTheExifMarkerIsNotReadPast();
    void pngAndFriendsReportDimensionsAndDepth_data();
    void pngAndFriendsReportDimensionsAndDepth();
    void aHeaderIsEnoughHoweverLargeTheFile();
};

void TestImageMetadata::readsEveryTagACameraWrote_data()
{
    QTest::addColumn<bool>("bigEndian");
    QTest::newRow("little endian (Intel)") << false;
    QTest::newRow("big endian (Motorola)") << true;
}

void TestImageMetadata::readsEveryTagACameraWrote()
{
    QFETCH(bool, bigEndian);

    const QByteArray file = jpegWithExif(QSize(640, 480), fullExif(bigEndian));
    const QList<FileFact> facts = ImageMetadataReader::factsFor(file, QStringLiteral("holiday.jpg"));

    QCOMPARE(factNamed(facts, QStringLiteral("Dimensions")), QStringLiteral("640 × 480"));
    QCOMPARE(factNamed(facts, QStringLiteral("Format")), QStringLiteral("JPEG"));

    // The make is not repeated when the model already carries it.
    QCOMPARE(factNamed(facts, QStringLiteral("Camera")), QStringLiteral("Canon EOS 5D Mark IV"));
    QCOMPARE(factNamed(facts, QStringLiteral("Lens")), QStringLiteral("EF 35mm f/1.4L II USM"));

    // Formatted the way a camera says them, not as the rational pairs they are.
    QCOMPARE(factNamed(facts, QStringLiteral("Exposure")), QStringLiteral("1/250 s"));
    QCOMPARE(factNamed(facts, QStringLiteral("Aperture")), QStringLiteral("f/2.8"));
    QCOMPARE(factNamed(facts, QStringLiteral("Sensitivity")), QStringLiteral("ISO 400"));
    QCOMPARE(factNamed(facts, QStringLiteral("Focal length")), QStringLiteral("35 mm"));
    QCOMPARE(factNamed(facts, QStringLiteral("Exposure bias")), QStringLiteral("-0.3 EV"));
    QCOMPARE(factNamed(facts, QStringLiteral("Flash")), QStringLiteral("fired"));
    QCOMPARE(factNamed(facts, QStringLiteral("Orientation")), QStringLiteral("rotated 90° clockwise"));
    QCOMPARE(factNamed(facts, QStringLiteral("Software")), QStringLiteral("Mole 1.0"));
    QVERIFY2(factNamed(facts, QStringLiteral("Taken")).contains(QStringLiteral("2026")),
        qPrintable(factNamed(facts, QStringLiteral("Taken"))));

    // Numbers, and nothing looked up: a preview must put nothing on the network.
    QCOMPARE(factNamed(facts, QStringLiteral("Position")), QStringLiteral("52.22969, 21.01219"));
    QCOMPARE(factNamed(facts, QStringLiteral("Altitude")), QStringLiteral("113 m"));
}

void TestImageMetadata::anOffsetPastTheEndCostsThatTagAndNothingElse()
{
    // An IFD entry says where its value is, and a file is allowed to lie. The
    // house lesson from ADR-0010: an offset inside a file is a claim.
    ExifBuilder exif(false);
    exif.addAscii(0x010f, "Canon");
    exif.addAscii(0x0110, "Canon EOS 5D Mark IV");
    exif.addAscii(0x0131, "Mole 1.0");
    exif.pointTagAt(0x0131, 0x7fffff00); // the software tag, somewhere in orbit
    exif.addExifRational(0x829a, 1, 250);

    const QByteArray file = jpegWithExif(QSize(64, 48), exif.build());
    const QList<FileFact> facts = ImageMetadataReader::factsFor(file, QStringLiteral("liar.jpg"));

    QVERIFY2(factNamed(facts, QStringLiteral("Software")).isEmpty(), "a tag that lies says nothing");
    // And everything around it is untouched.
    QCOMPARE(factNamed(facts, QStringLiteral("Camera")), QStringLiteral("Canon EOS 5D Mark IV"));
    QCOMPARE(factNamed(facts, QStringLiteral("Exposure")), QStringLiteral("1/250 s"));
    QCOMPARE(factNamed(facts, QStringLiteral("Dimensions")), QStringLiteral("64 × 48"));

    // The same for a whole directory: the pointer to the Exif sub-IFD is a
    // number in the file too, and one that leads outside costs every tag in
    // that directory rather than the file.
    ExifBuilder bent(false);
    bent.addAscii(0x010f, "Nikon");
    bent.addExifRational(0x829a, 1, 60);
    bent.pointTagAt(0x8769, 0x7ffffff0);

    const QList<FileFact> facts2 = ImageMetadataReader::factsFor(
        jpegWithExif(QSize(32, 32), bent.build()), QStringLiteral("bent.jpg"));
    QCOMPARE(factNamed(facts2, QStringLiteral("Camera")), QStringLiteral("Nikon"));
    QVERIFY2(factNamed(facts2, QStringLiteral("Exposure")).isEmpty(),
        "a directory that cannot be reached contributes nothing, and nothing else suffers");
    QCOMPARE(factNamed(facts2, QStringLiteral("Dimensions")), QStringLiteral("32 × 32"));
}

void TestImageMetadata::aPhotographWithNoExifStillHasDimensions()
{
    const QByteArray file = jpegWithExif(QSize(320, 200), QByteArray());
    const QList<FileFact> facts = ImageMetadataReader::factsFor(file, QStringLiteral("plain.jpg"));

    QCOMPARE(factNamed(facts, QStringLiteral("Dimensions")), QStringLiteral("320 × 200"));
    QCOMPARE(factNamed(facts, QStringLiteral("Format")), QStringLiteral("JPEG"));
    QVERIFY(factNamed(facts, QStringLiteral("Camera")).isEmpty());
    QVERIFY(factNamed(facts, QStringLiteral("Exposure")).isEmpty());
}

void TestImageMetadata::aTruncatedFileReportsWhatIsThereAndInventsNothing()
{
    const QByteArray whole = jpegWithExif(QSize(640, 480), fullExif(false));

    // Cut in the middle of the EXIF block: what is readable is read, and the
    // tags whose values are past the cut are simply absent.
    for (const double fraction : { 0.05, 0.2, 0.5, 0.9 }) {
        const QByteArray cut = whole.left(qsizetype(whole.size() * fraction));
        const QList<FileFact> facts = ImageMetadataReader::factsFor(cut, QStringLiteral("cut.jpg"));
        for (const FileFact& fact : facts)
            QVERIFY2(!fact.value.isEmpty(), qPrintable(fact.label));
    }

    // Nothing recognisable at all is no facts rather than invented ones.
    QVERIFY(ImageMetadataReader::factsFor(QByteArray(2000, '\x01'), QStringLiteral("x.jpg")).isEmpty());
    QVERIFY(ImageMetadataReader::factsFor(QByteArray(), QStringLiteral("x.jpg")).isEmpty());
}

/// A prefix that ends in the middle of the six bytes that name the EXIF block.
///
/// The segment walk guarantees the marker and its length are present, and then
/// the EXIF test compared six bytes at the payload without checking that six
/// bytes are *there*: the length it had was the segment's own declared length,
/// which says nothing about where the prefix ends. QByteArrayView::sliced() only
/// asserts, so a release build read one to five bytes past the buffer -- and a
/// metadata reader is handed a bounded prefix of a file on a remote drive, so the
/// buffer ends exactly where the read stopped. A JPEG whose APP1 marker sits at
/// 4091 hits this by accident; a hostile file does it on purpose.
///
/// It cannot fail without a sanitizer, which is what `make asan` is in the gate
/// for -- so the case walks every cut rather than one, to give the read
/// somewhere to land. See ADR-0010 and MOLE-357.
void TestImageMetadata::aPrefixEndingInsideTheExifMarkerIsNotReadPast()
{
    const QByteArray whole = jpegWithExif(QSize(64, 48), fullExif(false));

    // Where the APP1 payload starts: FF E1, then two length bytes.
    qsizetype app1 = -1;
    for (qsizetype at = 2; at + 4 <= whole.size(); ++at) {
        if (static_cast<unsigned char>(whole.at(at)) == 0xff
            && static_cast<unsigned char>(whole.at(at + 1)) == 0xe1) {
            app1 = at;
            break;
        }
    }
    QVERIFY2(app1 >= 0, "the fixture has to carry an APP1 segment for this to be about anything");

    // Every cut from "the length is complete and nothing follows" to "one byte
    // of the marker is missing". The middle of those is the fault: six bytes are
    // compared where fewer than six exist.
    //
    // Over a buffer of exactly the right size, and that is the whole trick.
    // `whole.left(n)` is a QByteArray, and Qt allocates it room for a terminator
    // and usually more -- so a five-byte over-read lands inside the block and no
    // sanitizer says a word. A view over an exactly-sized allocation puts the
    // end of the buffer where the end of the data is, which is also what a
    // reader gets when the bytes came off a drive into a sized buffer.
    for (qsizetype missing = 6; missing >= 0; --missing) {
        const qsizetype size = app1 + 4 + (6 - missing);
        auto exact = std::make_unique<char[]>(size_t(size));
        std::memcpy(exact.get(), whole.constData(), size_t(size));
        const QByteArrayView cut(exact.get(), size);

        const QList<FileFact> facts = ImageMetadataReader::factsFor(cut, QStringLiteral("cut.jpg"));
        // Nothing is claimed about what it finds -- there is nothing to find. The
        // claim is that asking does not read past the end of the buffer, and it
        // is the sanitizer that holds it.
        for (const FileFact& fact : facts)
            QVERIFY2(!fact.value.isEmpty(), qPrintable(fact.label));
        // And the embedded-thumbnail path takes the same block.
        QVERIFY(ImageMetadataReader::embeddedThumbnail(cut).isEmpty());
    }
}

void TestImageMetadata::pngAndFriendsReportDimensionsAndDepth_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("format");
    QTest::addColumn<QString>("dimensions");
    QTest::addColumn<QString>("transparency");

    QTest::newRow("png with alpha") << pngOf(QSize(120, 90), QImage::Format_ARGB32) << "PNG" << "120 × 90"
                                    << "yes";
    QTest::newRow("png without") << pngOf(QSize(16, 16), QImage::Format_RGB32) << "PNG" << "16 × 16" << "no";
}

void TestImageMetadata::pngAndFriendsReportDimensionsAndDepth()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QString, format);
    QFETCH(QString, dimensions);
    QFETCH(QString, transparency);

    const QList<FileFact> facts = ImageMetadataReader::factsFor(bytes, QStringLiteral("picture.png"));
    QCOMPARE(factNamed(facts, QStringLiteral("Format")), format);
    QCOMPARE(factNamed(facts, QStringLiteral("Dimensions")), dimensions);
    QCOMPARE(factNamed(facts, QStringLiteral("Transparency")), transparency);
    QVERIFY(factNamed(facts, QStringLiteral("Colour depth")).endsWith(QStringLiteral("-bit")));
}

void TestImageMetadata::aHeaderIsEnoughHoweverLargeTheFile()
{
    // The claim that makes this a header read rather than a decode: the answer
    // from the first pages is the answer from the whole file.
    const QByteArray head = jpegWithExif(QSize(4000, 3000), fullExif(false));
    QByteArray huge = head;
    huge += QByteArray(8 * 1024 * 1024, '\x77'); // as if the rest of a 60 MB raw

    const QList<FileFact> fromHead = ImageMetadataReader::factsFor(
        head.left(ImageMetadataReader::kHeaderBytes), QStringLiteral("raw.jpg"));
    const QList<FileFact> fromWhole = ImageMetadataReader::factsFor(huge, QStringLiteral("raw.jpg"));

    QCOMPARE(fromHead.size(), fromWhole.size());
    QCOMPARE(factNamed(fromHead, QStringLiteral("Dimensions")), QStringLiteral("4000 × 3000"));
    QCOMPARE(factNamed(fromHead, QStringLiteral("Camera")), factNamed(fromWhole, QStringLiteral("Camera")));

    // And one page is not enough for this file, which is what the top-up read is
    // for -- it says so rather than answering with half a header.
    QVERIFY(ImageMetadataReader::wantsMore(head.left(64), QStringLiteral("raw.jpg")));
    QVERIFY(!ImageMetadataReader::wantsMore(head, QStringLiteral("raw.jpg")));
}

MOLE_TEST_MAIN(TestImageMetadata)
#include "tst_ImageMetadata.moc"
