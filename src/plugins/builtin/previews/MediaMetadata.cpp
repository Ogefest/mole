#include "plugins/builtin/previews/MediaMetadata.h"

#include "core/vfs/VfsManager.h"

#include <QHash>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace mole {
namespace {

    // ---- reading numbers out of a buffer that may be lying ----------------

    bool fits(QByteArrayView bytes, qint64 offset, qint64 length)
    {
        return offset >= 0 && length >= 0 && offset <= bytes.size() - length;
    }

    std::optional<quint32> beU32(QByteArrayView bytes, qint64 offset)
    {
        if (!fits(bytes, offset, 4))
            return std::nullopt;
        quint32 value = 0;
        for (int i = 0; i < 4; ++i)
            value = value << 8 | static_cast<unsigned char>(bytes.at(offset + i));
        return value;
    }

    std::optional<quint16> beU16(QByteArrayView bytes, qint64 offset)
    {
        if (!fits(bytes, offset, 2))
            return std::nullopt;
        return quint16(static_cast<unsigned char>(bytes.at(offset)) << 8
            | static_cast<unsigned char>(bytes.at(offset + 1)));
    }

    std::optional<quint64> beU64(QByteArrayView bytes, qint64 offset)
    {
        if (!fits(bytes, offset, 8))
            return std::nullopt;
        quint64 value = 0;
        for (int i = 0; i < 8; ++i)
            value = value << 8 | static_cast<unsigned char>(bytes.at(offset + i));
        return value;
    }

    std::optional<quint32> leU32(QByteArrayView bytes, qint64 offset)
    {
        if (!fits(bytes, offset, 4))
            return std::nullopt;
        quint32 value = 0;
        for (int i = 0; i < 4; ++i)
            value |= quint32(static_cast<unsigned char>(bytes.at(offset + i))) << (8 * i);
        return value;
    }

    QByteArray fourCharacters(QByteArrayView bytes, qint64 offset)
    {
        if (!fits(bytes, offset, 4))
            return {};
        return bytes.sliced(offset, 4).toByteArray();
    }

    void appendIf(QList<FileFact>& facts, const QString& label, const QString& value)
    {
        if (!value.isEmpty())
            facts.append({ label, value });
    }

    // ---- ISO base media ---------------------------------------------------

    /// The facts of one track, so a file's tracks can be counted and the video
    /// one picked out.
    struct Track
    {
        QByteArray handler;
        QString codec;
        int width = 0;
        int height = 0;
        double seconds = 0;
        qint64 samples = 0;
    };

    std::optional<double> isoDuration(QByteArrayView bytes, const IsoBox& header)
    {
        const std::optional<quint32> versionAndFlags = beU32(bytes, header.payloadOffset());
        if (!versionAndFlags)
            return std::nullopt;
        const quint32 version = *versionAndFlags >> 24;

        // Version 1 widens the two timestamps and the duration; everything
        // before the timescale moves with them.
        const qint64 at = header.payloadOffset() + (version == 1 ? 4 + 8 + 8 : 4 + 4 + 4);
        const std::optional<quint32> timescale = beU32(bytes, at);
        if (!timescale || *timescale == 0)
            return std::nullopt;

        if (version == 1) {
            const std::optional<quint64> ticks = beU64(bytes, at + 4);
            return ticks ? std::optional<double>(double(*ticks) / double(*timescale)) : std::nullopt;
        }
        const std::optional<quint32> ticks = beU32(bytes, at + 4);
        return ticks ? std::optional<double>(double(*ticks) / double(*timescale)) : std::nullopt;
    }

