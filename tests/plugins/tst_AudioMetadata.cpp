#include "plugins/builtin/previews/AudioMetadata.h"
#include "support/MoleTestMain.h"

using namespace mole;

namespace {

QByteArray be32(quint32 value)
{
    QByteArray out(4, '\0');
    for (int i = 0; i < 4; ++i)
        out[i] = char((value >> (8 * (3 - i))) & 0xff);
    return out;
}

QByteArray le32(quint32 value)
{
    QByteArray out(4, '\0');
    for (int i = 0; i < 4; ++i)
        out[i] = char((value >> (8 * i)) & 0xff);
    return out;
}

/// A synchsafe length: seven bits a byte, which is how ID3v2 writes a size that
/// could otherwise be mistaken for a frame sync.
QByteArray synchsafe(quint32 value)
{
    QByteArray out(4, '\0');
    for (int i = 0; i < 4; ++i)
        out[i] = char((value >> (7 * (3 - i))) & 0x7f);
    return out;
}

/// One ID3v2 text frame: an id, a size, two flag bytes and the encoded text.
QByteArray textFrame(const char* id, const QByteArray& text, int major, char encoding = 0)
{
    const QByteArray payload = QByteArray(1, encoding) + text;
    const QByteArray size = major >= 4 ? synchsafe(quint32(payload.size())) : be32(quint32(payload.size()));
    return QByteArray(id, 4) + size + QByteArray(2, '\0') + payload;
}

/// A COMM frame: encoding, a three-letter language, a description, a NUL, and
/// then the comment.
///
/// The shape that broke the reader -- see MOLE-383. The description is what
/// iTunes and every normaliser put there, and the NUL after it is the one
/// id3Text() was cutting the whole payload at.
QByteArray commentFrame(
    const QByteArray& description, const QByteArray& comment, int major, char encoding = 0)
{
    QByteArray payload(1, encoding);
    payload += QByteArrayLiteral("eng");
    payload += description;
    payload += '\0';
    payload += comment;
    const QByteArray size = major >= 4 ? synchsafe(quint32(payload.size())) : be32(quint32(payload.size()));
    return QByteArrayLiteral("COMM") + size + QByteArray(2, '\0') + payload;
}

/// A v2.4 frame carrying a data-length indicator: four bytes of length in front
/// of the frame's own content, announced by bit 0 of the second flag byte.
QByteArray frameWithADataLength(const char* id, const QByteArray& text)
{
    const QByteArray content = QByteArray(1, '\0') + text;
    const QByteArray payload = synchsafe(quint32(content.size())) + content;
    QByteArray flags(2, '\0');
    flags[1] = char(0x01);
    return QByteArray(id, 4) + synchsafe(quint32(payload.size())) + flags + payload;
}

/// A frame header a decoder would accept: MPEG 1 layer III, 128 kbit/s, 44.1 kHz.
QByteArray mpegFrame()
{
    QByteArray frame(4, '\0');
    frame[0] = char(0xff);
    frame[1] = char(0xfb); // MPEG 1, layer III, no CRC
    frame[2] = char(0x90); // 128 kbit/s, 44.1 kHz
    frame[3] = char(0x00); // stereo
    return frame + QByteArray(413, '\0');
}

QByteArray id3v2(int major, const QByteArray& frames, bool unsynchronised = false)
{
    QByteArray body = frames;
    if (unsynchronised) {
        // Every 0xFF in the block is followed by a 0x00 that is not part of it.
        QByteArray escaped;
        for (const char byte : body) {
            escaped += byte;
            if (static_cast<unsigned char>(byte) == 0xff)
                escaped += '\0';
        }
        body = escaped;
    }

    QByteArray header = QByteArrayLiteral("ID3");
    header += char(major);
    header += char(0);
    header += char(unsynchronised ? 0x80 : 0x00);
    header += synchsafe(quint32(body.size()));
    return header + body;
}

/// The same, with an extended header between the header and the first frame.
///
/// One bit in the flags, and the whole tag was invisible: the first "frame id"
/// read was the extended header's own size, whose leading byte is a NUL, and the
/// loop treated that as padding. See MOLE-383.
QByteArray id3v2WithAnExtendedHeader(int major, const QByteArray& frames)
{
    QByteArray extended;
    if (major >= 4) {
        // v2.4: a synchsafe size that counts itself, one byte of flag count,
        // one of flags.
        extended = synchsafe(6) + QByteArray(1, char(1)) + QByteArray(1, '\0');
    } else {
        // v2.3: a plain size of what follows the size field, then flags and a
        // padding count.
        extended = be32(6) + QByteArray(6, '\0');
    }

    const QByteArray body = extended + frames;
    QByteArray header = QByteArrayLiteral("ID3");
    header += char(major);
    header += char(0);
    header += char(0x40); // the extended-header flag
    header += synchsafe(quint32(body.size()));
    return header + body;
}

QByteArray mp3WithId3v2(int major, bool unsynchronised = false, qsizetype bodyFrames = 40)
{
    QByteArray frames;
    frames += textFrame("TIT2", QByteArrayLiteral("Blue Monday"), major);
    frames += textFrame("TPE1", QByteArrayLiteral("New Order"), major);
    frames += textFrame("TALB", QByteArrayLiteral("Power, Corruption & Lies"), major);
    frames += textFrame("TPE2", QByteArrayLiteral("New Order"), major);
    frames += textFrame(major >= 4 ? "TDRC" : "TYER", QByteArrayLiteral("1983"), major);
    frames += textFrame("TRCK", QByteArrayLiteral("3/8"), major);
    frames += textFrame("TCON", QByteArrayLiteral("Synth-pop"), major);

    QByteArray out = id3v2(major, frames, unsynchronised);
    for (qsizetype i = 0; i < bodyFrames; ++i)
        out += mpegFrame();
    return out;
}

/// The 128 bytes at the end of a file that carried tags before ID3v2 existed.
QByteArray id3v1()
{
    QByteArray tag = QByteArrayLiteral("TAG");
    const auto padded = [](const QByteArray& text, int length) {
        QByteArray out = text.left(length);
        out.resize(length, '\0');
        return out;
    };
    tag += padded(QByteArrayLiteral("Temptation"), 30);
    tag += padded(QByteArrayLiteral("New Order"), 30);
    tag += padded(QByteArrayLiteral("Substance"), 30);
    tag += padded(QByteArrayLiteral("1987"), 4);
    QByteArray comment(30, '\0');
    comment[29] = char(5); // ID3v1.1 keeps the track number in the last byte
    tag += comment;
    tag += char(52);
    return tag;
}

QByteArray flac()
{
    // STREAMINFO: everything up to the sample rate is fixed, and the rate, the
    // channel count and the sample total share three bytes at the end of it.
    QByteArray info(34, '\0');
    const quint32 sampleRate = 44100;
    const quint32 channelsMinusOne = 1;
    const quint32 bitsMinusOne = 15;
    const quint64 samples = 44100ULL * 217; // 3:37
    const quint32 packed
        = (sampleRate << 12) | (channelsMinusOne << 9) | (bitsMinusOne << 4) | quint32(samples >> 32);
    info.replace(10, 4, be32(packed));
    info.replace(14, 4, be32(quint32(samples & 0xffffffffu)));

    QByteArray comments = le32(6) + QByteArrayLiteral("Mole/1");
    const QList<QByteArray> entries { QByteArrayLiteral("TITLE=Ceremony"),
        QByteArrayLiteral("ARTIST=New Order"), QByteArrayLiteral("ALBUM=Substance"),
        QByteArrayLiteral("DATE=1987"), QByteArrayLiteral("TRACKNUMBER=1"),
        QByteArrayLiteral("GENRE=Post-punk") };
    comments += le32(quint32(entries.size()));
    for (const QByteArray& entry : entries)
        comments += le32(quint32(entry.size())) + entry;

    const auto block = [](int type, const QByteArray& payload, bool last) {
        QByteArray out;
        out += char((last ? 0x80 : 0x00) | type);
        out += char((payload.size() >> 16) & 0xff);
        out += char((payload.size() >> 8) & 0xff);
        out += char(payload.size() & 0xff);
        return out + payload;
    };

    return QByteArrayLiteral("fLaC") + block(0, info, false) + block(4, comments, true)
        + QByteArray(512, '\x01');
}

// ---- an .m4a, through the same box walk the video reader uses --------------

QByteArray box(const char* type, const QByteArray& payload)
{
    return be32(quint32(payload.size() + 8)) + QByteArray(type, 4) + payload;
}

QByteArray dataBox(const QByteArray& text)
{
    return box("data", be32(1) + be32(0) + text);
}

/// A sample-description chain deep enough for the codec to be read out of it:
/// trak -> mdia -> minf -> stbl -> stsd -> the entry's own four characters.
QByteArray audioTrack(const char* codec)
{
    const QByteArray stsd = box("stsd", be32(0) + be32(1) + box(codec, QByteArray(70, '\0')));
    const QByteArray minf = box("minf", box("stbl", stsd));
    const QByteArray mdia = box("mdia", box("hdlr", QByteArray(24, '\0')) + minf);
    return box("trak", mdia);
}

QByteArray m4a(const char* codec = "mp4a", bool wideHeader = false)
{
    // Version 0: a 32-bit creation time, modification time, timescale and
    // duration. Version 1 widens the two times and the duration to 64 bits,
    // which is what the audio reader's own copy of this read wrongly.
    QByteArray mvhd;
    if (wideHeader) {
        mvhd = be32(1u << 24); // version 1
        mvhd += be32(0) + be32(0) + be32(0) + be32(0); // created, modified
        mvhd += be32(1000);
        mvhd += be32(0) + be32(215000); // a 64-bit duration
        mvhd += QByteArray(80, '\0');
    } else {
        mvhd = be32(0) + be32(0) + be32(0) + be32(1000) + be32(215000) + QByteArray(80, '\0');
    }

    QByteArray ilst;
    ilst += box("\xa9"
                "nam",
        dataBox(QByteArrayLiteral("Bizarre Love Triangle")));
    ilst += box("\xa9"
                "ART",
        dataBox(QByteArrayLiteral("New Order")));
    ilst += box("\xa9"
                "alb",
        dataBox(QByteArrayLiteral("Brotherhood")));
    ilst += box("\xa9"
                "day",
        dataBox(QByteArrayLiteral("1986")));
    ilst += box("\xa9"
                "gen",
        dataBox(QByteArrayLiteral("Synth-pop")));
    // trkn: two reserved bytes, then the number and the total, each big-endian.
    ilst += box("trkn", dataBox(QByteArray(3, '\0') + QByteArray(1, char(4)) + QByteArray(4, '\0')));

    const QByteArray meta = box("meta", box("hdlr", QByteArray(24, '\0')) + box("ilst", ilst));
    const QByteArray moov = box("moov", box("mvhd", mvhd) + audioTrack(codec) + box("udta", meta));
    return box("ftyp", QByteArrayLiteral("M4A ") + be32(0) + QByteArrayLiteral("M4A mp42isom")) + moov
        + box("mdat", QByteArray(2048, 'a'));
}

QString factNamed(const QList<FileFact>& facts, const QString& label)
{
    for (const FileFact& fact : facts) {
        if (fact.label == label)
            return fact.value;
    }
    return {};
}

} // namespace

