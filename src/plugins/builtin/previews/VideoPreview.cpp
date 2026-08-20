#include "plugins/builtin/previews/VideoPreview.h"

#include "plugins/builtin/previews/PreviewProviders.h"

#include "core/settings/Preferences.h"

#include <QMimeDatabase>
#include <QMimeType>

#ifdef MOLE_HAVE_MULTIMEDIA
#include <QMediaFormat>
#endif

namespace mole {
namespace {

    QUrl qmlView(const char* name)
    {
        return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/%1").arg(QLatin1String(name)));
    }

} // namespace

#ifdef MOLE_HAVE_MULTIMEDIA

namespace {

    /// One key for every video, not one per suffix.
    ///
    /// The opposite of what ADR-0006 keys a viewer *option* by, and on purpose:
    /// *render this .html as a page* is a choice about a file type, and *this room
    /// is quiet* is a choice about the person -- remembering it separately for
    /// `.mp4` and `.mkv` would be the surprise rather than the courtesy. Same
    /// argument, same shape, as `preview.details.open`.
    const QString kMutedKey = QStringLiteral("preview.video.muted");

} // namespace

VideoPreviewController::VideoPreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_services(services)
    , m_copy(new LocalCopyProvider(services, this))
{
    connect(m_copy, &LocalCopyProvider::ready, this, [this](const QString& url) {
        setLoading(false);
        m_source = url;
        emit sourceChanged();
    });
    connect(m_copy, &LocalCopyProvider::failed, this, [this](const QString& reason) {
        setLoading(false);
        setErrorText(reason);
    });

    // Read on the way in, which is what makes the next video open the way the last
    // one was left: a controller is built per file, so there is no state here to
    // carry and the preference is the only thing that survives between them.
    if (m_services.preferences)
        m_muted = m_services.preferences->value(kMutedKey, false).toBool();
}

void VideoPreviewController::load(const FileEntry& entry)
{
    setErrorText({});
    setLoading(true);
    m_source.clear();
    emit sourceChanged();
    // No cap on the bytes: a player seeks, and half a container is not a video.
    // A remote file is therefore fetched whole, which is what opening a remote
    // video means -- the alternative is a viewer that plays the first minute of
    // something and stops.
    m_copy->request(entry.uri);
}

void VideoPreviewController::setMuted(bool muted)
{
    // Written before the early return, the shape setDetailsOpen() uses: the value
    // belongs to the application rather than to this viewer, and this controller is
    // not necessarily the one that last wrote it. Preferences::setValue() does
    // nothing when the value is already what is stored, so this costs no file
    // write.
    if (m_services.preferences)
        m_services.preferences->setValue(kMutedKey, muted);

    if (m_muted == muted)
        return;
    m_muted = muted;
    emit mutedChanged();
}

void VideoPreviewController::reportPlaybackFailure(const QString& reason)
{
    setLoading(false);
    setErrorText(reason.trimmed().isEmpty()
            ? QStringLiteral("This build cannot decode this video. The details panel says what the "
                             "file is.")
            : reason);
}

#endif // MOLE_HAVE_MULTIMEDIA

VideoPreviewProvider::VideoPreviewProvider(PluginServices services)
    : m_services(services)
{
}

bool VideoPreviewProvider::isAvailable()
{
#ifdef MOLE_HAVE_MULTIMEDIA
    // The codec list is the half of QMediaFormat the GStreamer backend fills in,
    // and an empty one is a build that can decode no video at all.
    static const bool any = !QMediaFormat().supportedVideoCodecs(QMediaFormat::Decode).isEmpty();
    return any;
#else
    return false;
#endif
}

QStringList VideoPreviewProvider::videoSuffixes()
{
    // Deliberately not gated on isAvailable(). What a video file is called is a
    // question for the MIME database and costs nothing; whether this build can
    // decode one wakes the whole GStreamer stack. Folding the two together meant
    // every caller paid the second price to ask the first -- see the note in the
    // header. Callers check availability themselves, after the suffix.
    QStringList suffixes;
    const QList<QMimeType> types = QMimeDatabase().allMimeTypes();
    for (const QMimeType& type : types) {
        if (!type.name().startsWith(QLatin1String("video/")))
            continue;
        const QStringList known = type.suffixes();
        for (const QString& suffix : known) {
            const QString lower = suffix.toLower();
            if (!suffixes.contains(lower))
                suffixes.append(lower);
        }
    }
    return suffixes;
}

bool VideoPreviewProvider::canPreview(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;
    // The suffix first and the decoder second, and that order is the whole point:
    // only a file that really looks like a video is worth asking Qt Multimedia
    // about, and asking it anything costs most of a second.
    static const QStringList supported = videoSuffixes();
    return supported.contains(entry.uri.suffix()) && isAvailable();
}

QUrl VideoPreviewProvider::viewSource() const
{
    return qmlView("VideoPreview.qml");
}

PreviewController* VideoPreviewProvider::createController(QObject* parent)
{
#ifdef MOLE_HAVE_MULTIMEDIA
    return new VideoPreviewController(m_services, parent);
#else
    Q_UNUSED(parent);
    return nullptr;
#endif
}

} // namespace mole
