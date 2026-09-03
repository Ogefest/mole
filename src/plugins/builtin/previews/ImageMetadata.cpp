#include "plugins/builtin/previews/ImageMetadata.h"

#include "plugins/builtin/previews/PreviewProviders.h"

#include "core/vfs/VfsManager.h"

#include <QBuffer>
#include <QDateTime>
#include <QImageReader>
#include <QLocale>

#include <cmath>
#include <optional>

namespace mole {
namespace {

    // ---- reading a TIFF block without trusting a byte of it ---------------

    /// A bounds-checked view of the TIFF block inside an EXIF segment.
    ///
    /// Every offset in an IFD is relative to the start of this block and is a
    /// number the file chose. Nothing here reads without asking whether the
    /// range is inside the buffer first, so a file claiming its value lives at
    /// offset 2^31 costs one lookup that answers "no".
    class TiffBlock
    {
    public:
        TiffBlock(QByteArrayView bytes, bool bigEndian)
            : m_bytes(bytes)
            , m_bigEndian(bigEndian)
        {
        }

        bool has(qint64 offset, qint64 length) const
        {
            return offset >= 0 && length >= 0 && offset <= m_bytes.size() - length;
        }

        std::optional<quint16> u16(qint64 offset) const
        {
            if (!has(offset, 2))
                return std::nullopt;
            const auto a = static_cast<quint16>(static_cast<unsigned char>(m_bytes.at(offset)));
            const auto b = static_cast<quint16>(static_cast<unsigned char>(m_bytes.at(offset + 1)));
            return m_bigEndian ? static_cast<quint16>(a << 8 | b) : static_cast<quint16>(b << 8 | a);
        }

        std::optional<quint32> u32(qint64 offset) const
        {
            if (!has(offset, 4))
                return std::nullopt;
            quint32 value = 0;
            for (int i = 0; i < 4; ++i) {
                const auto byte = static_cast<quint32>(static_cast<unsigned char>(m_bytes.at(offset + i)));
                value |= m_bigEndian ? byte << (8 * (3 - i)) : byte << (8 * i);
            }
            return value;
        }

        QByteArrayView bytes() const { return m_bytes; }

    private:
        QByteArrayView m_bytes;
        bool m_bigEndian = false;
    };

    /// One IFD entry, already resolved to where its value actually is.
    struct Entry
    {
        quint16 tag = 0;
        quint16 type = 0;
        quint32 count = 0;
        /// Absolute within the TIFF block, and already checked to be inside it.
        qint64 valueOffset = 0;
        qint64 valueBytes = 0;
    };

    int bytesPerComponent(quint16 type)
    {
        switch (type) {
        case 1: // byte
        case 2: // ascii
        case 6: // signed byte
        case 7: // undefined
            return 1;
        case 3: // short
        case 8: // signed short
            return 2;
        case 4: // long
        case 9: // signed long
        case 11: // float
            return 4;
        case 5: // rational
        case 10: // signed rational
        case 12: // double
            return 8;
        default:
            return 0;
        }
    }

    /// The entries of one IFD, or nothing when the directory itself does not fit.
    QList<Entry> readDirectory(const TiffBlock& block, qint64 offset)
    {
        QList<Entry> entries;
        const std::optional<quint16> count = block.u16(offset);
        if (!count)
            return entries;

        for (quint16 i = 0; i < *count; ++i) {
            const qint64 at = offset + 2 + qint64(i) * 12;
            const std::optional<quint16> tag = block.u16(at);
            const std::optional<quint16> type = block.u16(at + 2);
            const std::optional<quint32> components = block.u32(at + 4);
            if (!tag || !type || !components)
                break;

            const int unit = bytesPerComponent(*type);
            if (unit == 0)
                continue;

            Entry entry;
            entry.tag = *tag;
            entry.type = *type;
            entry.count = *components;
            entry.valueBytes = qint64(*components) * unit;

            // Four bytes or fewer live in the entry itself; anything longer is
            // somewhere the file points at, and that is the number to distrust.
            if (entry.valueBytes <= 4) {
                entry.valueOffset = at + 8;
            } else {
                const std::optional<quint32> pointer = block.u32(at + 8);
                if (!pointer)
                    continue;
                entry.valueOffset = *pointer;
            }
            if (!block.has(entry.valueOffset, entry.valueBytes))
                continue; // a claim that leads outside the file costs this tag only

            entries.append(entry);
        }
        return entries;
    }