    void readTrack(QByteArrayView bytes, const IsoBox& trak, Track& track)
    {
        if (const std::optional<IsoBox> tkhd
            = isoBoxNamed(bytes, trak.payloadOffset(), trak.payloadBytes(), "tkhd")) {
            const std::optional<quint32> versionAndFlags = beU32(bytes, tkhd->payloadOffset());
            const quint32 version = versionAndFlags ? *versionAndFlags >> 24 : 0;
            // Everything up to the matrix is fixed width, and only the two
            // timestamps and the duration change size with the version: 12 bytes
            // more in version 1, which is what moves the picture size along.
            const qint64 at = tkhd->payloadOffset() + (version == 1 ? 88 : 76);
            if (const std::optional<quint32> width = beU32(bytes, at))
                track.width = int(*width >> 16); // 16.16 fixed point
            if (const std::optional<quint32> height = beU32(bytes, at + 4))
                track.height = int(*height >> 16);
        }

        const std::optional<IsoBox> mdia
            = isoBoxNamed(bytes, trak.payloadOffset(), trak.payloadBytes(), "mdia");
        if (!mdia)
            return;

        if (const std::optional<IsoBox> mdhd
            = isoBoxNamed(bytes, mdia->payloadOffset(), mdia->payloadBytes(), "mdhd")) {
            if (const std::optional<double> seconds = isoDuration(bytes, *mdhd))
                track.seconds = *seconds;
        }
        if (const std::optional<IsoBox> hdlr
            = isoBoxNamed(bytes, mdia->payloadOffset(), mdia->payloadBytes(), "hdlr")) {
            track.handler = fourCharacters(bytes, hdlr->payloadOffset() + 8);
        }

        const std::optional<IsoBox> minf
            = isoBoxNamed(bytes, mdia->payloadOffset(), mdia->payloadBytes(), "minf");
        if (!minf)
            return;
        const std::optional<IsoBox> table
            = isoBoxNamed(bytes, minf->payloadOffset(), minf->payloadBytes(), "stbl");
        if (!table)
            return;

        if (const std::optional<IsoBox> stsd
            = isoBoxNamed(bytes, table->payloadOffset(), table->payloadBytes(), "stsd")) {
            // A full box: four bytes of version and flags, four of entry count,
            // and then the sample entries, each a box of its own.
            const QList<IsoBox> entries
                = isoBoxesIn(bytes, stsd->payloadOffset() + 8, stsd->payloadBytes() - 8);
            if (!entries.isEmpty())
                track.codec = QString::fromLatin1(entries.first().type);
        }
        if (const std::optional<IsoBox> stsz
            = isoBoxNamed(bytes, table->payloadOffset(), table->payloadBytes(), "stsz")) {
            if (const std::optional<quint32> count = beU32(bytes, stsz->payloadOffset() + 8))
                track.samples = *count;
        }
    }

    QList<FileFact> isoFactsFrom(QByteArrayView bytes, const IsoBox* moovBox)
    {
        QList<FileFact> facts;
        const std::optional<IsoBox> found
            = moovBox ? std::nullopt : isoBoxAt(bytes, { QByteArrayLiteral("moov") });
        const IsoBox* moov = moovBox ? moovBox : (found ? &*found : nullptr);
        if (!moov)
            return facts;

        double seconds = 0;
        if (const std::optional<IsoBox> mvhd
            = isoBoxNamed(bytes, moov->payloadOffset(), moov->payloadBytes(), "mvhd")) {
            if (const std::optional<double> duration = isoDuration(bytes, *mvhd))
                seconds = *duration;
        }

        QList<Track> tracks;
        const QList<IsoBox> children = isoBoxesIn(bytes, moov->payloadOffset(), moov->payloadBytes());
        for (const IsoBox& child : children) {
            if (child.type != QByteArrayLiteral("trak"))
                continue;
            Track track;
            readTrack(bytes, child, track);
            tracks.append(track);
        }

        const Track* picture = nullptr;
        for (const Track& track : tracks) {
            if (track.handler == QByteArrayLiteral("vide")) {
                picture = &track;
                break;
            }
        }

        if (seconds > 0) {
            facts.append({ QStringLiteral("Duration"), durationText(seconds),
                QStringLiteral("media.duration"), seconds });
        }
        if (picture && picture->width > 0 && picture->height > 0) {
            facts.append({ QStringLiteral("Picture"),
                QStringLiteral("%1 × %2").arg(picture->width).arg(picture->height) });
        }
        if (picture && picture->samples > 0 && picture->seconds > 0) {
            facts.append({ QStringLiteral("Frame rate"),
                QStringLiteral("%1 fps").arg(double(picture->samples) / picture->seconds, 0, 'g', 3) });
        }

        QStringList codecs;
        for (const Track& track : tracks) {
            if (!track.codec.isEmpty())
                codecs.append(codecName(track.codec));
        }
        if (!codecs.isEmpty())
            facts.append({ QStringLiteral("Codecs"), codecs.join(QStringLiteral(", ")),
                QStringLiteral("media.codec") });
        if (!tracks.isEmpty())
            facts.append({ QStringLiteral("Tracks"), QString::number(tracks.size()) });
        return facts;
    }

