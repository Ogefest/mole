#include "plugins/builtin/previews/MediaMetadata.h"
#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"

#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <cstring>

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

/// An ISO box: its length, its four characters, and whatever is inside it.
QByteArray box(const char* type, const QByteArray& payload)
{
    return be32(quint32(payload.size() + 8)) + QByteArray(type, 4) + payload;
}

/// A movie header at `seconds`, in a timescale a real file would use.
QByteArray mvhd(quint32 timescale, quint32 ticks)
{
    QByteArray payload = be32(0); // version 0, no flags
    payload += be32(0) + be32(0); // created, modified
    payload += be32(timescale) + be32(ticks);
    payload += QByteArray(80, '\0'); // rate, volume, matrix, predefined
    return box("mvhd", payload);
}

QByteArray tkhd(int width, int height)
{
    QByteArray payload = be32(0);
    payload += be32(0) + be32(0); // created, modified
    payload += be32(1) + be32(0) + be32(0); // track id, reserved, duration
    payload += QByteArray(8, '\0'); // reserved
    payload += QByteArray(8, '\0'); // layer, alternate group, volume, reserved
    payload += QByteArray(36, '\0'); // matrix
    payload += be32(quint32(width) << 16) + be32(quint32(height) << 16);
    return box("tkhd", payload);
}

QByteArray hdlr(const char* handler)
{
    QByteArray payload = be32(0) + be32(0); // version/flags, predefined
    payload += QByteArray(handler, 4);
    payload += QByteArray(12, '\0'); // reserved
    payload += QByteArray("Track\0", 6);
    return box("hdlr", payload);
}

QByteArray mdhd(quint32 timescale, quint32 ticks)
{
    QByteArray payload = be32(0) + be32(0) + be32(0);
    payload += be32(timescale) + be32(ticks);
    payload += be32(0); // language, predefined
    return box("mdhd", payload);
}

QByteArray stsd(const char* codec)
{
    QByteArray entry = box(codec, QByteArray(70, '\0'));
    return box("stsd", be32(0) + be32(1) + entry);
}

QByteArray stsz(quint32 samples)
{
    return box("stsz", be32(0) + be32(0) + be32(samples));
}

QByteArray videoTrack(int width, int height, const char* codec, quint32 timescale, quint32 ticks,
    quint32 samples, const char* handler = "vide")
{
    QByteArray table = stsd(codec) + stsz(samples);
    QByteArray minf = box("minf", box("stbl", table));
    QByteArray mdia = box("mdia", mdhd(timescale, ticks) + hdlr(handler) + minf);
    return box("trak", tkhd(width, height) + mdia);
}

/// A whole little MP4: `ftyp`, then `moov` and `mdat` in the order asked for.
QByteArray mp4(bool indexFirst, qsizetype bodyBytes = 4096)
{
    const QByteArray ftyp
        = box("ftyp", QByteArrayLiteral("isom") + be32(512) + QByteArrayLiteral("isomavc1"));
    const QByteArray moov = box("moov",
        mvhd(1000, 187000) // 187 seconds
            + videoTrack(1920, 1080, "avc1", 1000, 187000, 4488)
            + videoTrack(0, 0, "mp4a", 48000, 8976000, 0, "soun"));
    const QByteArray mdat = box("mdat", QByteArray(bodyBytes, 'v'));

    return indexFirst ? ftyp + moov + mdat : ftyp + mdat + moov;
}

// ---- Matroska ------------------------------------------------------------

/// An EBML identifier is written as it is; a size gets its own marker.
QByteArray ebmlSize(quint64 value)
{
    // The four-byte form for everything, which is legal and keeps this simple.
    QByteArray out(4, '\0');
    const quint32 marked = quint32(value) | 0x10000000u;
    for (int i = 0; i < 4; ++i)
        out[i] = char((marked >> (8 * (3 - i))) & 0xff);
    return out;
}

QByteArray ebml(const QByteArray& id, const QByteArray& payload)
{
    return id + ebmlSize(quint64(payload.size())) + payload;
}

