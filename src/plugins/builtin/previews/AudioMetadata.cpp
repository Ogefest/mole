#include "plugins/builtin/previews/AudioMetadata.h"

#include "plugins/builtin/previews/MediaMetadata.h"

#include "core/vfs/VfsManager.h"

#include <QMimeDatabase>
#include <QStringDecoder>

#include <optional>

namespace mole {
namespace {

    bool fits(QByteArrayView bytes, qint64 offset, qint64 length)
    {
        return offset >= 0 && length >= 0 && offset <= bytes.size() - length;
    }

    quint32 beU32(QByteArrayView bytes, qint64 offset)
    {
        if (!fits(bytes, offset, 4))
            return 0;
        quint32 value = 0;
        for (int i = 0; i < 4; ++i)
            value = value << 8 | static_cast<unsigned char>(bytes.at(offset + i));
        return value;
    }

    quint32 leU32(QByteArrayView bytes, qint64 offset)
    {
        if (!fits(bytes, offset, 4))
            return 0;
        quint32 value = 0;
        for (int i = 0; i < 4; ++i)
            value |= quint32(static_cast<unsigned char>(bytes.at(offset + i))) << (8 * i);
        return value;
    }

    /// One tag, kept in the order the panel shows them rather than in the order
    /// the file happened to write them.
    struct Tags
    {
        QString title;
        QString artist;
        QString album;
        QString albumArtist;
        QString year;
        QString track;
        QString genre;
        QString comment;

        QString* field(const QString& name)
        {
            if (name == QLatin1String("title"))
                return &title;
            if (name == QLatin1String("artist"))
                return &artist;
            if (name == QLatin1String("album"))
                return &album;
            if (name == QLatin1String("albumartist"))
                return &albumArtist;
            if (name == QLatin1String("date") || name == QLatin1String("year"))
                return &year;
            if (name == QLatin1String("tracknumber") || name == QLatin1String("track"))
                return &track;
            if (name == QLatin1String("genre"))
                return &genre;
            if (name == QLatin1String("comment"))
                return &comment;
            return nullptr;
        }

        bool isEmpty() const
        {
            return title.isEmpty() && artist.isEmpty() && album.isEmpty() && albumArtist.isEmpty()
                && year.isEmpty() && track.isEmpty() && genre.isEmpty() && comment.isEmpty();
        }
    };

    /// What the stream itself is, as opposed to what somebody wrote about it.
    struct Stream
    {
        double seconds = 0;
        bool durationIsEstimated = false;
        int sampleRate = 0;
        int channels = 0;
        int bitrate = 0; ///< kbit/s
        QString codec;
    };

    void setOnce(QString& field, const QString& value)
    {
        if (field.isEmpty() && !value.isEmpty())
            field = value.trimmed();
    }

    // ---- ID3v2 -------------------------------------------------------------

    /// A synchsafe integer: seven bits per byte, so the value can never look like
    /// a frame sync.
    quint32 synchsafe(QByteArrayView bytes, qint64 offset)
    {
        if (!fits(bytes, offset, 4))
            return 0;
        quint32 value = 0;
        for (int i = 0; i < 4; ++i)
            value = value << 7 | (static_cast<unsigned char>(bytes.at(offset + i)) & 0x7f);
        return value;
    }

    /// Undoes the unsynchronisation scheme: every `FF 00` in the block stands for
    /// a single `FF`.
    QByteArray resynchronised(QByteArrayView bytes)
    {
        QByteArray out;
        out.reserve(bytes.size());
        for (qint64 i = 0; i < bytes.size(); ++i) {
            const char byte = bytes.at(i);
            out += byte;
            if (static_cast<unsigned char>(byte) == 0xff && i + 1 < bytes.size() && bytes.at(i + 1) == '\0')
                ++i;
        }
        return out;
    }

    /// An ID3v2 text frame: one byte of encoding, then the text in it.
    QString id3Text(QByteArrayView payload)
    {
        if (payload.isEmpty())
            return {};
        const auto encoding = static_cast<unsigned char>(payload.at(0));
        const QByteArray text = payload.sliced(1).toByteArray();

        QString decoded;
        switch (encoding) {
        case 0: // Latin-1
            decoded = QString::fromLatin1(text);
            break;
        case 1: { // UTF-16 with a byte order mark
            QStringDecoder utf16(QStringConverter::Utf16);
            decoded = utf16.decode(text);
            break;
        }
        case 2: { // UTF-16BE without one
            QStringDecoder utf16(QStringConverter::Utf16BE);
            decoded = utf16.decode(text);
            break;
        }
        default:
            decoded = QString::fromUtf8(text);
            break;
        }

        // A frame may carry several values separated by NULs, and every frame
        // may be padded with them.
        const qsizetype nul = decoded.indexOf(QChar(u'\0'));
        if (nul >= 0)
            decoded.truncate(nul);
        return decoded.trimmed();
    }