    const Entry* find(const QList<Entry>& entries, quint16 tag)
    {
        for (const Entry& entry : entries) {
            if (entry.tag == tag)
                return &entry;
        }
        return nullptr;
    }

    QString asText(const TiffBlock& block, const Entry& entry)
    {
        if (entry.type != 2)
            return {};
        QByteArray text(block.bytes().sliced(entry.valueOffset, entry.valueBytes).toByteArray());
        // ASCII values are NUL-terminated, and cameras pad with spaces.
        const qsizetype nul = text.indexOf('\0');
        if (nul >= 0)
            text.truncate(nul);
        return QString::fromLatin1(text).trimmed();
    }

    std::optional<double> asNumber(const TiffBlock& block, const Entry& entry, quint32 index = 0)
    {
        const int unit = bytesPerComponent(entry.type);
        if (unit == 0 || index >= entry.count)
            return std::nullopt;
        const qint64 at = entry.valueOffset + qint64(index) * unit;

        switch (entry.type) {
        case 1:
        case 7:
            return block.has(at, 1) ? std::optional<double>(static_cast<unsigned char>(block.bytes().at(at)))
                                    : std::nullopt;
        case 3:
            if (const std::optional<quint16> value = block.u16(at))
                return double(*value);
            return std::nullopt;
        case 4:
            if (const std::optional<quint32> value = block.u32(at))
                return double(*value);
            return std::nullopt;
        case 8:
            if (const std::optional<quint16> value = block.u16(at))
                return double(static_cast<qint16>(*value));
            return std::nullopt;
        case 9:
            if (const std::optional<quint32> value = block.u32(at))
                return double(static_cast<qint32>(*value));
            return std::nullopt;
        case 5:
        case 10: {
            const std::optional<quint32> numerator = block.u32(at);
            const std::optional<quint32> denominator = block.u32(at + 4);
            if (!numerator || !denominator || *denominator == 0)
                return std::nullopt;
            if (entry.type == 10) {
                return double(static_cast<qint32>(*numerator)) / double(static_cast<qint32>(*denominator));
            }
            return double(*numerator) / double(*denominator);
        }
        default:
            return std::nullopt;
        }
    }

    // ---- turning numbers into the words a camera uses ---------------------

    QString exposureText(double seconds)
    {
        if (seconds <= 0)
            return {};
        if (seconds >= 1.0)
            return QStringLiteral("%1 s").arg(seconds, 0, 'g', 3);
        return QStringLiteral("1/%1 s").arg(qRound(1.0 / seconds));
    }

    QString apertureText(double fNumber)
    {
        return fNumber > 0 ? QStringLiteral("f/%1").arg(fNumber, 0, 'g', 3) : QString();
    }

    QString biasText(double stops)
    {
        if (qFuzzyIsNull(stops))
            return QStringLiteral("0 EV");
        return QStringLiteral("%1%2 EV")
            .arg(stops > 0 ? QStringLiteral("+") : QString())
            .arg(stops, 0, 'g', 2);
    }

    QString orientationText(int orientation)
    {
        switch (orientation) {
        case 1:
            return QStringLiteral("normal");
        case 2:
            return QStringLiteral("mirrored");
        case 3:
            return QStringLiteral("rotated 180°");
        case 4:
            return QStringLiteral("mirrored, rotated 180°");
        case 5:
            return QStringLiteral("mirrored, rotated 90° anticlockwise");
        case 6:
            return QStringLiteral("rotated 90° clockwise");
        case 7:
            return QStringLiteral("mirrored, rotated 90° clockwise");
        case 8:
            return QStringLiteral("rotated 90° anticlockwise");
        default:
            return {};
        }
    }

