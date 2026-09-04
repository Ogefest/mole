#pragma once

#include "sdk/IMetadataReader.h"

#include <QByteArrayView>
#include <QList>

#include <optional>

namespace mole {

/// One box in an ISO base media file, already checked to lie inside the buffer
/// it was found in.
///
/// `.mp4`, `.m4v`, `.mov` and `.m4a` are all this format, which is why the walk
/// is here rather than inside one reader: the video reader wants `moov/trak`
/// and the audio reader wants `moov/udta/meta/ilst`, and neither should write
/// the tree walk twice.
struct IsoBox
{
    /// Four characters, as they appear in the file.
    QByteArray type;
    /// Where the box starts within the buffer.
    qint64 offset = 0;
    /// The whole box, header included.
    qint64 size = 0;
    /// 8, or 16 when the box carries a 64-bit size.
    qint64 headerBytes = 8;

    qint64 payloadOffset() const { return offset + headerBytes; }
    qint64 payloadBytes() const { return size - headerBytes; }
};

/// The boxes directly inside `[offset, offset + length)`.
///
/// **Every length in the file is a claim.** A box that says it is four gigabytes
/// long inside a 64 kB buffer is refused, and so is one that says it is shorter
/// than its own header; what has been read so far is kept and the walk stops
/// there. Same lesson as ADR-0010's archive entries.
QList<IsoBox> isoBoxesIn(QByteArrayView bytes, qint64 offset, qint64 length);

/// The first box of `type` directly inside a range, if it is there.
std::optional<IsoBox> isoBoxNamed(QByteArrayView bytes, qint64 offset, qint64 length, const char* type);

/// A path of nested boxes, e.g. `moov/udta/meta`. Empty when any step is missing.
std::optional<IsoBox> isoBoxAt(QByteArrayView bytes, const QList<QByteArray>& path);

/// What a video file says about itself: how long it runs, how big the picture
/// is, what it is coded in.
///
/// Three container families cover very nearly everything anybody has: ISO base
/// media (`.mp4`, `.mov`), Matroska and WebM, and AVI. All three keep it in a
/// header, so this is a header read like every other reader here -- 64 kB at the
/// front, and one bounded read at the tail for the ISO files whose index was
/// written last.
///
/// This is not playback. MOLE-37 is playback and needs Qt Multimedia; in a build
/// without it, this panel is what a video has instead of nine numbers.
class VideoMetadataReader final : public IMetadataReader
{
public:
    QString id() const override { return QStringLiteral("mole.metadata.video"); }
    int priority() const override { return 100; }
    bool canRead(const FileEntry& entry) const override;
    QList<FileFact> read(const FileEntry& entry, QByteArrayView head, const PluginServices& services,
        const CancelToken& cancel) const override;

    /// The head is enough for a file written for streaming; the tail is where an
    /// ISO file that was not keeps its index.
    static constexpr qint64 kHeadBytes = 64 * 1024;
    static constexpr qint64 kTailBytes = 256 * 1024;

    /// Everything derivable from a head and, when there is one, a tail. No I/O:
    /// the caller has already read whatever it is handing over.
    static QList<FileFact> factsFor(QByteArrayView head, QByteArrayView tail = {});

    /// Whether the head alone cannot answer -- an ISO file whose `moov` is not in
    /// it. The one case that costs a second read.
    static bool wantsTail(QByteArrayView head);
};

/// A codec's four-character code or CodecID as a person says it, or the raw
/// identifier when it is not in the table. Shared with the audio reader.
QString codecName(const QString& identifier);

/// How long the movie in `moov` is, in seconds, from its `mvhd`.
///
/// Shared so there is one reading of the box rather than two. The audio reader
/// had its own, and its own read the version-1 duration as 32 bits -- taking the
/// *high* half of a 64-bit field, so a file written with the wide header
/// reported a duration in the millions of hours or nought. See MOLE-383.
std::optional<double> isoMovieSeconds(QByteArrayView bytes, const IsoBox& moov);

/// The four characters naming the codec of the first sample entry under `moov`,
/// or empty.
///
/// Also shared, and for the same reason: the audio reader was setting "AAC"
/// unconditionally, so every ALAC .m4a -- a lossless file somebody chose
/// deliberately -- was described as AAC. See MOLE-383.
QString isoFirstCodec(QByteArrayView bytes, const IsoBox& moov);

/// `1:03:07`, or `3:07` for anything under an hour.
QString durationText(double seconds);

} // namespace mole
