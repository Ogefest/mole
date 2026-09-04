#pragma once

#include "sdk/IMetadataReader.h"

namespace mole {

/// The title, artist and album an audio file carries inside it.
///
/// Four tag formats cover the files people actually have: ID3v2 at the head of
/// an MP3, ID3v1 in its last 128 bytes, Vorbis comments in a FLAC or an Ogg, and
/// an MP4 `ilst` for an `.m4a`. The last of those is the box walk MOLE-135 wrote
/// for video, used rather than written again.
///
/// **Bounded, and no cover art.** 64 kB at the head and one 4 kB read at the
/// tail for an ID3v1 tag. The picture in a tag block is the one thing in it that
/// is megabytes rather than bytes, and nobody asked for it.
///
/// **An estimate is labelled as one.** An MP3 with no Xing or VBRI header has no
/// duration written down anywhere, so it is worked out from the first frame's
/// bitrate and the file's size -- which is right for a constant bitrate and
/// wrong for a variable one. The row says so, because an estimate presented as a
/// fact is worse than an estimate.
class AudioMetadataReader final : public IMetadataReader
{
public:
    QString id() const override { return QStringLiteral("mole.metadata.audio"); }
    int priority() const override { return 100; }
    bool canRead(const FileEntry& entry) const override;

    /// Every suffix the MIME database calls audio. Read once, because the
    /// database walk is not free and the answer does not change.
    static QStringList audioSuffixes();
    QList<FileFact> read(const FileEntry& entry, QByteArrayView head, PluginServices services,
        const CancelToken& cancel) const override;

    static constexpr qint64 kHeadBytes = 64 * 1024;
    /// Enough for the 128 bytes of an ID3v1 tag, read as one block.
    static constexpr qint64 kTailBytes = 4 * 1024;

    /// Everything derivable from a head, a tail and the file's size, with no I/O.
    /// `fileSize` is only used for the duration of an MP3 that does not say.
    static QList<FileFact> factsFor(QByteArrayView head, QByteArrayView tail = {}, qint64 fileSize = -1);
};

} // namespace mole
