#pragma once

#include "sdk/IPreviewProvider.h"

#include <QStringList>

namespace mole {

class LocalCopyProvider;

#ifdef MOLE_HAVE_MULTIMEDIA

/// A video, as its first frame and a play button.
///
/// Playback itself is the view's business: `MediaPlayer` and `VideoOutput` do it,
/// and this hands them a file and gets out of the way. What is here is the part a
/// QML file must not do -- fetching a copy of anything that is not on a local
/// disk, because `MediaPlayer` cannot open an `archive://` uri or a remote one.
/// The image and document viewers reach for `LocalCopyProvider` for the same
/// reason; see docs/adr/0004-pdf-previews.md.
///
/// **It does not start playing.** `F3` walks a folder with the arrows, and a
/// viewer that begins making noise as the cursor passes over a file is the wrong
/// default: the first frame with a play button on it is what the tab shows.
class VideoPreviewController final : public PreviewController
{
    Q_OBJECT
    /// A `file:` url the player can open, empty until the copy is in place.
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)

public:
    explicit VideoPreviewController(PluginServices services, QObject* parent = nullptr);

    QString source() const { return m_source; }
    void load(const FileEntry& entry) override;

    /// What the view calls when the player refuses the file.
    ///
    /// A container this build can demux may still hold a stream it has no decoder
    /// for, and there is no way to know before trying -- see
    /// VideoPreviewProvider::videoSuffixes(). So the answer is to say what
    /// happened, in the same place every other viewer says it, rather than to
    /// leave a black frame that reads as a broken file.
    Q_INVOKABLE void reportPlaybackFailure(const QString& reason);

signals:
    void sourceChanged();

private:
    QString m_source;
    LocalCopyProvider* m_copy = nullptr;
};

#endif // MOLE_HAVE_MULTIMEDIA

/// Claims what a video file is called -- but only in a build that can play one.
///
/// Without `Qt6::Multimedia` this provider still exists and still refuses every
/// file, so a video falls through to the information viewer rather than opening a
/// frame nothing can fill. The image and document viewers set the precedent by
/// claiming only what their build can decode.
class VideoPreviewProvider final : public IPreviewProvider
{
public:
    explicit VideoPreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.video"); }
    QString displayName() const override { return QStringLiteral("Video"); }
    /// Above the text viewer, which would otherwise read a container as bytes.
    /// It never meets the image or document viewers: a suffix belongs to one
    /// family or another.
    int priority() const override { return 60; }
    bool canPreview(const FileEntry& entry) const override;
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

    /// True when this build can decode any video at all.
    ///
    /// Asked of Qt, which answers this one accurately:
    /// `QMediaFormat::supportedVideoCodecs(Decode)` is what the backend really
    /// found -- H264, H265, VP8, Theora and so on, one entry per decoder
    /// installed. An empty list is a build with no video decoders, and then
    /// claiming a `.mp4` would open a black frame instead of showing the file's
    /// details.
    static bool isAvailable();

    /// The suffixes a video file goes by, from the system's own MIME database.
    ///
    /// **Not from `QMediaFormat::supportedFileFormats()`, and that is a measured
    /// decision rather than an oversight.** On a machine whose GStreamer decodes
    /// H.264 in MP4, VP8 in WebM and Matroska without complaint, Qt 6.4 lists
    /// exactly two decodable file formats: AVI and Wave. The container half of
    /// that API is not populated by the GStreamer backend, so believing it would
    /// mean refusing every format anybody previews video in. The codec half *is*
    /// populated, which is what isAvailable() asks.
    ///
    /// So the containers come from `QMimeDatabase` -- every `video/*` type the
    /// system knows, which is still asked rather than hard-coded and still
    /// narrows with a narrower installation -- and whether the stream inside can
    /// actually be decoded is answered by trying, with the failure reported in
    /// words. See VideoPreviewController::reportPlaybackFailure().
    static QStringList videoSuffixes();

private:
    PluginServices m_services;
};

} // namespace mole