    /// A date as the camera wrote it: "2026:03:14 09:31:07".
    QString dateText(const QString& exifDate)
    {
        const QDateTime when = QDateTime::fromString(exifDate, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
        return when.isValid() ? QLocale().toString(when, QLocale::ShortFormat) : exifDate;
    }

    /// Degrees, minutes and seconds as one number, signed by the reference.
    std::optional<double> coordinate(const TiffBlock& block, const Entry& value, const QString& reference)
    {
        if (value.count < 3)
            return std::nullopt;
        const std::optional<double> degrees = asNumber(block, value, 0);
        const std::optional<double> minutes = asNumber(block, value, 1);
        const std::optional<double> seconds = asNumber(block, value, 2);
        if (!degrees || !minutes || !seconds)
            return std::nullopt;

        double decimal = *degrees + *minutes / 60.0 + *seconds / 3600.0;
        if (reference == QLatin1String("S") || reference == QLatin1String("W"))
            decimal = -decimal;
        return decimal;
    }

    void appendIf(QList<FileFact>& facts, const QString& label, const QString& value)
    {
        if (!value.isEmpty())
            facts.append({ label, value });
    }

    // ---- finding the EXIF block -------------------------------------------

    /// The TIFF block inside a JPEG's APP1 segment, or inside a TIFF file, as an
    /// offset and a length within `bytes`. Empty when there is none.
    QByteArrayView exifBlock(QByteArrayView bytes)
    {
        static const QByteArray marker = QByteArrayLiteral("Exif\0\0");

        // A TIFF file is its own block.
        if (bytes.size() >= 4
            && (bytes.startsWith(QByteArrayLiteral("II\x2a\x00"))
                || bytes.startsWith(QByteArrayLiteral("MM\x00\x2a"))))
            return bytes;

        if (bytes.size() < 4 || static_cast<unsigned char>(bytes.at(0)) != 0xff
            || static_cast<unsigned char>(bytes.at(1)) != 0xd8)
            return {};

        // Walk the segments. Each is a marker and a big-endian length that
        // includes the length itself, and every one of those lengths is a claim.
        qint64 at = 2;
        while (at + 4 <= bytes.size()) {
            if (static_cast<unsigned char>(bytes.at(at)) != 0xff)
                return {};
            const auto marker8 = static_cast<unsigned char>(bytes.at(at + 1));
            if (marker8 == 0xd8 || marker8 == 0x01 || (marker8 >= 0xd0 && marker8 <= 0xd7)) {
                at += 2;
                continue;
            }
            if (marker8 == 0xda || marker8 == 0xd9)
                return {}; // the image data starts here; there is no EXIF

            const qint64 length = (static_cast<unsigned char>(bytes.at(at + 2)) << 8)
                | static_cast<unsigned char>(bytes.at(at + 3));
            if (length < 2)
                return {};

            const qint64 payload = at + 4;
            const qint64 payloadBytes = length - 2;
            // `payload + marker.size() <= bytes.size()` before the slice, and it is
            // a different question from the one beside it: payloadBytes is what
            // the *segment header claims*, and the prefix may end anywhere.
            // QByteArrayView::sliced() only asserts, so a release build read up
            // to five bytes past the buffer whenever the prefix ended just after
            // an FF E1 LL LL -- a JPEG whose APP1 marker sits at 4091 hits it by
            // accident, a hostile file on purpose. Every other slice in this file
            // is guarded; this one was not. See ADR-0010 and MOLE-357.
            if (marker8 == 0xe1 && payloadBytes > marker.size()
                && payload + marker.size() <= bytes.size()
                && bytes.sliced(payload, marker.size()) == QByteArrayView(marker)) {
                const qint64 start = payload + marker.size();
                // Truncated on the way in: take what is here rather than nothing,
                // and let every offset inside it be checked as usual.
                const qint64 available = std::min(payloadBytes - marker.size(), bytes.size() - start);
                return available > 0 ? bytes.sliced(start, available) : QByteArrayView();
            }
            at = payload + payloadBytes;
        }
        return {};
    }

    QList<FileFact> exifFacts(QByteArrayView block)
    {
        QList<FileFact> facts;
        if (block.size() < 8)
            return facts;

        const bool bigEndian = block.startsWith(QByteArrayLiteral("MM"));
        if (!bigEndian && !block.startsWith(QByteArrayLiteral("II")))
            return facts;

        const TiffBlock tiff(block, bigEndian);
        const std::optional<quint32> firstDirectory = tiff.u32(4);
        if (!firstDirectory)
            return facts;

        const QList<Entry> ifd0 = readDirectory(tiff, *firstDirectory);
        if (ifd0.isEmpty())
            return facts;

        QList<Entry> exif;
        if (const Entry* pointer = find(ifd0, 0x8769)) {
            if (const std::optional<double> offset = asNumber(tiff, *pointer))
                exif = readDirectory(tiff, qint64(*offset));
        }
        QList<Entry> gps;
        if (const Entry* pointer = find(ifd0, 0x8825)) {
            if (const std::optional<double> offset = asNumber(tiff, *pointer))
                gps = readDirectory(tiff, qint64(*offset));
        }

        const auto text = [&tiff](const QList<Entry>& entries, quint16 tag) {
            const Entry* entry = find(entries, tag);
            return entry ? asText(tiff, *entry) : QString();
        };
        const auto number = [&tiff](const QList<Entry>& entries, quint16 tag) -> std::optional<double> {
            const Entry* entry = find(entries, tag);
            return entry ? asNumber(tiff, *entry) : std::nullopt;
        };

        const QString make = text(ifd0, 0x010f);
        const QString model = text(ifd0, 0x0110);
        // "Canon Canon EOS 5D" is what the two fields literally say, because the
        // model usually repeats the make. Either alone is also a whole answer.
        QString camera = model;
        if (make.isEmpty())
            camera = model;
        else if (model.isEmpty())
            camera = make;
        else if (!model.startsWith(make, Qt::CaseInsensitive))
            camera = QStringLiteral("%1 %2").arg(make, model);
        if (!camera.isEmpty())
            facts.append({ QStringLiteral("Camera"), camera, QStringLiteral("image.camera") });
        if (const QString lens = text(exif, 0xa434); !lens.isEmpty())
            facts.append({ QStringLiteral("Lens"), lens, QStringLiteral("image.lens") });

        if (const std::optional<double> seconds = number(exif, 0x829a))
            appendIf(facts, QStringLiteral("Exposure"), exposureText(*seconds));
        if (const std::optional<double> fNumber = number(exif, 0x829d))
            appendIf(facts, QStringLiteral("Aperture"), apertureText(*fNumber));
        if (const std::optional<double> iso = number(exif, 0x8827)) {
            facts.append({ QStringLiteral("Sensitivity"), QStringLiteral("ISO %1").arg(qRound(*iso)),
                QStringLiteral("image.iso"), *iso });
        }
        if (const std::optional<double> focal = number(exif, 0x920a))
            appendIf(facts, QStringLiteral("Focal length"), QStringLiteral("%1 mm").arg(qRound(*focal)));
        if (const std::optional<double> bias = number(exif, 0x9204))
            appendIf(facts, QStringLiteral("Exposure bias"), biasText(*bias));
        if (const std::optional<double> flash = number(exif, 0x9209)) {
            appendIf(facts, QStringLiteral("Flash"),
                (qRound(*flash) & 1) ? QStringLiteral("fired") : QStringLiteral("did not fire"));
        }

        const QString taken = text(exif, 0x9003);
        const QString when = taken.isEmpty() ? text(ifd0, 0x0132) : taken;
        if (const QDateTime moment = QDateTime::fromString(when, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
            moment.isValid()) {
            // Seconds since the epoch as the number, so "taken last summer" is
            // a range in SQL rather than string comparisons on a date format.
            facts.append({ QStringLiteral("Taken"), dateText(when), QStringLiteral("image.taken"),
                double(moment.toSecsSinceEpoch()) });
        }

        if (const std::optional<double> orientation = number(ifd0, 0x0112))
            appendIf(facts, QStringLiteral("Orientation"), orientationText(qRound(*orientation)));
        appendIf(facts, QStringLiteral("Software"), text(ifd0, 0x0131));

        // Numbers only, and nothing looked up anywhere.
        if (const Entry* latitude = find(gps, 0x0002)) {
            if (const Entry* longitude = find(gps, 0x0004)) {
                const std::optional<double> north = coordinate(tiff, *latitude, text(gps, 0x0001));
                const std::optional<double> east = coordinate(tiff, *longitude, text(gps, 0x0003));
                if (north && east) {
                    appendIf(facts, QStringLiteral("Position"),
                        QStringLiteral("%1, %2").arg(*north, 0, 'f', 5).arg(*east, 0, 'f', 5));
                }
            }
        }
        if (const std::optional<double> altitude = number(gps, 0x0006)) {
            const std::optional<double> below = number(gps, 0x0005);
            const double metres = below && qRound(*below) == 1 ? -*altitude : *altitude;
            appendIf(facts, QStringLiteral("Altitude"), QStringLiteral("%1 m").arg(metres, 0, 'f', 0));
        }

        return facts;
    }

} // namespace

bool ImageMetadataReader::canRead(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;
    if (entry.mimeType.startsWith(QLatin1String("image/")))
        return true;
    static const QStringList suffixes = ImagePreviewProvider::imageSuffixes();
    return suffixes.contains(entry.uri.suffix());
}

QList<FileFact> ImageMetadataReader::factsFor(QByteArrayView bytes, const QString& fileName)
{
    QList<FileFact> facts;
    if (bytes.isEmpty())
        return facts;

    // A buffer, not a file: QImageReader reads the header out of what it is
    // given and stops. A prefix too short to hold the header answers nothing,
    // which is what wantsMore() is for.
    QByteArray buffer = QByteArray::fromRawData(bytes.data(), bytes.size());
    QBuffer device(&buffer);
    device.open(QIODevice::ReadOnly);

    QImageReader reader(&device);
    reader.setDecideFormatFromContent(true);

    const QByteArray format = reader.format();
    if (!format.isEmpty())
        facts.append({ QStringLiteral("Format"), QString::fromLatin1(format).toUpper() });

    if (reader.supportsOption(QImageIOHandler::Size)) {
        const QSize size = reader.size();
        if (size.isValid()) {
            facts.append({ QStringLiteral("Dimensions"),
                QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()), QStringLiteral("image.width"),
                double(size.width()) });
            facts.append({ QStringLiteral("Height"), QString::number(size.height()),
                QStringLiteral("image.height"), double(size.height()) });
        }
    }

    const QImage::Format pixels = reader.imageFormat();
    if (pixels != QImage::Format_Invalid) {
        const int depth = QImage(1, 1, pixels).depth();
        if (depth > 0)
            facts.append({ QStringLiteral("Colour depth"), QStringLiteral("%1-bit").arg(depth) });
        facts.append({ QStringLiteral("Transparency"),
            QImage(1, 1, pixels).hasAlphaChannel() ? QStringLiteral("yes") : QStringLiteral("no") });
    }

    Q_UNUSED(fileName);
    facts.append(exifFacts(exifBlock(bytes)));
    return facts;
}

QByteArray ImageMetadataReader::embeddedThumbnail(QByteArrayView bytes)
{
    const QByteArrayView block = exifBlock(bytes);
    if (block.size() < 8)
        return {};

    const bool bigEndian = block.startsWith(QByteArrayLiteral("MM"));
    if (!bigEndian && !block.startsWith(QByteArrayLiteral("II")))
        return {};
    const TiffBlock tiff(block, bigEndian);

    const std::optional<quint32> firstDirectory = tiff.u32(4);
    if (!firstDirectory)
        return {};

    // IFD1 is where the thumbnail lives, and where it is is the last four bytes of
    // IFD0 -- a number the file chose, like every other one here.
    const std::optional<quint16> ifd0Count = tiff.u16(*firstDirectory);
    if (!ifd0Count)
        return {};
    const qint64 nextPointerAt = qint64(*firstDirectory) + 2 + qint64(*ifd0Count) * 12;
    const std::optional<quint32> ifd1At = tiff.u32(nextPointerAt);
    if (!ifd1At || *ifd1At == 0)
        return {}; // no second directory, which is most files that are not photographs

    const QList<Entry> ifd1 = readDirectory(tiff, *ifd1At);
    if (ifd1.isEmpty())
        return {};

    // JPEGInterchangeFormat and its length. Both have to be there and both have
    // to point inside the block, or there is no thumbnail as far as this is
    // concerned.
    const Entry* at = find(ifd1, 0x0201);
    const Entry* length = find(ifd1, 0x0202);
    if (!at || !length)
        return {};
    const std::optional<double> offset = asNumber(tiff, *at);
    const std::optional<double> count = asNumber(tiff, *length);
    if (!offset || !count || *offset < 0 || *count <= 0)
        return {};

    const qint64 from = qint64(*offset);
    const qint64 size = qint64(*count);
    if (!tiff.has(from, size))
        return {}; // an offset in a file is a claim, not a promise

    QByteArray thumbnail = block.sliced(from, size).toByteArray();
    // A complete JPEG or nothing: a truncated one would be a broken tile rather
    // than a missing one.
    if (thumbnail.size() < 4 || static_cast<unsigned char>(thumbnail.at(0)) != 0xff
        || static_cast<unsigned char>(thumbnail.at(1)) != 0xd8) {
        return {};
    }
    return thumbnail;
}

bool ImageMetadataReader::wantsMore(QByteArrayView bytes, const QString& fileName)
{
    Q_UNUSED(fileName);
    if (bytes.isEmpty())
        return true;

    QByteArray buffer = QByteArray::fromRawData(bytes.data(), bytes.size());
    QBuffer device(&buffer);
    device.open(QIODevice::ReadOnly);
    QImageReader reader(&device);
    reader.setDecideFormatFromContent(true);

    // No dimensions yet: the header is longer than what is here.
    if (!reader.size().isValid())
        return true;

    // A JPEG whose EXIF segment we never reached. Every APP1 a camera writes is
    // near the front, so this is only ever the difference between one page and
    // sixteen.
    return exifBlock(bytes).isEmpty() && bytes.size() >= 2 && static_cast<unsigned char>(bytes.at(0)) == 0xff
        && static_cast<unsigned char>(bytes.at(1)) == 0xd8;
}

QList<FileFact> ImageMetadataReader::read(
    const FileEntry& entry, QByteArrayView head, PluginServices services, const CancelToken& cancel) const
{
    QByteArray owned;
    QByteArrayView bytes = head;

    // One top-up, bounded, and only when the prefix in hand cannot answer. The
    // rest of the file is never touched however large it is.
    if (wantsMore(bytes, entry.name) && services.vfs && head.size() < kHeaderBytes) {
        if (FileSystemPtr fs = services.vfs->resolve(entry.uri)) {
            Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
            if (opened.ok() && opened.value()) {
                owned = opened.value()->read(kHeaderBytes);
                if (owned.size() > head.size())
                    bytes = QByteArrayView(owned);
            }
        }
    }
    if (cancel.isCancelled())
        return {};

    return factsFor(bytes, entry.name);
}

} // namespace mole
