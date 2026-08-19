#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QImageWriter>
#include <QList>
#include <QPainter>
#include <QSize>

#include <optional>

/// Image fixtures every test that reads a picture needs: an EXIF block written by
/// hand, and real files with it spliced in.
///
/// Shared rather than copied, because two hand-written EXIF writers in one suite
/// is two things to keep in step. Both the metadata readers and the thumbnailer
/// are tested against these.
namespace mole::test {

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

    /// The small picture a camera writes into IFD1, as the JPEG bytes it is.
    /// Adding one is what makes IFD1 exist at all.
    void addThumbnail(const QByteArray& jpeg) { m_thumbnail = jpeg; }
    /// Points IFD1's `JPEGInterchangeFormat` somewhere it has no business
    /// pointing, so a test can build the file a corrupt or hostile camera wrote.
    void pointThumbnailAt(quint32 offset) { m_thumbnailLie = offset; }

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
        // IFD1 holds two entries -- where the thumbnail is and how long it is --
        // and the thumbnail's own bytes sit straight after it, so the offset it
        // records is a number this layout knows before anything is written.
        const qint64 ifd1At = gpsAt + gpsBytes;
        const qint64 ifd1Bytes = m_thumbnail.isEmpty() ? 0 : 2 + 2 * 12 + 4;
        const qint64 thumbnailAt = ifd1At + ifd1Bytes;
        qint64 valuesAt = thumbnailAt + m_thumbnail.size();

        if (!m_exif.isEmpty())
            ifd0.append({ 0x8769, 4, packLong(quint32(exifAt)) });
        if (!m_gps.isEmpty())
            ifd0.append({ 0x8825, 4, packLong(quint32(gpsAt)) });

        QList<Field> ifd1;
        if (!m_thumbnail.isEmpty()) {
            const quint32 at = m_thumbnailLie.value_or(quint32(thumbnailAt));
            ifd1.append({ 0x0201, 4, packLong(at) });
            ifd1.append({ 0x0202, 4, packLong(quint32(m_thumbnail.size())) });
        }

        QByteArray values;
        QByteArray out = header;
        out += directory(ifd0, valuesAt, values, m_thumbnail.isEmpty() ? 0 : quint32(ifd1At));
        if (!m_exif.isEmpty())
            out += directory(m_exif, valuesAt, values);
        if (!m_gps.isEmpty())
            out += directory(m_gps, valuesAt, values);
        if (!m_thumbnail.isEmpty()) {
            out += directory(ifd1, valuesAt, values);
            out += m_thumbnail;
        }
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

    /// `nextAt` is the trailing next-IFD pointer: zero for a directory that is the
    /// last one, and where IFD1 starts for IFD0 when there is a thumbnail.
    QByteArray directory(
        const QList<Field>& fields, qint64& valuesAt, QByteArray& values, quint32 nextAt = 0) const
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
        out += packLong(nextAt);
        return out;
    }

    bool m_bigEndian = false;
    QList<Field> m_ifd0;
    QList<Field> m_exif;
    QList<Field> m_gps;
    QHash<quint16, quint32> m_liesAbout;
    QByteArray m_thumbnail;
    std::optional<quint32> m_thumbnailLie;
};

/// A picture that compresses like a photograph rather than like two rectangles,
/// so a test about what a file costs to read has a file worth reading. Textured
/// deterministically: a fixture that varies varies the answer with it.
inline QImage texturedImage(QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const int mixed = (x * 7 + y * 13 + ((x * y) % 97)) % 251;
            image.setPixel(x, y, qRgb(mixed, (mixed * 5) % 251, (mixed * 11) % 251));
        }
    }
    return image;
}

/// A real JPEG of `image`, encoded by Qt, with `exif` spliced in as its APP1
/// segment. Real bytes, because the dimensions have to come out of a header Qt
/// itself wrote rather than out of one this test invented.
inline QByteArray jpegOf(const QImage& image, const QByteArray& exif)
{
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

/// The same, of a flat two-rectangle picture: what most tests want, because they
/// are about the header and not about the pixels.
inline QByteArray jpegWithExif(QSize size, const QByteArray& exif)
{
    QImage image(size, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.fillRect(image.rect(), Qt::darkCyan);
    painter.fillRect(0, 0, size.width() / 2, size.height() / 2, Qt::yellow);
    painter.end();
    return jpegOf(image, exif);
}

inline QByteArray pngOf(QSize size, QImage::Format format)
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
} // namespace mole::test