    /// A COMM or USLT frame: encoding, a three-letter language, a description,
    /// a NUL, and then the text.
    ///
    /// **Its own decode, because id3Text() cuts at the first NUL and that NUL is
    /// the one separating the description from the comment.** So the Comment row
    /// read "eng" -- or "engiTunNORM", the description run together with the
    /// language -- on essentially every MP3 that has one, and setOnce() then
    /// kept the ID3v1 comment from replacing it. See MOLE-383.
    QString id3Comment(QByteArrayView payload)
    {
        // Encoding, then three bytes of language. A frame with neither is not
        // one of these.
        if (payload.size() < 5)
            return {};
        const auto encoding = static_cast<unsigned char>(payload.at(0));
        const QByteArray rest = payload.sliced(4).toByteArray();

        // The description ends at a terminator, which is one NUL for the
        // single-byte encodings and two for the UTF-16 ones -- and the
        // terminator is what has to be found *before* decoding, because after
        // decoding the two halves are one string with nothing between them.
        QByteArray text;
        if (encoding == 1 || encoding == 2) {
            for (qsizetype i = 0; i + 1 < rest.size(); i += 2) {
                if (rest.at(i) == '\0' && rest.at(i + 1) == '\0') {
                    text = rest.mid(i + 2);
                    break;
                }
            }
        } else if (const qsizetype nul = rest.indexOf('\0'); nul >= 0) {
            text = rest.mid(nul + 1);
        }
        if (text.isEmpty())
            return {};

        // The encoding byte back in front, so the one decoder handles all four.
        QByteArray asAFrame(1, char(encoding));
        asAFrame += text;
        return id3Text(asAFrame);
    }

    /// Reads an ID3v2 block and returns how many bytes of the file it occupied,
    /// so the caller knows where the audio starts.
    qint64 readId3v2(QByteArrayView head, Tags& tags)
    {
        if (!fits(head, 0, 10) || head.sliced(0, 3) != QByteArrayView(QByteArrayLiteral("ID3")))
            return 0;

        const auto major = static_cast<unsigned char>(head.at(3));
        const auto flags = static_cast<unsigned char>(head.at(5));
        const qint64 declared = qint64(synchsafe(head, 6));
        if (declared <= 0)
            return 0;

        // What is actually here, which is the shorter of what the header claims
        // and what was read: a truncated download declares the whole thing.
        const qint64 available = std::min<qint64>(declared, head.size() - 10);
        if (available <= 0)
            return declared + 10;

        const QByteArray block = (flags & 0x80) ? resynchronised(head.sliced(10, available))
                                                : head.sliced(10, available).toByteArray();

        const int headerBytes = major >= 3 ? 10 : 6;
        qint64 at = 0;

        // The extended header, when the flag says there is one. It sits between
        // the header and the first frame, and skipping it was not done at all --
        // so the first "frame id" was the extended header's own size, whose
        // leading byte is a NUL, and the loop broke immediately: **every tag in
        // the file invisible**, from one bit. Its length field is synchsafe in
        // v2.4 and a plain 32-bit count of the bytes *after* itself in v2.3.
        // See MOLE-383.
        if ((flags & 0x40) && major >= 3) {
            const qint64 extended = major >= 4 ? qint64(synchsafe(block, 0)) : qint64(beU32(block, 0)) + 4;
            if (extended <= 0 || extended >= block.size())
                return declared + 10;
            at = extended;
        }
        while (at + headerBytes <= block.size()) {
            const QByteArray id = block.mid(at, major >= 3 ? 4 : 3);
            if (id.isEmpty() || id.at(0) == '\0')
                break; // padding, which is where the frames end

            qint64 size = 0;
            if (major >= 4)
                size = qint64(synchsafe(block, at + 4));
            else if (major == 3)
                size = qint64(beU32(block, at + 4));
            else
                size = (static_cast<unsigned char>(block.at(at + 3)) << 16)
                    | (static_cast<unsigned char>(block.at(at + 4)) << 8)
                    | static_cast<unsigned char>(block.at(at + 5));

            // A frame claiming more than the block holds is skipped rather than
            // trusted -- the declared size is the file's word, not a fact.
            if (size <= 0 || at + headerBytes + size > block.size())
                break;

            QByteArrayView payload = QByteArrayView(block).sliced(at + headerBytes, size);

            // A v2.4 frame may carry a data-length indicator: four extra bytes
            // in front of the frame's own content, announced by bit 0 of the
            // second flag byte. Not skipping them put four bytes of length where
            // the encoding byte should be, so the text came out garbled -- and it
            // is the flag a compressed or encrypted frame sets, which is
            // ordinary in files written by anything that pads.
            // See MOLE-383.
            if (major >= 4 && size > 4) {
                const auto frameFlags = static_cast<unsigned char>(block.at(at + 9));
                if (frameFlags & 0x01)
                    payload = payload.sliced(4);
            }

            const QString text = id3Text(payload);
            if (id == QByteArrayLiteral("TIT2") || id == QByteArrayLiteral("TT2"))
                setOnce(tags.title, text);
            else if (id == QByteArrayLiteral("TPE1") || id == QByteArrayLiteral("TP1"))
                setOnce(tags.artist, text);
            else if (id == QByteArrayLiteral("TALB") || id == QByteArrayLiteral("TAL"))
                setOnce(tags.album, text);
            else if (id == QByteArrayLiteral("TPE2"))
                setOnce(tags.albumArtist, text);
            else if (id == QByteArrayLiteral("TDRC") || id == QByteArrayLiteral("TYER"))
                setOnce(tags.year, text.left(4));
            else if (id == QByteArrayLiteral("TRCK") || id == QByteArrayLiteral("TRK"))
                setOnce(tags.track, text);
            else if (id == QByteArrayLiteral("TCON"))
                setOnce(tags.genre, text);
            else if (id == QByteArrayLiteral("COMM") || id == QByteArrayLiteral("COM"))
                setOnce(tags.comment, id3Comment(payload));

            at += headerBytes + size;
        }
        return declared + 10;
    }

