#pragma once

#include "sdk/IThumbnailer.h"

namespace mole {

#ifdef MOLE_HAVE_MULTIMEDIA

/// A frame from a video, so a folder of them is not a wall of identical icons.
///
/// Two things this has to get right, or a gallery of videos is worse than icons:
///
/// - **Not frame zero.** A great many videos open on black or on a fade from it,
///   and a folder of black tiles is less useful than a folder of icons. It seeks a
///   little way in -- a small fraction of the duration, with a floor and a ceiling
///   in seconds -- and takes the frame there.
/// - **A hard time limit per file.** This is the one thumbnail here that can take
///   an unbounded amount of time on a file that is damaged, oddly encoded or on a
///   slow drive. Past the limit it gives up and the tile stays an icon.
///
/// It is local-only, and that is not a limitation to remove later: decoding a
/// frame means seeking, and a drive that answers a seek by downloading the file up
/// to it is the case MOLE-143 exists to avoid. A video on a bucket gets an icon.
class VideoThumbnailer final : public IThumbnailer
{
public:
    QString id() const override { return QStringLiteral("mole.thumb.video"); }
    int priority() const override { return 100; }

    bool canThumbnail(const FileEntry& entry) const override;
    QImage thumbnail(const FileEntry& entry, int size, const PluginServices& services,
        const CancelToken& cancel) const override;

    /// Whether this build can decode any video at all. Asked of Qt, like the video
    /// viewer's own availability, because a build with the module and no codecs
    /// claims nothing.
    static bool isAvailable();

    /// How far in the frame is taken from: a tenth of the way through, never less
    /// than the floor and never more than the ceiling -- and never past nine
    /// tenths of the file, so a two-second clip still has a frame to give.
    ///
    /// Two seconds rather than one, because an opening title or a fade from black
    /// is routinely a second long and a tile of black is worse than an icon.
    static constexpr qint64 kMinimumSeekMs = 2000;
    static constexpr qint64 kMaximumSeekMs = 10000;
    /// Everything about one file, decode included, inside this. A tile is not
    /// worth a thread for longer than somebody would wait for it.
    static constexpr int kTimeLimitMs = 5000;
};

#endif // MOLE_HAVE_MULTIMEDIA

} // namespace mole
