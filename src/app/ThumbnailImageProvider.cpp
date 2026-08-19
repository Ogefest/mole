#include "ThumbnailImageProvider.h"

#include <QQuickTextureFactory>

namespace mole {

/// One tile's request. Completed from the UI thread by the pump; Qt deletes it
/// once it has emitted finished().
class ThumbnailResponse final : public QQuickImageResponse
{
    Q_OBJECT

public:
    QQuickTextureFactory* textureFactory() const override
    {
        // Null for a file with no thumbnail, which is not an error: the delegate
        // keeps the icon tile it already had and nothing is said about it.
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    void cancel() override
    {
        // Qt calls this when the Image asking for the picture is destroyed, which
        // is what a GridView does the moment a delegate leaves its cache buffer.
        // It has to reach the task, or a flick through a folder leaves a queue of
        // decodes nobody is waiting for.
        emit cancelled();
    }

    void complete(const QImage& image)
    {
        m_image = image;
        emit finished();
    }

signals:
    void cancelled();

private:
    QImage m_image;
};

ThumbnailImageProvider::ThumbnailImageProvider(PluginServices services)
    : m_cache(std::make_unique<ThumbnailCache>())
    , m_pump(new ThumbnailPump(services, m_cache.get()))
{
    QObject::connect(m_pump, &ThumbnailPump::ready, m_pump, [](QObject* response, const QImage& image) {
        if (auto* waiting = qobject_cast<ThumbnailResponse*>(response))
            waiting->complete(image);
    });
}

ThumbnailImageProvider::~ThumbnailImageProvider()
{
    delete m_pump;
}

QQuickImageResponse* ThumbnailImageProvider::requestImageResponse(
    const QString& id, const QSize& requestedSize)
{
    Q_UNUSED(requestedSize); // the size is in the url, because it is part of the key

    auto* response = new ThumbnailResponse;
    // Queued, and deliberately so: this may be called from Qt's pixmap reader
    // thread, while TaskManager has to be driven from the thread that owns it.
    QMetaObject::invokeMethod(
        m_pump, "startFor", Qt::QueuedConnection, Q_ARG(QObject*, response), Q_ARG(QString, id));
    QObject::connect(
        response, &ThumbnailResponse::cancelled, m_pump, [this, response] { m_pump->cancelFor(response); });
    return response;
}

} // namespace mole

#include "ThumbnailImageProvider.moc"