/// The title, artist and album an audio file carries inside it.
class TestAudioMetadata : public QObject
{
    Q_OBJECT

private slots:
    void anMp3WithId3v2_data();
    void anMp3WithId3v2();
    void aCommentFrameIsTheCommentAndNotTheLanguage_data();
    void aCommentFrameIsTheCommentAndNotTheLanguage();
    void anExtendedHeaderDoesNotHideEveryTag_data();
    void anExtendedHeaderDoesNotHideEveryTag();
    void aFrameWithADataLengthIndicatorIsStillReadable();
    void anUnsynchronisedBlockIsReadTheSameWay();
    void anMp3WithOnlyAnId3v1Tag();
    void aDurationWithNoHeaderToReadItFromIsMarkedAsAnEstimate();
    void aFlacReportsItsTagsAndAnExactDuration();
    void anM4aReportsItsIlstTags();
    void anUntaggedFileReportsTheStreamAndNoTagRows();
    void aFrameThatOverrunsTheBlockIsNotFollowed();
    void anIlstDataBoxClaimingTheWholeAddressSpaceIsRefused();
};

void TestAudioMetadata::anMp3WithId3v2_data()
{
    QTest::addColumn<int>("major");
    QTest::newRow("ID3v2.3") << 3;
    QTest::newRow("ID3v2.4") << 4;
}