QByteArray ebmlUnsigned(quint64 value, int bytes)
{
    QByteArray out(bytes, '\0');
    for (int i = 0; i < bytes; ++i)
        out[i] = char((value >> (8 * (bytes - 1 - i))) & 0xff);
    return out;
}

QByteArray ebmlDouble(double value)
{
    QByteArray out(8, '\0');
    quint64 raw = 0;
    std::memcpy(&raw, &value, 8);
    for (int i = 0; i < 8; ++i)
        out[i] = char((raw >> (8 * (7 - i))) & 0xff);
    return out;
}

QByteArray webm()
{
    const QByteArray info = ebml(QByteArrayLiteral("\x15\x49\xa9\x66"),
        ebml(QByteArrayLiteral("\x2a\xd7\xb1"), ebmlUnsigned(1000000, 4))
            + ebml(QByteArrayLiteral("\x44\x89"), ebmlDouble(65000.0)));

    const QByteArray videoTrackEntry = ebml(QByteArrayLiteral("\xae"),
        ebml(QByteArrayLiteral("\x86"), QByteArrayLiteral("V_VP9"))
            + ebml(QByteArrayLiteral("\x23\xe3\x83"), ebmlUnsigned(41708333, 4))
            + ebml(QByteArrayLiteral("\xe0"),
                ebml(QByteArrayLiteral("\xb0"), ebmlUnsigned(1280, 2))
                    + ebml(QByteArrayLiteral("\xba"), ebmlUnsigned(720, 2))));
    const QByteArray audioTrackEntry
        = ebml(QByteArrayLiteral("\xae"), ebml(QByteArrayLiteral("\x86"), QByteArrayLiteral("A_OPUS")));

    const QByteArray tracks = ebml(QByteArrayLiteral("\x16\x54\xae\x6b"), videoTrackEntry + audioTrackEntry);
    const QByteArray segment = ebml(QByteArrayLiteral("\x18\x53\x80\x67"), info + tracks);
    return QByteArrayLiteral("\x1a\x45\xdf\xa3") + ebmlSize(4) + QByteArray(4, '\0') + segment;
}

// ---- AVI -----------------------------------------------------------------

QByteArray riff(const char* type, const QByteArray& payload)
{
    QByteArray out = QByteArray(type, 4) + le32(quint32(payload.size())) + payload;
    if (payload.size() % 2)
        out += '\0';
    return out;
}