    // ---- ID3v1, in the last 128 bytes --------------------------------------

    void readId3v1(QByteArrayView tail, Tags& tags)
    {
        if (tail.size() < 128)
            return;
        const qint64 at = tail.size() - 128;
        if (tail.sliced(at, 3) != QByteArrayView(QByteArrayLiteral("TAG")))
            return;

        const auto field = [&tail, at](qint64 offset, qint64 length) {
            QByteArray value = tail.sliced(at + offset, length).toByteArray();
            const qsizetype nul = value.indexOf('\0');
            if (nul >= 0)
                value.truncate(nul);
            return QString::fromLatin1(value).trimmed();
        };

        setOnce(tags.title, field(3, 30));
        setOnce(tags.artist, field(33, 30));
        setOnce(tags.album, field(63, 30));
        setOnce(tags.year, field(93, 4));
        setOnce(tags.comment, field(97, 30));

        // A track number lives in the last two bytes of the comment when the
        // one before them is a NUL: ID3v1.1's way of finding room for it.
        const auto marker = static_cast<unsigned char>(tail.at(at + 125));
        const auto number = static_cast<unsigned char>(tail.at(at + 126));
        if (marker == 0 && number > 0)
            setOnce(tags.track, QString::number(number));
    }

    // ---- MPEG audio frames -------------------------------------------------