void TestAudioMetadata::anMp3WithId3v2()
{
    QFETCH(int, major);

    const QByteArray file = mp3WithId3v2(major);
    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, file, file.size());

    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Blue Monday"));
    QCOMPARE(factNamed(facts, QStringLiteral("Artist")), QStringLiteral("New Order"));
    QCOMPARE(factNamed(facts, QStringLiteral("Album")), QStringLiteral("Power, Corruption & Lies"));
    QCOMPARE(factNamed(facts, QStringLiteral("Album artist")), QStringLiteral("New Order"));
    QCOMPARE(factNamed(facts, QStringLiteral("Year")), QStringLiteral("1983"));
    QCOMPARE(factNamed(facts, QStringLiteral("Track")), QStringLiteral("3/8"));
    QCOMPARE(factNamed(facts, QStringLiteral("Genre")), QStringLiteral("Synth-pop"));

    // And what the stream itself is, which no tag says.
    QCOMPARE(factNamed(facts, QStringLiteral("Bitrate")), QStringLiteral("128 kbit/s"));
    QCOMPARE(factNamed(facts, QStringLiteral("Sample rate")), QStringLiteral("44.1 kHz"));
    QCOMPARE(factNamed(facts, QStringLiteral("Channels")), QStringLiteral("stereo"));
}