QByteArray avi()
{
    QByteArray header = le32(41708); // microseconds per frame -- 24 fps
    header += le32(0) + le32(0) + le32(0);
    header += le32(2400); // total frames
    header += le32(0) + le32(2) + le32(0); // initial frames, streams, buffer
    header += le32(720) + le32(576); // width, height
    header += QByteArray(16, '\0');

    QByteArray strh = QByteArrayLiteral("vids") + QByteArrayLiteral("XVID") + QByteArray(48, '\0');
    QByteArray list = QByteArrayLiteral("hdrl") + riff("avih", header)
        + riff("LIST", QByteArrayLiteral("strl") + riff("strh", strh));

    const QByteArray body = QByteArrayLiteral("AVI ") + riff("LIST", list)
        + riff("LIST", QByteArrayLiteral("movi") + riff("00dc", QByteArray(1024, 'v')));
    return QByteArrayLiteral("RIFF") + le32(quint32(body.size())) + body;
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

/// How long a video runs, how big the picture is, and what it is coded in --
/// from the container's header, never from the file.
class TestMediaMetadata : public QObject
{
    Q_OBJECT

private slots:
    void anMp4WithItsIndexFirst();
    void anMp4WithItsIndexLast();
    void aWebmReportsTheSameSet();
    void anAviReportsTheSameSet();
    void aBoxClaimingFourGigabytesIsRefused();
    void rubbishCostsItsOwnRowsAndNothingElse();
    void anUnknownCodecIsShownAsItself();
    void noVideoIsReadWholeToBeDescribed();
};

void TestMediaMetadata::anMp4WithItsIndexFirst()
{
    const QList<FileFact> facts = VideoMetadataReader::factsFor(mp4(true));

    QCOMPARE(factNamed(facts, QStringLiteral("Duration")), QStringLiteral("3:07"));
    QCOMPARE(factNamed(facts, QStringLiteral("Picture")), QStringLiteral("1920 × 1080"));
    QCOMPARE(factNamed(facts, QStringLiteral("Frame rate")), QStringLiteral("24 fps"));
    QCOMPARE(factNamed(facts, QStringLiteral("Codecs")), QStringLiteral("H.264, AAC"));
    QCOMPARE(factNamed(facts, QStringLiteral("Tracks")), QStringLiteral("2"));
    QCOMPARE(factNamed(facts, QStringLiteral("Container")), QStringLiteral("MP4 / QuickTime"));

    QVERIFY2(!VideoMetadataReader::wantsTail(mp4(true)), "the index is in the head, so nothing else is read");
}

void TestMediaMetadata::anMp4WithItsIndexLast()
{
    // A file not written for streaming keeps its index at the end. One head read
    // finds nothing, and one bounded tail read finds everything -- rather than
    // the file being read through to reach it.
    const QByteArray whole = mp4(false, 200 * 1024);
    const QByteArray head = whole.left(VideoMetadataReader::kHeadBytes);
    const QByteArray tail = whole.right(int(VideoMetadataReader::kTailBytes));

    QVERIFY(VideoMetadataReader::wantsTail(head));
    QVERIFY(VideoMetadataReader::factsFor(head).isEmpty());

    const QList<FileFact> facts = VideoMetadataReader::factsFor(head, tail);
    QCOMPARE(factNamed(facts, QStringLiteral("Duration")), QStringLiteral("3:07"));
    QCOMPARE(factNamed(facts, QStringLiteral("Picture")), QStringLiteral("1920 × 1080"));
    QCOMPARE(factNamed(facts, QStringLiteral("Codecs")), QStringLiteral("H.264, AAC"));
}

void TestMediaMetadata::aWebmReportsTheSameSet()
{
    const QList<FileFact> facts = VideoMetadataReader::factsFor(webm());

    QCOMPARE(factNamed(facts, QStringLiteral("Duration")), QStringLiteral("1:05"));
    QCOMPARE(factNamed(facts, QStringLiteral("Picture")), QStringLiteral("1280 × 720"));
    QCOMPARE(factNamed(facts, QStringLiteral("Frame rate")), QStringLiteral("24 fps"));
    QCOMPARE(factNamed(facts, QStringLiteral("Codecs")), QStringLiteral("VP9, Opus"));
    QCOMPARE(factNamed(facts, QStringLiteral("Tracks")), QStringLiteral("2"));
    QCOMPARE(factNamed(facts, QStringLiteral("Container")), QStringLiteral("Matroska"));
}

void TestMediaMetadata::anAviReportsTheSameSet()
{
    const QList<FileFact> facts = VideoMetadataReader::factsFor(avi());

    QCOMPARE(factNamed(facts, QStringLiteral("Duration")), QStringLiteral("1:40"));
    QCOMPARE(factNamed(facts, QStringLiteral("Picture")), QStringLiteral("720 × 576"));
    QCOMPARE(factNamed(facts, QStringLiteral("Frame rate")), QStringLiteral("24 fps"));
    QCOMPARE(factNamed(facts, QStringLiteral("Codecs")), QStringLiteral("MPEG-4"));
    QCOMPARE(factNamed(facts, QStringLiteral("Container")), QStringLiteral("AVI"));
}

void TestMediaMetadata::aBoxClaimingFourGigabytesIsRefused()
{
    // A length is a number in the file, and a file is allowed to lie. What must
    // not happen is a read past the buffer or a walk that never ends.
    QByteArray hostile = box("ftyp", QByteArrayLiteral("isom"));
    hostile += be32(0xffffffffu) + QByteArrayLiteral("moov") + QByteArray(64, '\0');

    const QList<FileFact> facts = VideoMetadataReader::factsFor(hostile);
    QVERIFY2(facts.isEmpty(), "a box that does not fit is not walked");

    // A box shorter than its own header is the other way to lie, and a box of
    // length zero after a real one must not loop forever.
    QByteArray tiny = box("ftyp", QByteArrayLiteral("isom")) + be32(2) + QByteArrayLiteral("moov");
    QVERIFY(VideoMetadataReader::factsFor(tiny).isEmpty());

    // And a real file with a lying box after its index still reports the index.
    QByteArray mixed = mp4(true) + be32(0xffffffffu) + QByteArrayLiteral("free");
    QCOMPARE(factNamed(VideoMetadataReader::factsFor(mixed), QStringLiteral("Picture")),
        QStringLiteral("1920 × 1080"));
}

void TestMediaMetadata::rubbishCostsItsOwnRowsAndNothingElse()
{
    QVERIFY(VideoMetadataReader::factsFor(QByteArray()).isEmpty());
    QVERIFY(VideoMetadataReader::factsFor(QByteArray(4096, '\x01')).isEmpty());
    QVERIFY(VideoMetadataReader::factsFor(QByteArrayLiteral("just some text, honestly")).isEmpty());

    // Truncated in every interesting place: nothing found is fine, a crash is
    // not. Green under the sanitizers is the other half of this test.
    const QByteArray whole = mp4(true);
    for (qsizetype cut = 1; cut < whole.size(); cut += 7)
        VideoMetadataReader::factsFor(whole.left(cut));
    for (qsizetype cut = 1; cut < 400; ++cut)
        VideoMetadataReader::factsFor(webm().left(cut));
    for (qsizetype cut = 1; cut < 400; ++cut)
        VideoMetadataReader::factsFor(avi().left(cut));
}

void TestMediaMetadata::anUnknownCodecIsShownAsItself()
{
    // A codec nobody here has heard of is still an answer; dropping it would be
    // pretending the file did not say.
    const QByteArray file = box("ftyp", QByteArrayLiteral("isom"))
        + box("moov", mvhd(1000, 5000) + videoTrack(640, 480, "zzzz", 1000, 5000, 120));

    const QList<FileFact> facts = VideoMetadataReader::factsFor(file);
    QCOMPARE(factNamed(facts, QStringLiteral("Codecs")), QStringLiteral("zzzz"));
    QCOMPARE(factNamed(facts, QStringLiteral("Duration")), QStringLiteral("0:05"));
}

void TestMediaMetadata::noVideoIsReadWholeToBeDescribed()
{
    // Through a drive that counts, because the promise is about what was read
    // rather than about what came back: a head, and one tail for the file whose
    // index was written last.
    const QByteArray file = mp4(false, 8 * 1024 * 1024);

    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/holiday.mp4"), file);
    auto counted = std::make_shared<test::FaultyFileSystem>(memory);

    VfsManager manager;
    Mount mount;
    mount.id = QStringLiteral("counted");
    mount.displayName = QStringLiteral("counted");
    mount.root = VfsUri::fromString(QStringLiteral("mem://counted/"));
    mount.fileSystem = counted;
    QVERIFY(!manager.addMount(mount).isEmpty());

    PluginServices services;
    services.vfs = &manager;

    FileEntry entry;
    entry.name = QStringLiteral("holiday.mp4");
    entry.uri = VfsUri::fromString(QStringLiteral("mem://counted/holiday.mp4"));
    entry.size = file.size();
    entry.mimeType = QStringLiteral("video/mp4");

    const VideoMetadataReader reader;
    QVERIFY(reader.canRead(entry));

    const CancelToken cancel;
    const QList<FileFact> facts = reader.read(entry, QByteArrayView(), services, cancel);
    QCOMPARE(factNamed(facts, QStringLiteral("Picture")), QStringLiteral("1920 × 1080"));

    const qint64 allowed = VideoMetadataReader::kHeadBytes + VideoMetadataReader::kTailBytes;
    QVERIFY2(counted->bytesRead() <= allowed,
        qPrintable(
            QStringLiteral("read %1 bytes of an %2 byte file").arg(counted->bytesRead()).arg(file.size())));
    QVERIFY2(counted->bytesRead() > VideoMetadataReader::kHeadBytes,
        "and the tail really was read, or this proves nothing");
}

MOLE_TEST_MAIN(TestMediaMetadata)
#include "tst_MediaMetadata.moc"