    /// The first frame header after the tags, which is where an MP3 says what it
    /// is. Returns false when there is none in the buffer.
    bool readMpegFrame(QByteArrayView head, qint64 from, Stream& stream)
    {
        static const int bitrates[16]
            = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
        static const int rates[4] = { 44100, 48000, 32000, 0 };

        for (qint64 at = std::max<qint64>(0, from); at + 4 <= head.size(); ++at) {
            const auto first = static_cast<unsigned char>(head.at(at));
            const auto second = static_cast<unsigned char>(head.at(at + 1));
            if (first != 0xff || (second & 0xe0) != 0xe0)
                continue;

            const int versionBits = (second >> 3) & 0x03;
            const int layerBits = (second >> 1) & 0x03;
            if (versionBits == 1 || layerBits == 0)
                continue; // reserved, so this is not a frame header

            const auto third = static_cast<unsigned char>(head.at(at + 2));
            const int bitrateIndex = (third >> 4) & 0x0f;
            const int rateIndex = (third >> 2) & 0x03;
            if (bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3)
                continue;

            const auto fourth = static_cast<unsigned char>(head.at(at + 3));
            stream.bitrate = bitrates[bitrateIndex];
            stream.sampleRate = rates[rateIndex];
            if (versionBits == 2) // MPEG 2
                stream.sampleRate /= 2;
            else if (versionBits == 0) // MPEG 2.5
                stream.sampleRate /= 4;
            stream.channels = ((fourth >> 6) & 0x03) == 3 ? 1 : 2;
            stream.codec = QStringLiteral("MP3");

            // Xing or VBRI, when the encoder wrote one: the frame count is what
            // makes a variable bitrate file's duration a fact rather than a guess.
            const qint64 window = std::min<qint64>(head.size() - at, 1024);
            const QByteArrayView frame = head.sliced(at, window);
            const qsizetype xing = frame.indexOf(QByteArrayLiteral("Xing")) >= 0
                ? frame.indexOf(QByteArrayLiteral("Xing"))
                : frame.indexOf(QByteArrayLiteral("Info"));
            if (xing >= 0 && stream.sampleRate > 0) {
                const quint32 flags = beU32(frame, xing + 4);
                if (flags & 0x1) {
                    const quint32 frames = beU32(frame, xing + 8);
                    const int samplesPerFrame = versionBits == 3 ? 1152 : 576;
                    if (frames > 0)
                        stream.seconds = double(frames) * samplesPerFrame / stream.sampleRate;
                }
            }
            return true;
        }
        return false;
    }

    // ---- FLAC and Ogg Vorbis comments --------------------------------------

    void readVorbisComments(QByteArrayView block, Tags& tags)
    {
        // A vendor string, then a count, then `NAME=value` each with its own
        // length. Every one of those lengths is checked before it is followed.
        const quint32 vendor = leU32(block, 0);
        qint64 at = 4 + qint64(vendor);
        if (!fits(block, at, 4))
            return;
        const quint32 count = leU32(block, at);
        at += 4;

        for (quint32 i = 0; i < count && i < 1024; ++i) {
            if (!fits(block, at, 4))
                return;
            const qint64 length = qint64(leU32(block, at));
            at += 4;
            if (!fits(block, at, length))
                return;

            const QString entry = QString::fromUtf8(block.sliced(at, length).toByteArray());
            at += length;

            const qsizetype equals = entry.indexOf(QLatin1Char('='));
            if (equals <= 0)
                continue;
            if (QString* field = tags.field(entry.left(equals).toLower()))
                setOnce(*field, entry.mid(equals + 1));
        }
    }

    bool readFlac(QByteArrayView head, Tags& tags, Stream& stream)
    {
        if (!head.startsWith(QByteArrayLiteral("fLaC")))
            return false;

        qint64 at = 4;
        while (at + 4 <= head.size()) {
            const auto header = static_cast<unsigned char>(head.at(at));
            const bool last = (header & 0x80) != 0;
            const int type = header & 0x7f;
            const qint64 length = (static_cast<unsigned char>(head.at(at + 1)) << 16)
                | (static_cast<unsigned char>(head.at(at + 2)) << 8)
                | static_cast<unsigned char>(head.at(at + 3));
            const qint64 payload = at + 4;
            if (length < 0 || !fits(head, payload, length))
                return true; // what was read stands; the rest is not here

            if (type == 0 && length >= 18) {
                // STREAMINFO: the sample rate, the channel count and the total
                // number of samples, which makes the duration exact.
                const quint32 rateAndRest = beU32(head, payload + 10);
                stream.sampleRate = int(rateAndRest >> 12);
                stream.channels = int(((rateAndRest >> 9) & 0x07) + 1);
                const quint64 samples = (quint64(rateAndRest & 0x0f) << 32) | beU32(head, payload + 14);
                if (stream.sampleRate > 0 && samples > 0)
                    stream.seconds = double(samples) / stream.sampleRate;
                stream.codec = QStringLiteral("FLAC");
            } else if (type == 4) {
                readVorbisComments(head.sliced(payload, length), tags);
            }

            at = payload + length;
            if (last)
                break;
        }
        return true;
    }

    bool readOgg(QByteArrayView head, Tags& tags, Stream& stream)
    {
        if (!head.startsWith(QByteArrayLiteral("OggS")))
            return false;
        stream.codec = QStringLiteral("Ogg");

        // The comment header lies in one of the first pages, and finding it by
        // its own signature is enough here: the pages before it are a fixed
        // shape but the segment table makes walking them fiddly for no gain.
        const qsizetype vorbis = head.indexOf(QByteArrayLiteral("\x03vorbis"));
        if (vorbis >= 0) {
            stream.codec = QStringLiteral("Vorbis");
            readVorbisComments(head.sliced(vorbis + 7), tags);
            return true;
        }
        const qsizetype opus = head.indexOf(QByteArrayLiteral("OpusTags"));
        if (opus >= 0) {
            stream.codec = QStringLiteral("Opus");
            readVorbisComments(head.sliced(opus + 8), tags);
        }
        return true;
    }

