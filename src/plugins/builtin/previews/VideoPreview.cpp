#include "plugins/builtin/previews/VideoPreview.h"

#include "plugins/builtin/previews/PreviewProviders.h"

#include "core/settings/Preferences.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPluginLoader>

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
    // A refusal with nothing to say still says something: the reason travels into
    // the strip, and an empty one there would leave the reader with a viewer that
    // changed under them for no stated cause.
    decline(
        reason.trimmed().isEmpty() ? QStringLiteral("this build has no decoder for it") : reason.trimmed());
}

#endif // MOLE_HAVE_MULTIMEDIA

VideoPreviewProvider::VideoPreviewProvider(PluginServices services)
    : m_services(services)
{
}

#ifdef MOLE_HAVE_MULTIMEDIA
/// Can a media backend plugin actually be loaded?
///
/// Asked of the plugin files rather than of Qt Multimedia, because the only Qt
/// question that answers it -- `QMediaFormat::supportedVideoCodecs()` -- aborts when
/// the answer is no. Qt loads a backend from a `multimedia` directory under one of
/// its library paths; with nothing loadable there, `QMediaFormat` has nothing behind
/// it and touching it ends the process.
///
/// **`QPluginLoader::load()` and not "is the file there".** That was the first
/// attempt and it does not work: the bundle carries `libffmpegmediaplugin.so`, so the
/// file is always present, and on a machine without libx264 and libx265 -- which the
/// bundle deliberately does not carry, see make-bundle.sh -- Ubuntu's libavcodec
/// cannot load, so the plugin cannot either. Existing and loading are different
/// questions and only the second one is safe to build on. `load()` is a dlopen: it
/// fails and says why, rather than aborting.
///
/// A plugin that loads is still not a promise that it decodes: GStreamer may have no
/// elements behind it, which is why an empty codec list is treated generously below.
/// This rules out only the case that cannot be survived.
static bool aMediaBackendExists()
{
    QStringList roots = QCoreApplication::libraryPaths();
    roots.append(QLibraryInfo::path(QLibraryInfo::PluginsPath));
    for (const QString& root : roots) {
        const QDir dir(root + QStringLiteral("/multimedia"));
        if (!dir.exists())
            continue;
        const QStringList candidates = dir.entryList({ QStringLiteral("*.so") }, QDir::Files);
        for (const QString& candidate : candidates) {
            // Left loaded on purpose. Qt is about to load it again for itself, and
            // unloading a plugin that has already registered its factories is a
            // hazard with nothing to gain.
            QPluginLoader loader(dir.absoluteFilePath(candidate));
            if (loader.load())
                return true;
        }
    }
    return false;
}
#endif

const VideoPreviewProvider::Probe& VideoPreviewProvider::probe()
{
    static const Probe probe = [] {
        Probe out;
        out.requestedBackend = qEnvironmentVariable("QT_MEDIA_BACKEND");
#ifdef MOLE_HAVE_MULTIMEDIA
        out.moduleBuiltIn = true;
        out.backendPresent = aMediaBackendExists();
        // Only asked when there is something to answer it. See the note on
        // Probe::backendPresent: with no backend this call aborts rather than
        // returning nothing, so the guard is not a tidiness measure.
        if (out.backendPresent) {
            const QList<QMediaFormat::VideoCodec> codecs
                = QMediaFormat().supportedVideoCodecs(QMediaFormat::Decode);
            for (QMediaFormat::VideoCodec codec : codecs)
                out.decodableVideoCodecs.append(QMediaFormat::videoCodecName(codec));
        }
#endif
        return out;
    }();
    return probe;
}

bool VideoPreviewProvider::videoIsWorthTrying(const Probe& probe)
{
    // Deliberately not "and it listed a codec". An empty list from a backend
    // that is present and working is indistinguishable here from no multimedia
    // at all, and treating the two the same made videos stop existing in the
    // application with nothing said about it.
    //
    // **But a backend has to exist.** Without one there is nothing to try: the
    // player cannot be built, and the call that would have said so is the call that
    // ends the process. A video then gets the ordinary fact sheet, which is what any
    // file Mole cannot preview gets, rather than taking the application down with
    // it. See MOLE-316.
    return probe.moduleBuiltIn && probe.backendPresent;
}

QStringList VideoPreviewProvider::diagnosticLines()
{
    const Probe& current = probe();

    QStringList out;
    out.append(
        QStringLiteral("multimedia module: %1")
            .arg(current.moduleBuiltIn ? QStringLiteral("built in") : QStringLiteral("not in this build")));
    if (!current.moduleBuiltIn)
        return out;

    // "asked for" rather than "in use", because Qt 6.4 has no API that names the
    // backend actually chosen. Saying `media backend: ffmpeg` and then that none is
    // installed read as a contradiction; the two lines are about different things
    // and now say which.
    out.append(QStringLiteral("media backend asked for: %1")
                   .arg(current.requestedBackend.isEmpty()
                           ? QStringLiteral("the platform default (QT_MEDIA_BACKEND is not set)")
                           : current.requestedBackend));

    // **Three answers, not two**, because they lead to three different behaviours
    // and the middle one used to be reported as the last. No backend at all means
    // a video gets the ordinary fact sheet; a backend that names no codec means
    // videos are tried and a failure is reported per file; a list means it decodes.
    // Telling the first two apart is what MOLE-316 asked for, and it is the
    // difference between "install a decoder" and "this file is not one".
    if (!current.backendPresent) {
        out.append(QStringLiteral("backend found: none that will load, so a video gets the file-information "
                                  "view rather than a frame. See \"Video previews\" in README.md for what to "
                                  "install"));
        return out;
    }
    out.append(QStringLiteral("backend found: yes"));
    out.append(QStringLiteral("video codecs it reports: %1")
                   .arg(current.decodableVideoCodecs.isEmpty()
                           ? QStringLiteral("none -- the backend did not say, so videos are tried anyway "
                                            "and a failure is reported per file")
                           : current.decodableVideoCodecs.join(QStringLiteral(", "))));
    return out;
}

bool VideoPreviewProvider::isAvailable()
{
    return videoIsWorthTrying(probe());
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
