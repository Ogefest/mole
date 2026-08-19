#include "plugins/builtin/previews/VideoPreview.h"

#include "plugins/builtin/previews/PreviewProviders.h"

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

VideoPreviewController::VideoPreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
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
    if (!isAvailable())
        return {};

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
    static const QStringList supported = videoSuffixes();
    return supported.contains(entry.uri.suffix());
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