    // ---- MP4 ilst, through MOLE-135's box walk -----------------------------

    bool readMp4Tags(QByteArrayView head, Tags& tags, Stream& stream)
    {
        const std::optional<IsoBox> ilst = isoBoxAt(head,
            { QByteArrayLiteral("moov"), QByteArrayLiteral("udta"), QByteArrayLiteral("meta"),
                QByteArrayLiteral("ilst") });
        const std::optional<IsoBox> moov = isoBoxAt(head, { QByteArrayLiteral("moov") });
        if (!moov)
            return false;

        if (ilst) {
            const QList<IsoBox> items = isoBoxesIn(head, ilst->payloadOffset(), ilst->payloadBytes());
            for (const IsoBox& item : items) {
                const std::optional<IsoBox> data
                    = isoBoxNamed(head, item.payloadOffset(), item.payloadBytes(), "data");
                if (!data || data->payloadBytes() <= 8)
                    continue;
                // Asked of the buffer, not of the box. The box walk refuses one
                // that runs past the buffer, so this is belt as well as braces --
                // and it is the braces that broke: a 64-bit size near INT64_MAX
                // overflowed the walk's own check, and a view claiming 2^63 bytes
                // read off the end until it faulted. A reader that slices on a
                // number out of a file checks the number. See MOLE-357.
                if (!fits(head, data->payloadOffset() + 8, data->payloadBytes() - 8))
                    continue;
                const QByteArrayView value = head.sliced(data->payloadOffset() + 8, data->payloadBytes() - 8);
                const QString text = QString::fromUtf8(value.toByteArray()).trimmed();

                if (item.type
                    == QByteArrayLiteral("\xa9"
                                         "nam"))
                    setOnce(tags.title, text);
                else if (item.type
                    == QByteArrayLiteral("\xa9"
                                         "ART"))
                    setOnce(tags.artist, text);
                else if (item.type
                    == QByteArrayLiteral("\xa9"
                                         "alb"))
                    setOnce(tags.album, text);
                else if (item.type
                    == QByteArrayLiteral("\xa9"
                                         "day"))
                    setOnce(tags.year, text.left(4));
                else if (item.type
                    == QByteArrayLiteral("\xa9"
                                         "gen"))
                    setOnce(tags.genre, text);
                else if (item.type == QByteArrayLiteral("trkn") && value.size() >= 4) {
                    const auto number = static_cast<unsigned char>(value.at(3));
                    if (number > 0)
                        setOnce(tags.track, QString::number(number));
                }
            }
        }

        // The duration and the codec both come from the video reader's own walk
        // now, rather than from a second reading of the same boxes.
        //
        // The second reading had two faults. It read a version-1 duration as 32
        // bits -- the *high* half of a 64-bit field -- so a file with the wide
        // header reported millions of hours or nought. And the codec was "AAC"
        // unconditionally, so every ALAC .m4a, a lossless file somebody chose on
        // purpose, was described as AAC. See MOLE-383.
        if (const std::optional<double> seconds = isoMovieSeconds(head, *moov))
            stream.seconds = *seconds;
        const QString codec = isoFirstCodec(head, *moov);
        stream.codec = codec.isEmpty() ? QStringLiteral("AAC") : codec;
        return true;
    }