    // ---- Matroska and WebM ------------------------------------------------

    /// An EBML variable-length integer: the first set bit says how many bytes it
    /// occupies. Returns the value and how far to advance, or nothing when the
    /// buffer cannot hold it.
    struct Variable
    {
        quint64 value = 0;
        int length = 0;
    };

    std::optional<Variable> ebmlNumber(QByteArrayView bytes, qint64 offset, bool keepMarker)
    {
        if (!fits(bytes, offset, 1))
            return std::nullopt;
        const auto first = static_cast<unsigned char>(bytes.at(offset));
        if (first == 0)
            return std::nullopt;

        int length = 1;
        unsigned char mask = 0x80;
        while (!(first & mask) && length < 8) {
            mask >>= 1;
            ++length;
        }
        if (!fits(bytes, offset, length))
            return std::nullopt;

        quint64 value = keepMarker ? first : quint64(first & ~mask);
        for (int i = 1; i < length; ++i)
            value = value << 8 | static_cast<unsigned char>(bytes.at(offset + i));
        return Variable { value, length };
    }

    /// How deep a container walk will go before it stops.
    ///
    /// The real structures are four levels deep. A file that nests Segment inside
    /// Segment costs five bytes a level, so a 64 kB prefix buys about thirteen
    /// thousand frames of recursion, each one carrying a std::function call --
    /// survivable on an 8 MB Linux stack and not on a 1 MB Windows one, and this
    /// runs on bytes off a remote drive. A depth nothing real reaches is not a
    /// limitation. See MOLE-357.
    constexpr int kMaxContainerDepth = 8;

    /// Calls `visit` for every element directly inside a range. `visit` returns
    /// true to descend into that element.
    void ebmlWalk(QByteArrayView bytes, qint64 offset, qint64 length,
        const std::function<bool(quint64, qint64, qint64)>& visit, int depth = 0)
    {
        if (depth >= kMaxContainerDepth)
            return;
        qint64 at = offset;
        const qint64 end = offset + length;
        while (at < end) {
            const std::optional<Variable> id = ebmlNumber(bytes, at, true);
            if (!id)
                return;
            const std::optional<Variable> size = ebmlNumber(bytes, at + id->length, false);
            if (!size)
                return;

            const qint64 payload = at + id->length + size->length;
            // An element claiming to run past the buffer is refused, and so is
            // the "unknown size" form: neither can be walked safely here.
            qint64 payloadBytes = qint64(size->value);
            if (payloadBytes < 0 || !fits(bytes, payload, payloadBytes))
                payloadBytes = std::min<qint64>(end, bytes.size()) - payload;
            if (payloadBytes < 0)
                return;

            if (visit(id->value, payload, payloadBytes))
                ebmlWalk(bytes, payload, payloadBytes, visit, depth + 1);
            at = payload + payloadBytes;
        }
    }

    double ebmlFloat(QByteArrayView bytes, qint64 offset, qint64 length)
    {
        if (length == 4) {
            const std::optional<quint32> raw = beU32(bytes, offset);
            if (!raw)
                return 0;
            float value = 0;
            std::memcpy(&value, &*raw, 4);
            return double(value);
        }
        if (length == 8) {
            const std::optional<quint64> raw = beU64(bytes, offset);
            if (!raw)
                return 0;
            double value = 0;
            std::memcpy(&value, &*raw, 8);
            return value;
        }
        return 0;
    }