void TestAudioMetadata::aCommentFrameIsTheCommentAndNotTheLanguage_data()
{
    QTest::addColumn<int>("major");
    QTest::newRow("ID3v2.3") << 3;
    QTest::newRow("ID3v2.4") << 4;
}

/// The Comment row said "eng" on essentially every MP3 that had one.
///
/// A COMM payload is encoding, a three-letter language, a description, a NUL,
/// and then the comment -- and id3Text() truncates at the first NUL, which is
/// exactly that separator. So what survived was the language, or the language
/// run together with the description ("engiTunNORM"), and setOnce() then kept
/// the ID3v1 comment from replacing it. See MOLE-383.
void TestAudioMetadata::aCommentFrameIsTheCommentAndNotTheLanguage()
{
    QFETCH(int, major);

    QByteArray frames = textFrame("TIT2", QByteArrayLiteral("Blue Monday"), major);
    frames
        += commentFrame(QByteArrayLiteral("iTunNORM"), QByteArrayLiteral("Ripped from the 12 inch"), major);
    QByteArray file = id3v2(major, frames);
    for (int i = 0; i < 40; ++i)
        file += mpegFrame();

    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, file, file.size());
    QCOMPARE(factNamed(facts, QStringLiteral("Comment")), QStringLiteral("Ripped from the 12 inch"));
    // And the title beside it, so this is not a case that passes because nothing
    // was read at all.
    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Blue Monday"));

    // A comment with an empty description is the other common shape, and it has
    // the same NUL in the same place.
    QByteArray plain = textFrame("TIT2", QByteArrayLiteral("Blue Monday"), major);
    plain += commentFrame(QByteArray(), QByteArrayLiteral("no description here"), major);
    QByteArray second = id3v2(major, plain);
    for (int i = 0; i < 40; ++i)
        second += mpegFrame();
    QCOMPARE(
        factNamed(AudioMetadataReader::factsFor(second, second, second.size()), QStringLiteral("Comment")),
        QStringLiteral("no description here"));
}

