#pragma once

#include "sdk/IPreviewProvider.h"

#include <QStringList>

namespace mole {

class LocalCopyProvider;

#ifdef MOLE_HAVE_MULTIMEDIA

/// A video, playing, with somewhere to pause it and a position to drag.
///
/// Playback itself is the view's business: `MediaPlayer` and `VideoOutput` do it,
/// and this hands them a file and gets out of the way. What is here is the part a
/// QML file must not do -- fetching a copy of anything that is not on a local
/// disk, because `MediaPlayer` cannot open an `archive://` uri or a remote one.
/// The image and document viewers reach for `LocalCopyProvider` for the same
/// reason; see docs/adr/0004-pdf-previews.md.
///
/// **It starts playing by itself**, including a file stepped onto with the arrows.
/// Opening a preview of a video is asking to see it move, and one rule that is
/// always true beats a viewer that plays or does not depending on how somebody
/// arrived at the file. It opened paused until MOLE-223, on the argument that `F3`
/// walks a folder with the arrows and a viewer making noise as the cursor passes
/// over a file is the wrong default -- overruled knowingly, with the cost written
/// down in docs/adr/0053-a-video-preview-plays-itself.md.
class VideoPreviewController final : public PreviewController
{
    Q_OBJECT
    /// A `file:` url the player can open, empty until the copy is in place.
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)
    /// Whether the sound is off. Remembered, so the view binds rather than holds it.
    Q_PROPERTY(bool muted READ isMuted NOTIFY mutedChanged)

public:
    explicit VideoPreviewController(PluginServices services, QObject* parent = nullptr);

    QString source() const { return m_source; }
    bool isMuted() const { return m_muted; }
    void load(const FileEntry& entry) override;

    /// Turns the sound off or on and remembers which, for every video and across
    /// restarts.
    ///
    /// **One key for every video rather than one per suffix**, which is the
    /// opposite of what ADR-0006 keys a viewer *option* by, and deliberately:
    /// whether the room is quiet is a fact about the person, not about a container
    /// format, so remembering it per `.mp4` and per `.mkv` separately would be a
    /// surprise rather than a courtesy. It is the argument
    /// `PreviewTabController::setDetailsOpen()` already makes for the details
    /// panel. See the revision in
    /// docs/adr/0053-a-video-preview-plays-itself.md.
    Q_INVOKABLE void setMuted(bool muted);

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
    void mutedChanged();

private:
    PluginServices m_services;
    QString m_source;
    bool m_muted = false;
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
    ///
    /// **This answers regardless of isAvailable(), and must keep doing so.** The
    /// MIME database is free; isAvailable() builds the GStreamer stack, at 710 ms
    /// and five threads. Folding the check in here made every caller pay that to
    /// learn what a file is called, which put it on application startup. Ask this
    /// first and isAvailable() only for a file whose suffix matched.
    static QStringList videoSuffixes();

private:
    PluginServices m_services;
};

} // namespace mole