    quint64 ebmlUnsigned(QByteArrayView bytes, qint64 offset, qint64 length)
    {
        if (length <= 0 || length > 8 || !fits(bytes, offset, length))
            return 0;
        quint64 value = 0;
        for (qint64 i = 0; i < length; ++i)
            value = value << 8 | static_cast<unsigned char>(bytes.at(offset + i));
        return value;
    }

    QList<FileFact> matroskaFacts(QByteArrayView bytes)
    {
        QList<FileFact> facts;

        double timecodeScale = 1000000.0; // nanoseconds, the default
        double durationTicks = 0;
        int width = 0;
        int height = 0;
        quint64 defaultDuration = 0;
        QStringList codecs;
        int tracks = 0;

        ebmlWalk(bytes, 0, bytes.size(), [&](quint64 id, qint64 offset, qint64 length) {
            switch (id) {
            case 0x18538067: // Segment
            case 0x1549A966: // Info
            case 0x1654AE6B: // Tracks
            case 0xE0: // Video
                return true;
            case 0xAE: // TrackEntry
                ++tracks;
                return true;
            case 0x2AD7B1:
                timecodeScale = double(ebmlUnsigned(bytes, offset, length));
                return false;
            case 0x4489:
                durationTicks = ebmlFloat(bytes, offset, length);
                return false;
            case 0xB0:
                width = int(ebmlUnsigned(bytes, offset, length));
                return false;
            case 0xBA:
                height = int(ebmlUnsigned(bytes, offset, length));
                return false;
            case 0x23E383:
                defaultDuration = ebmlUnsigned(bytes, offset, length);
                return false;
            case 0x86: // CodecID
                if (fits(bytes, offset, length))
                    codecs.append(codecName(QString::fromLatin1(bytes.sliced(offset, length).toByteArray())));
                return false;
            default:
                return false;
            }
        });

        if (durationTicks > 0 && timecodeScale > 0)
            appendIf(facts, QStringLiteral("Duration"), durationText(durationTicks * timecodeScale / 1e9));
        if (width > 0 && height > 0)
            facts.append({ QStringLiteral("Picture"), QStringLiteral("%1 × %2").arg(width).arg(height) });
        if (defaultDuration > 0) {
            facts.append({ QStringLiteral("Frame rate"),
                QStringLiteral("%1 fps").arg(1e9 / double(defaultDuration), 0, 'g', 3) });
        }
        if (!codecs.isEmpty())
            facts.append({ QStringLiteral("Codecs"), codecs.join(QStringLiteral(", ")) });
        if (tracks > 0)
            facts.append({ QStringLiteral("Tracks"), QString::number(tracks) });
        return facts;
    }

    // ---- AVI --------------------------------------------------------------