void TestAudioMetadata::anExtendedHeaderDoesNotHideEveryTag_data()
{
    QTest::addColumn<int>("major");
    QTest::newRow("ID3v2.3") << 3;
    QTest::newRow("ID3v2.4") << 4;
}

/// One flag bit made every tag in the file invisible.
///
/// readId3v2() ignored header flag 0x40, so with an extended header the first
/// "frame id" read was the extended header's own size -- whose leading byte is a
/// NUL -- and the loop broke on it as padding. Not one tag came out. See
/// MOLE-383.
void TestAudioMetadata::anExtendedHeaderDoesNotHideEveryTag()
{
    QFETCH(int, major);

    QByteArray frames = textFrame("TIT2", QByteArrayLiteral("Blue Monday"), major);
    frames += textFrame("TPE1", QByteArrayLiteral("New Order"), major);
    QByteArray file = id3v2WithAnExtendedHeader(major, frames);
    for (int i = 0; i < 40; ++i)
        file += mpegFrame();

    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, file, file.size());
    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Blue Monday"));
    QCOMPARE(factNamed(facts, QStringLiteral("Artist")), QStringLiteral("New Order"));
}

/// A v2.4 frame with a data-length indicator came out garbled.
///
/// The indicator is four extra bytes in front of the frame's own content,
/// announced by bit 0 of the second flag byte -- and it is the flag a compressed
/// or encrypted frame sets, which is ordinary in files written by anything that
/// pads. Not skipping them put four bytes of length where the encoding byte
/// should be. See MOLE-383.
void TestAudioMetadata::aFrameWithADataLengthIndicatorIsStillReadable()
{
    QByteArray frames = frameWithADataLength("TIT2", QByteArrayLiteral("Blue Monday"));
    frames += textFrame("TPE1", QByteArrayLiteral("New Order"), 4);
    QByteArray file = id3v2(4, frames);
    for (int i = 0; i < 40; ++i)
        file += mpegFrame();

    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, file, file.size());
    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Blue Monday"));
    // The frame after it is still found, which is what says the size was read
    // right as well as the content.
    QCOMPARE(factNamed(facts, QStringLiteral("Artist")), QStringLiteral("New Order"));
}

void TestAudioMetadata::anUnsynchronisedBlockIsReadTheSameWay()
{
    // A block where every 0xFF carries a 0x00 that is not part of the text. Read
    // wrong, the tags come out with NULs in them or not at all.
    const QByteArray file = mp3WithId3v2(4, true);
    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, file, file.size());

    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Blue Monday"));
    QCOMPARE(factNamed(facts, QStringLiteral("Artist")), QStringLiteral("New Order"));
}

void TestAudioMetadata::anMp3WithOnlyAnId3v1Tag()
{
    // The tag in the last 128 bytes, which is the one read from the tail.
    QByteArray file;
    for (int i = 0; i < 40; ++i)
        file += mpegFrame();
    file += id3v1();

    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, file, file.size());
    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Temptation"));
    QCOMPARE(factNamed(facts, QStringLiteral("Artist")), QStringLiteral("New Order"));
    QCOMPARE(factNamed(facts, QStringLiteral("Album")), QStringLiteral("Substance"));
    QCOMPARE(factNamed(facts, QStringLiteral("Year")), QStringLiteral("1987"));
    QCOMPARE(factNamed(facts, QStringLiteral("Track")), QStringLiteral("5"));
}

