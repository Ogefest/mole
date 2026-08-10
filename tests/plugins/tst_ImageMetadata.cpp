#include "plugins/builtin/previews/ImageMetadata.h"
#include "support/MoleTestMain.h"

#include <QBuffer>
#include <QImage>
#include <QImageWriter>
#include <QPainter>

using namespace mole;

namespace {

/// A TIFF block written by hand, so a test can say exactly what is in a file --
/// including the things a camera would never write.
///
/// Nothing here is committed as a fixture: a binary blob in the tree is one
/// nobody can review, and the bytes that matter about an EXIF block are the
/// twenty a test can spell out.
class ExifBuilder
{
public:
    explicit ExifBuilder(bool bigEndian)
        : m_bigEndian(bigEndian)
    {
    }

    /// A value short enough to live in the entry, or long enough to be pointed
    /// at -- the builder decides the same way a camera does.
    void addAscii(quint16 tag, const QByteArray& text) { m_ifd0.append({ tag, 2, text + '\0' }); }
    void addShort(quint16 tag, quint16 value) { m_ifd0.append({ tag, 3, packShort(value) }); }
    void addExifAscii(quint16 tag, const QByteArray& text) { m_exif.append({ tag, 2, text + '\0' }); }
    void addExifShort(quint16 tag, quint16 value) { m_exif.append({ tag, 3, packShort(value) }); }
    void addExifRational(quint16 tag, quint32 numerator, quint32 denominator)
    {
        m_exif.append({ tag, 5, packLong(numerator) + packLong(denominator) });
    }
    void addExifSignedRational(quint16 tag, qint32 numerator, qint32 denominator)
    {
        m_exif.append({ tag, 10, packLong(quint32(numerator)) + packLong(quint32(denominator)) });
    }
    void addGpsAscii(quint16 tag, const QByteArray& text) { m_gps.append({ tag, 2, text + '\0' }); }
    void addGpsCoordinate(quint16 tag, int degrees, int minutes, double seconds)
    {
        QByteArray value;
        value += packLong(quint32(degrees)) + packLong(1);
        value += packLong(quint32(minutes)) + packLong(1);
        value += packLong(quint32(qRound(seconds * 100))) + packLong(100);
        m_gps.append({ tag, 5, value });
    }
    void addGpsRational(quint16 tag, quint32 numerator, quint32 denominator)
    {
        m_gps.append({ tag, 5, packLong(numerator) + packLong(denominator) });
    }

    /// Overrides the value offset of the next tag added to IFD0, so a test can
    /// build a file that points somewhere it has no business pointing.
    void pointTagAt(quint16 tag, quint32 offset) { m_liesAbout.insert(tag, offset); }

    QByteArray build() const
    {
        // Layout: header, IFD0 (plus its pointers), the Exif IFD, the GPS IFD,
        // then every value too long to live in an entry.
        QByteArray header = m_bigEndian ? QByteArrayLiteral("MM\x00\x2a") : QByteArrayLiteral("II\x2a\x00");
        header += packLong(8);

        QList<Field> ifd0 = m_ifd0;
        const qint64 ifd0At = 8;
        const qint64 ifd0Bytes = 2 + qint64(ifd0.size() + m_pointerCount()) * 12 + 4;
        const qint64 exifAt = ifd0At + ifd0Bytes;
        const qint64 exifBytes = m_exif.isEmpty() ? 0 : 2 + qint64(m_exif.size()) * 12 + 4;
        const qint64 gpsAt = exifAt + exifBytes;
        const qint64 gpsBytes = m_gps.isEmpty() ? 0 : 2 + qint64(m_gps.size()) * 12 + 4;
        qint64 valuesAt = gpsAt + gpsBytes;

        if (!m_exif.isEmpty())
            ifd0.append({ 0x8769, 4, packLong(quint32(exifAt)) });
        if (!m_gps.isEmpty())
            ifd0.append({ 0x8825, 4, packLong(quint32(gpsAt)) });

        QByteArray values;
        QByteArray out = header;
        out += directory(ifd0, valuesAt, values);
        if (!m_exif.isEmpty())
            out += directory(m_exif, valuesAt, values);
        if (!m_gps.isEmpty())
            out += directory(m_gps, valuesAt, values);
        out += values;
        return out;
    }

private:
    struct Field
    {
        quint16 tag = 0;
        quint16 type = 0;
        QByteArray value;
    };

    int m_pointerCount() const { return (m_exif.isEmpty() ? 0 : 1) + (m_gps.isEmpty() ? 0 : 1); }

    static int unitFor(quint16 type)
    {
        switch (type) {
        case 2:
            return 1;
        case 3:
            return 2;
        case 4:
            return 4;
        case 5:
        case 10:
            return 8;
        default:
            return 1;
        }
    }

    QByteArray packShort(quint16 value) const
    {
        QByteArray out(2, '\0');
        if (m_bigEndian) {
            out[0] = char(value >> 8);
            out[1] = char(value & 0xff);
        } else {
            out[0] = char(value & 0xff);
            out[1] = char(value >> 8);
        }
        return out;
    }

    QByteArray packLong(quint32 value) const
    {
        QByteArray out(4, '\0');
        for (int i = 0; i < 4; ++i)
            out[i] = char((value >> (m_bigEndian ? 8 * (3 - i) : 8 * i)) & 0xff);
        return out;
    }

    QByteArray directory(const QList<Field>& fields, qint64& valuesAt, QByteArray& values) const
    {
        QByteArray out = packShort(quint16(fields.size()));
        for (const Field& field : fields) {
            out += packShort(field.tag);
            out += packShort(field.type);
            out += packLong(quint32(field.value.size() / unitFor(field.type)));

            if (const auto lie = m_liesAbout.constFind(field.tag); lie != m_liesAbout.constEnd()) {
                out += packLong(*lie);
                continue;
            }
            if (field.value.size() <= 4) {
                QByteArray padded = field.value;
                padded.resize(4, '\0');
                out += padded;
            } else {
                out += packLong(quint32(valuesAt + values.size()));
                values += field.value;
                if (values.size() % 2)
                    values += '\0';
            }
        }
        out += packLong(0);
        return out;
    }

    bool m_bigEndian = false;
    QList<Field> m_ifd0;
    QList<Field> m_exif;
    QList<Field> m_gps;
    QHash<quint16, quint32> m_liesAbout;
};

/// A real JPEG of `size`, encoded by Qt, with `exif` spliced in as its APP1
/// segment. Real bytes, because the dimensions have to come out of a header Qt
/// itself wrote rather than out of one this test invented.
QByteArray jpegWithExif(QSize size, const QByteArray& exif)
{
    QImage image(size, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.fillRect(image.rect(), Qt::darkCyan);
    painter.fillRect(0, 0, size.width() / 2, size.height() / 2, Qt::yellow);
    painter.end();

    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.write(image);
    buffer.close();

    if (exif.isEmpty())
        return encoded;

    QByteArray segment = QByteArray("Exif\0\0", 6) + exif;
    const int length = int(segment.size()) + 2;
    QByteArray app1;
    app1 += char(0xff);
    app1 += char(0xe1);
    app1 += char((length >> 8) & 0xff);
    app1 += char(length & 0xff);
    app1 += segment;

    // Straight after the start-of-image marker, which is where a camera puts it.
    return encoded.left(2) + app1 + encoded.mid(2);
}

QByteArray pngOf(QSize size, QImage::Format format)
{
    QImage image(size, format);
    image.fill(Qt::transparent);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "png");
    writer.write(image);
    return encoded;
}

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