    QList<FileFact> aviFacts(QByteArrayView bytes)
    {
        QList<FileFact> facts;

        // RIFF chunks: a four-character type, a little-endian length, and a
        // payload -- with LIST chunks carrying a second type before theirs.
        double seconds = 0;
        double fps = 0;
        int width = 0;
        int height = 0;
        int streams = 0;
        QStringList codecs;

        const std::function<void(qint64, qint64, int)> walk = [&](qint64 offset, qint64 end, int depth) {
            if (depth >= kMaxContainerDepth)
                return;
            qint64 at = offset;
            while (at + 8 <= end) {
                const QByteArray type = fourCharacters(bytes, at);
                const std::optional<quint32> length = leU32(bytes, at + 4);
                if (type.isEmpty() || !length)
                    return;
                const qint64 payload = at + 8;
                const qint64 payloadBytes = qint64(*length);
                if (!fits(bytes, payload, std::min<qint64>(payloadBytes, bytes.size() - payload)))
                    return;
                const qint64 available = std::min<qint64>(payloadBytes, bytes.size() - payload);

                if (type == QByteArrayLiteral("LIST") || type == QByteArrayLiteral("RIFF")) {
                    walk(payload + 4, payload + available, depth + 1);
                } else if (type == QByteArrayLiteral("avih") && available >= 40) {
                    const std::optional<quint32> microseconds = leU32(bytes, payload);
                    const std::optional<quint32> frames = leU32(bytes, payload + 16);
                    const std::optional<quint32> streamCount = leU32(bytes, payload + 24);
                    const std::optional<quint32> w = leU32(bytes, payload + 32);
                    const std::optional<quint32> h = leU32(bytes, payload + 36);
                    if (microseconds && *microseconds > 0) {
                        fps = 1e6 / double(*microseconds);
                        if (frames)
                            seconds = double(*frames) * double(*microseconds) / 1e6;
                    }
                    if (streamCount)
                        streams = int(*streamCount);
                    if (w)
                        width = int(*w);
                    if (h)
                        height = int(*h);
                } else if (type == QByteArrayLiteral("strh") && available >= 8) {
                    const QByteArray handler = fourCharacters(bytes, payload + 4);
                    if (!handler.isEmpty() && handler != QByteArray(4, '\0'))
                        codecs.append(codecName(QString::fromLatin1(handler)));
                }
                // Chunks are padded to an even length.
                at = payload + payloadBytes + (payloadBytes % 2);
            }
        };
        walk(0, bytes.size(), 0);

        appendIf(facts, QStringLiteral("Duration"), durationText(seconds));
        if (width > 0 && height > 0)
            facts.append({ QStringLiteral("Picture"), QStringLiteral("%1 × %2").arg(width).arg(height) });
        if (fps > 0)
            facts.append({ QStringLiteral("Frame rate"), QStringLiteral("%1 fps").arg(fps, 0, 'g', 3) });
        if (!codecs.isEmpty())
            facts.append({ QStringLiteral("Codecs"), codecs.join(QStringLiteral(", ")) });
        if (streams > 0)
            facts.append({ QStringLiteral("Tracks"), QString::number(streams) });
        return facts;
    }

    /// The index of a file whose window does not start at a box boundary.
    ///
    /// A tail read lands in the middle of the payload, so the walk has no
    /// footing: the index is found by looking for its name and then checking
    /// that the length in front of it is one this buffer could hold. A wrong
    /// guess fails the same bounds checks as everything else.
    QList<FileFact> isoFactsInWindow(QByteArrayView bytes)
    {
        QList<FileFact> facts = isoFactsFrom(bytes, nullptr);
        if (!facts.isEmpty())
            return facts;

        static const QByteArray name = QByteArrayLiteral("moov");
        for (qint64 at = 4; at + 4 <= bytes.size(); ++at) {
            if (bytes.at(at) != 'm' || bytes.sliced(at, 4) != QByteArrayView(name))
                continue;

            const std::optional<quint32> declared = beU32(bytes, at - 4);
            if (!declared)
                continue;

            IsoBox box;
            box.type = name;
            box.offset = at - 4;
            box.headerBytes = 8;
            box.size = qint64(*declared);
            if (box.size < box.headerBytes || box.offset + box.size > bytes.size())
                continue;

            facts = isoFactsFrom(bytes, &box);
            if (!facts.isEmpty())
                return facts;
        }
        return facts;
    }

    bool looksLikeIso(QByteArrayView bytes)
    {
        const QList<IsoBox> boxes = isoBoxesIn(bytes, 0, bytes.size());
        for (const IsoBox& box : boxes) {
            if (box.type == QByteArrayLiteral("ftyp") || box.type == QByteArrayLiteral("moov")
                || box.type == QByteArrayLiteral("mdat"))
                return true;
        }
        return false;
    }

    bool looksLikeMatroska(QByteArrayView bytes)
    {
        return bytes.size() >= 4 && bytes.startsWith(QByteArrayLiteral("\x1a\x45\xdf\xa3"));
    }

    bool looksLikeAvi(QByteArrayView bytes)
    {
        return bytes.size() >= 12 && bytes.startsWith(QByteArrayLiteral("RIFF"))
            && bytes.sliced(8, 4) == QByteArrayLiteral("AVI ");
    }

} // namespace

// ---- the shared pieces ----------------------------------------------------