void TestAudioMetadata::aDurationWithNoHeaderToReadItFromIsMarkedAsAnEstimate()
{
    // No Xing header, so the length is arithmetic on the size: right for a
    // constant bitrate and wrong for a variable one, and the row says so.
    const QByteArray file = mp3WithId3v2(4);
    const qint64 pretendSize = 3 * 1024 * 1024; // as if the file went on
    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, file, pretendSize);

    const QString duration = factNamed(facts, QStringLiteral("Duration"));
    QVERIFY2(duration.contains(QStringLiteral("estimated")), qPrintable(duration));
    // 3 MB at 128 kbit/s is a little over three minutes.
    QVERIFY2(duration.startsWith(QStringLiteral("3:")), qPrintable(duration));
}

void TestAudioMetadata::aFlacReportsItsTagsAndAnExactDuration()
{
    const QByteArray file = flac();
    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, {}, file.size());

    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Ceremony"));
    QCOMPARE(factNamed(facts, QStringLiteral("Artist")), QStringLiteral("New Order"));
    QCOMPARE(factNamed(facts, QStringLiteral("Album")), QStringLiteral("Substance"));
    QCOMPARE(factNamed(facts, QStringLiteral("Year")), QStringLiteral("1987"));
    QCOMPARE(factNamed(facts, QStringLiteral("Track")), QStringLiteral("1"));

    // STREAMINFO carries the total number of samples, so this is exact rather
    // than worked out from the size -- and it is not labelled as an estimate.
    QCOMPARE(factNamed(facts, QStringLiteral("Duration")), QStringLiteral("3:37"));
    QCOMPARE(factNamed(facts, QStringLiteral("Sample rate")), QStringLiteral("44.1 kHz"));
    QCOMPARE(factNamed(facts, QStringLiteral("Channels")), QStringLiteral("stereo"));
    QCOMPARE(factNamed(facts, QStringLiteral("Codec")), QStringLiteral("FLAC"));
}

void TestAudioMetadata::anM4aReportsItsIlstTags()
{
    const QByteArray file = m4a();
    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, {}, file.size());

    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Bizarre Love Triangle"));
    QCOMPARE(factNamed(facts, QStringLiteral("Artist")), QStringLiteral("New Order"));
    QCOMPARE(factNamed(facts, QStringLiteral("Album")), QStringLiteral("Brotherhood"));
    QCOMPARE(factNamed(facts, QStringLiteral("Year")), QStringLiteral("1986"));
    QCOMPARE(factNamed(facts, QStringLiteral("Track")), QStringLiteral("4"));
    QCOMPARE(factNamed(facts, QStringLiteral("Duration")), QStringLiteral("3:35"));
    QCOMPARE(factNamed(facts, QStringLiteral("Codec")), QStringLiteral("AAC"));

    // **An ALAC file is not AAC.** The codec was set to "AAC" unconditionally,
    // so every lossless .m4a -- a file somebody chose deliberately for being
    // lossless -- was described as the lossy format. It comes from the sample
    // description now, through the same walk the video reader makes.
    // See MOLE-383.
    const QByteArray lossless = m4a("alac");
    QCOMPARE(factNamed(AudioMetadataReader::factsFor(lossless, {}, lossless.size()), QStringLiteral("Codec")),
        QStringLiteral("ALAC"));

    // And a version-1 movie header: the duration is 64 bits there, and this
    // reader's own copy of the box read the *high* half as a 32-bit count -- so
    // the file reported nought or millions of hours. It shares the video
    // reader's reading now. See MOLE-383.
    const QByteArray wide = m4a("mp4a", true);
    QCOMPARE(factNamed(AudioMetadataReader::factsFor(wide, {}, wide.size()), QStringLiteral("Duration")),
        QStringLiteral("3:35"));
}