    void appendIf(QList<FileFact>& facts, const QString& label, const QString& value)
    {
        if (!value.isEmpty())
            facts.append({ label, value });
    }

} // namespace

bool AudioMetadataReader::canRead(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;
    if (entry.mimeType.startsWith(QLatin1String("audio/")))
        return true;
    // The name as well. This one worked by luck: no viewer claims .mp3 by name,
    // so the sniff always ran and always filled the type in. The moment one does
    // -- an audio player, a waveform -- it would have gone the way the video
    // reader had already gone. Asked of the MIME database once, the way
    // VideoPreviewProvider::videoSuffixes() does. See MOLE-381.
    static const QStringList suffixes = audioSuffixes();
    return suffixes.contains(entry.uri.suffix());
}

QStringList AudioMetadataReader::audioSuffixes()
{
    QStringList suffixes;
    const QList<QMimeType> types = QMimeDatabase().allMimeTypes();
    for (const QMimeType& type : types) {
        if (!type.name().startsWith(QLatin1String("audio/")))
            continue;
        for (const QString& suffix : type.suffixes()) {
            const QString lower = suffix.toLower();
            if (!suffixes.contains(lower))
                suffixes.append(lower);
        }
    }
    return suffixes;
}

QList<FileFact> AudioMetadataReader::factsFor(QByteArrayView head, QByteArrayView tail, qint64 fileSize)
{
    QList<FileFact> facts;
    if (head.isEmpty())
        return facts;

    Tags tags;
    Stream stream;

    if (!readFlac(head, tags, stream) && !readOgg(head, tags, stream) && !readMp4Tags(head, tags, stream)) {
        // An MP3, or something that will look like one: the ID3v2 block first,
        // then the first frame after it, then the tag at the end of the file.
        const qint64 audioAt = readId3v2(head, tags);
        readId3v1(tail, tags);

        if (readMpegFrame(head, audioAt, stream) && stream.seconds <= 0 && stream.bitrate > 0
            && fileSize > 0) {
            // No Xing header, so the length is arithmetic on the size -- right
            // for a constant bitrate and wrong for a variable one.
            stream.seconds = double(fileSize - audioAt) * 8.0 / (stream.bitrate * 1000.0);
            stream.durationIsEstimated = true;
        }
    }

    // The four somebody searches by. The rest are shown and not asked about,
    // which is what an empty key means.
    if (!tags.title.isEmpty())
        facts.append({ QStringLiteral("Title"), tags.title, QStringLiteral("audio.title") });
    if (!tags.artist.isEmpty())
        facts.append({ QStringLiteral("Artist"), tags.artist, QStringLiteral("audio.artist") });
    if (!tags.album.isEmpty())
        facts.append({ QStringLiteral("Album"), tags.album, QStringLiteral("audio.album") });
    appendIf(facts, QStringLiteral("Album artist"), tags.albumArtist);
    appendIf(facts, QStringLiteral("Year"), tags.year);
    appendIf(facts, QStringLiteral("Track"), tags.track);
    appendIf(facts, QStringLiteral("Genre"), tags.genre);
    appendIf(facts, QStringLiteral("Comment"), tags.comment);

    if (stream.seconds > 0) {
        const QString text = durationText(stream.seconds);
        appendIf(facts, QStringLiteral("Duration"),
            stream.durationIsEstimated ? QStringLiteral("%1 (estimated)").arg(text) : text);
    }
    appendIf(facts, QStringLiteral("Codec"), codecName(stream.codec));
    if (stream.bitrate > 0)
        facts.append({ QStringLiteral("Bitrate"), QStringLiteral("%1 kbit/s").arg(stream.bitrate) });
    if (stream.sampleRate > 0) {
        facts.append({ QStringLiteral("Sample rate"),
            QStringLiteral("%1 kHz").arg(stream.sampleRate / 1000.0, 0, 'g', 4) });
    }
    if (stream.channels > 0) {
        facts.append({ QStringLiteral("Channels"),
            stream.channels == 1       ? QStringLiteral("mono")
                : stream.channels == 2 ? QStringLiteral("stereo")
                                       : QStringLiteral("%1 channels").arg(stream.channels) });
    }
    return facts;
}

QList<FileFact> AudioMetadataReader::read(const FileEntry& entry, QByteArrayView head,
    const PluginServices& services, const CancelToken& cancel) const
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

    // The tail, only for the format that keeps a tag there, and only when the
    // drive can reach it without being read through.
    QByteArray back;
    const bool wantsTail = front.startsWith(QByteArrayLiteral("ID3"))
        || (!front.startsWith(QByteArrayLiteral("fLaC")) && !front.startsWith(QByteArrayLiteral("OggS")));
    if (wantsTail && fs && entry.size > front.size()
        && fs->capabilities().testFlag(VfsCapability::RandomAccessRead)) {
        Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
        if (opened.ok() && opened.value()) {
            const qint64 from = std::max<qint64>(0, entry.size - kTailBytes);
            if (opened.value()->seek(from))
                back = opened.value()->read(kTailBytes);
        }
    } else if (entry.size <= front.size()) {
        back = front; // the whole file is in hand, so the tag at its end is too
    }
    if (cancel.isCancelled())
        return {};

    return factsFor(front, back, entry.size);
}

} // namespace mole