QList<IsoBox> isoBoxesIn(QByteArrayView bytes, qint64 offset, qint64 length)
{
    QList<IsoBox> boxes;
    if (offset < 0 || offset > bytes.size())
        return boxes;
    // The length is clamped to what is there *before* it is added to the offset,
    // rather than added and then clamped. A caller passes payloadBytes() of a box
    // that has already been read out of the file, so it is a claim like any other
    // and can be near INT64_MAX -- and `offset + length` on that is signed
    // overflow, which is undefined and in practice negative. See MOLE-357.
    const qint64 end = offset + std::clamp<qint64>(length, 0, bytes.size() - offset);

    qint64 at = offset;
    while (at + 8 <= end) {
        const std::optional<quint32> declared = beU32(bytes, at);
        const QByteArray type = fourCharacters(bytes, at + 4);
        if (!declared || type.isEmpty())
            return boxes;

        IsoBox box;
        box.type = type;
        box.offset = at;
        box.headerBytes = 8;
        box.size = qint64(*declared);

        if (box.size == 1) {
            // The 64-bit form: the real size follows the header.
            const std::optional<quint64> large = beU64(bytes, at + 8);
            if (!large)
                return boxes;
            box.size = qint64(*large);
            box.headerBytes = 16;
        } else if (box.size == 0) {
            // "To the end of the file", which here means to the end of what was
            // read -- a legitimate form, and the only one that is not a claim.
            box.size = end - at;
        }

        // A box shorter than its own header, or one running past the buffer, is
        // where the walk stops. What was found before it stands.
        //
        // Written as a subtraction rather than `at + box.size > end`: the 64-bit
        // form takes its size straight from the file, so a box claiming
        // 0x7FFFFFFFFFFFFFF0 made that addition overflow, the comparison passed,
        // and a box larger than the universe was appended for a caller to slice
        // with. `end - at` cannot overflow -- the loop condition has just
        // established that `at + 8 <= end`. A size that came back negative from
        // the conversion is caught by the first half. See MOLE-357.
        if (box.size < box.headerBytes || box.size > end - at)
            return boxes;

        boxes.append(box);
        at += box.size;
    }
    return boxes;
}

std::optional<IsoBox> isoBoxNamed(QByteArrayView bytes, qint64 offset, qint64 length, const char* type)
{
    const QList<IsoBox> boxes = isoBoxesIn(bytes, offset, length);
    for (const IsoBox& box : boxes) {
        if (box.type == QByteArray(type))
            return box;
    }
    return std::nullopt;
}

std::optional<IsoBox> isoBoxAt(QByteArrayView bytes, const QList<QByteArray>& path)
{
    qint64 offset = 0;
    qint64 length = bytes.size();
    std::optional<IsoBox> found;

    for (const QByteArray& step : path) {
        found = isoBoxNamed(bytes, offset, length, step.constData());
        if (!found)
            return std::nullopt;
        offset = found->payloadOffset();
        length = found->payloadBytes();
    }
    return found;
}

QString codecName(const QString& identifier)
{
    static const QHash<QString, QString> names {
        // ISO base media, four-character codes.
        { QStringLiteral("avc1"), QStringLiteral("H.264") },
        { QStringLiteral("avc3"), QStringLiteral("H.264") },
        { QStringLiteral("hev1"), QStringLiteral("H.265") },
        { QStringLiteral("hvc1"), QStringLiteral("H.265") },
        { QStringLiteral("av01"), QStringLiteral("AV1") },
        { QStringLiteral("mp4v"), QStringLiteral("MPEG-4") },
        { QStringLiteral("vp08"), QStringLiteral("VP8") },
        { QStringLiteral("vp09"), QStringLiteral("VP9") },
        { QStringLiteral("mp4a"), QStringLiteral("AAC") },
        { QStringLiteral("alac"), QStringLiteral("ALAC") },
        { QStringLiteral("Opus"), QStringLiteral("Opus") },
        // Matroska and WebM.
        { QStringLiteral("V_MPEG4/ISO/AVC"), QStringLiteral("H.264") },
        { QStringLiteral("V_MPEGH/ISO/HEVC"), QStringLiteral("H.265") },
        { QStringLiteral("V_VP8"), QStringLiteral("VP8") },
        { QStringLiteral("V_VP9"), QStringLiteral("VP9") },
        { QStringLiteral("V_AV1"), QStringLiteral("AV1") },
        { QStringLiteral("A_OPUS"), QStringLiteral("Opus") },
        { QStringLiteral("A_VORBIS"), QStringLiteral("Vorbis") },
        { QStringLiteral("A_AAC"), QStringLiteral("AAC") },
        { QStringLiteral("A_FLAC"), QStringLiteral("FLAC") },
        // AVI handlers.
        { QStringLiteral("XVID"), QStringLiteral("MPEG-4") },
        { QStringLiteral("DIVX"), QStringLiteral("MPEG-4") },
        { QStringLiteral("DX50"), QStringLiteral("MPEG-4") },
        { QStringLiteral("MJPG"), QStringLiteral("Motion JPEG") },
        { QStringLiteral("H264"), QStringLiteral("H.264") },
    };

    // Kept as itself when it is not in the table: a codec nobody here has heard
    // of is still an answer, and dropping it would be pretending the file did
    // not say.
    return names.value(identifier, identifier);
}