/// The box the ilst reader sliced with, claiming very nearly 2^63 bytes.
///
/// A `data` box in the 64-bit form -- size 1, then eight bytes of size -- with a
/// value near INT64_MAX overflowed the box walk's own `at + size > end` check, so
/// the box was accepted, and this reader then sliced with it directly. A
/// QByteArrayView claiming that many bytes reads off the end until it faults, on
/// the details panel of a file somebody merely selected. Green under `make asan`
/// is the other half of this case. See MOLE-357.
void TestAudioMetadata::anIlstDataBoxClaimingTheWholeAddressSpaceIsRefused()
{
    // The same shape as the real fixture, with the one box lying about its size.
    const QByteArray huge = be32(1) + QByteArrayLiteral("data") + be32(0x7fffffffu) + be32(0xfffffff0u)
        + be32(1) + be32(0) + QByteArrayLiteral("this text is nowhere near that long");
    QByteArray ilst = box("\xa9"
                          "nam",
        huge);
    // And a well-formed one after it, so "the walk stops and what was read
    // stands" is asserted rather than assumed.
    ilst += box("\xa9"
                "ART",
        dataBox(QByteArrayLiteral("New Order")));

    const QByteArray meta = box("meta", box("hdlr", QByteArray(24, '\0')) + box("ilst", ilst));
    const QByteArray moov = box("moov",
        box("mvhd", be32(0) + be32(0) + be32(0) + be32(1000) + be32(215000) + QByteArray(80, '\0'))
            + box("udta", meta));
    const QByteArray file
        = box("ftyp", QByteArrayLiteral("M4A ") + be32(0) + QByteArrayLiteral("M4A mp42isom")) + moov
        + box("mdat", QByteArray(64, 'a'));

    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, {}, file.size());

    // Nothing was read out of the lying box, and the file is still described.
    QVERIFY2(factNamed(facts, QStringLiteral("Title")).isEmpty(),
        "a box claiming more than the buffer holds has nothing to say");
    QCOMPARE(factNamed(facts, QStringLiteral("Duration")), QStringLiteral("3:35"));
}

void TestAudioMetadata::anUntaggedFileReportsTheStreamAndNoTagRows()
{
    QByteArray file;
    for (int i = 0; i < 20; ++i)
        file += mpegFrame();

    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, file, file.size());
    QVERIFY(factNamed(facts, QStringLiteral("Title")).isEmpty());
    QVERIFY(factNamed(facts, QStringLiteral("Artist")).isEmpty());
    QCOMPARE(factNamed(facts, QStringLiteral("Bitrate")), QStringLiteral("128 kbit/s"));

    // And nothing at all is nothing at all, rather than invented rows.
    QVERIFY(AudioMetadataReader::factsFor(QByteArray()).isEmpty());
    QVERIFY(AudioMetadataReader::factsFor(QByteArray("plain text, not a song")).isEmpty());
}

void TestAudioMetadata::aFrameThatOverrunsTheBlockIsNotFollowed()
{
    // A frame whose declared size runs past the end of the block: the size is
    // the file's word, not a fact. Green under the sanitizers is the other half
    // of this test.
    QByteArray frames = textFrame("TIT2", QByteArrayLiteral("Real title"), 4);
    QByteArray liar = QByteArrayLiteral("TPE1") + synchsafe(0x0fffffff) + QByteArray(2, '\0')
        + QByteArray(1, '\0') + QByteArrayLiteral("truncated");
    QByteArray file = id3v2(4, frames + liar) + mpegFrame();

    const QList<FileFact> facts = AudioMetadataReader::factsFor(file, file, file.size());
    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Real title"));
    QVERIFY2(factNamed(facts, QStringLiteral("Artist")).isEmpty(), "a frame that does not fit is not read");

    // Truncated everywhere, which is what a download interrupted looks like.
    const QByteArray whole = mp3WithId3v2(4);
    for (qsizetype cut = 1; cut < whole.size(); cut += 11)
        AudioMetadataReader::factsFor(whole.left(cut), {}, whole.size());
    const QByteArray song = flac();
    for (qsizetype cut = 1; cut < song.size(); cut += 7)
        AudioMetadataReader::factsFor(song.left(cut), {}, song.size());
}

MOLE_TEST_MAIN(TestAudioMetadata)
#include "tst_AudioMetadata.moc"
