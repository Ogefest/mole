#include "plugins/builtin/thumbnails/VideoThumbnailer.h"

#ifdef MOLE_HAVE_MULTIMEDIA

#include "plugins/builtin/previews/VideoPreview.h"
#include "plugins/builtin/thumbnails/ThumbnailLimits.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

namespace mole {

bool VideoThumbnailer::isAvailable()
{
    // The same question the video viewer asks, and the same answer: a build with
    // the module and no codecs can decode nothing.
    return VideoPreviewProvider::isAvailable();
}

bool VideoThumbnailer::canThumbnail(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;
    // No I/O, and no seeking anywhere near a drive that would download to answer
    // one: a video that is not on the local disk gets the icon tile.
    if (thumbnails::isRemote(entry.uri))
        return false;
    static const QStringList supported = VideoPreviewProvider::videoSuffixes();
    return supported.contains(entry.uri.suffix());
}

QImage VideoThumbnailer::thumbnail(
    const FileEntry& entry, int size, PluginServices services, const CancelToken& cancel) const
{
    Q_UNUSED(services);
    if (size <= 0 || cancel.isCancelled() || !isAvailable())
        return {};

    const QString path = entry.uri.toLocalPath();
    if (path.isEmpty())
        return {};

    // Everything here lives on this thread, which is a task pool thread: a player
    // is a QObject and its signals are only delivered where its own event loop
    // runs. The loop below is that event loop, and it exists for the length of one
    // file and no longer.
    QMediaPlayer player;
    QVideoSink sink;
    player.setVideoSink(&sink);
    player.setSource(QUrl::fromLocalFile(path));

    QEventLoop loop;
    QImage grabbed;
    bool sought = false;
    qint64 seekTo = 0;

    // A hard limit, and it is what makes this safe to run at all: decoding a frame
    // from a damaged or oddly encoded file can take an unbounded amount of time.
    QTimer limit;
    limit.setSingleShot(true);
    limit.setInterval(kTimeLimitMs);
    QObject::connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
    limit.start();

    // Polled for cancellation on the same loop, because the token is set by
    // another thread and nothing here would otherwise notice.
    QTimer watch;
    watch.setInterval(100);
    QObject::connect(&watch, &QTimer::timeout, &loop, [&] {
        if (cancel.isCancelled())
            loop.quit();
    });
    watch.start();

    QObject::connect(&sink, &QVideoSink::videoFrameChanged, &loop, [&](const QVideoFrame& frame) {
        if (!frame.isValid() || !sought)
            return; // whatever arrived before the seek is the opening frame
        // And a seek is not instant: the pipeline goes on delivering frames from
        // where it was until it gets there, and those are the black ones this
        // whole arrangement exists to avoid. The position is what says so.
        if (player.position() + 40 < seekTo)
            return;
        QImage image = frame.toImage();
        if (image.isNull())
            return;
        grabbed = std::move(image);
        loop.quit();
    });

    // Where the frame is chosen, and *when* is the whole of it. LoadedMedia says the
    // file has been opened, not that the pipeline can be moved: at that point
    // isSeekable() is still false and setPosition() is dropped without an error or
    // a signal, leaving the player at zero. Playing then runs the file from its
    // beginning and the position guard below rejects every frame until playback has
    // genuinely arrived at the target -- so the tile costs its own seek offset in
    // wall clock, and anything past the time limit never produces one at all.
    //
    // seekableChanged is the signal that says a seek will land. It comes a moment
    // after LoadedMedia and before any frame. Asked at every status as well, and
    // guarded on isSeekable() rather than on which signal woke it: a source already
    // seekable when it finished loading never emits the change.
    auto seekAndPlay = [&] {
        if (sought || !player.isSeekable())
            return;
        // A tenth of the way in, bounded: a title card is usually over by a
        // second, and a minute in is a scene nobody would recognise the file by.
        const qint64 duration = player.duration();
        // Never past nine tenths of the file, so a two-second clip still has a
        // frame to give rather than seeking off the end of itself.
        const qint64 latest = duration > 0 ? qMax<qint64>(1, duration * 9 / 10) : kMinimumSeekMs;
        const qint64 at = duration > 0
            ? qBound<qint64>(qMin(kMinimumSeekMs, latest), duration / 10, qMin(kMaximumSeekMs, latest))
            : kMinimumSeekMs;
        sought = true;
        seekTo = at;
        player.setPosition(at);
        // Playing is what makes the pipeline produce a frame at all; it is
        // stopped the moment one arrives.
        player.play();
    };

    QObject::connect(&player, &QMediaPlayer::seekableChanged, &loop, [&](bool) { seekAndPlay(); });

    QObject::connect(
        &player, &QMediaPlayer::mediaStatusChanged, &loop, [&](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::InvalidMedia) {
                loop.quit();
                return;
            }
            seekAndPlay();
        });

    QObject::connect(&player, &QMediaPlayer::errorOccurred, &loop,
        [&](QMediaPlayer::Error, const QString&) { loop.quit(); });

    loop.exec();
    player.stop();
    player.setVideoSink(nullptr);

    if (grabbed.isNull() || cancel.isCancelled())
        return {}; // past the limit, undecodable, or cancelled: the tile stays an icon

    if (grabbed.width() > size || grabbed.height() > size)
        grabbed = grabbed.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return grabbed;
}

} // namespace mole

#endif // MOLE_HAVE_MULTIMEDIA