QString durationText(double seconds)
{
    if (seconds <= 0 || !std::isfinite(seconds))
        return {};
    const auto whole = qint64(std::llround(seconds));
    const qint64 hours = whole / 3600;
    const qint64 minutes = (whole % 3600) / 60;
    const qint64 rest = whole % 60;

    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(rest, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(rest, 2, 10, QLatin1Char('0'));
}

// ---- the reader -----------------------------------------------------------

bool VideoMetadataReader::canRead(const FileEntry& entry) const
{
    return !entry.isDir && entry.mimeType.startsWith(QLatin1String("video/"));
}

bool VideoMetadataReader::wantsTail(QByteArrayView head)
{
    // Only the ISO family writes its index anywhere but the front, and only when
    // the file was not written for streaming.
    return looksLikeIso(head) && !isoBoxAt(head, { QByteArrayLiteral("moov") }).has_value();
}

QList<FileFact> VideoMetadataReader::factsFor(QByteArrayView head, QByteArrayView tail)
{
    QList<FileFact> facts;
    if (head.isEmpty())
        return facts;

    if (looksLikeMatroska(head)) {
        facts = matroskaFacts(head);
        appendIf(facts, QStringLiteral("Container"), QStringLiteral("Matroska"));
        return facts;
    }
    if (looksLikeAvi(head)) {
        facts = aviFacts(head);
        appendIf(facts, QStringLiteral("Container"), QStringLiteral("AVI"));
        return facts;
    }
    if (!looksLikeIso(head))
        return facts;

    facts = isoFactsFrom(head, nullptr);
    // The index may have been written last, which is the one case that costs a
    // second read. The tail is a window into the middle of the file rather than
    // a file of its own, so the index is found in it rather than walked to.
    if (facts.isEmpty() && !tail.isEmpty())
        facts = isoFactsInWindow(tail);
    if (!facts.isEmpty())
        appendIf(facts, QStringLiteral("Container"), QStringLiteral("MP4 / QuickTime"));
    return facts;
}

QList<FileFact> VideoMetadataReader::read(
    const FileEntry& entry, QByteArrayView head, PluginServices services, const CancelToken& cancel) const
{
    QByteArray front = head.toByteArray();
    FileSystemPtr fs = services.vfs ? services.vfs->resolve(entry.uri) : nullptr;

    if (front.size() < kHeadBytes && fs) {
        Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
        if (opened.ok() && opened.value())
            front = opened.value()->read(kHeadBytes);
    }
    if (cancel.isCancelled())
        return {};

    QByteArray back;
    // A drive that cannot seek is not read through to reach the tail: that would
    // be the whole file, which is the one thing this must never do.
    const bool canSeek = fs && fs->capabilities().testFlag(VfsCapability::RandomAccessRead);
    if (wantsTail(front) && canSeek && entry.size > front.size()) {
        Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
        if (opened.ok() && opened.value()) {
            const qint64 from = std::max<qint64>(0, entry.size - kTailBytes);
            if (opened.value()->seek(from))
                back = opened.value()->read(kTailBytes);
        }
    }
    if (cancel.isCancelled())
        return {};

    return factsFor(front, back);
}

} // namespace mole
