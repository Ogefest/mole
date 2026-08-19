#pragma once

#include "ui/ThumbnailSource.h"

#include <QQuickAsyncImageProvider>

namespace mole {

/// The `image://mole-thumb/...` provider: how a tile in the gallery asks for a
/// picture.
///
/// Thin on purpose. Everything about producing a thumbnail -- parsing the url,
/// choosing the thumbnailer, running it on the task pool, cancelling it -- is in
/// ThumbnailPump, which is headless and tested. This is the Qt Quick shell around
/// it, and it exists because `QQuickAsyncImageProvider` is the only shape that
/// keeps FileListModel passive and the UI thread off storage: the model hands out
/// a url, and the picture arrives when it arrives.
class ThumbnailImageProvider final : public QQuickAsyncImageProvider
{
public:
    explicit ThumbnailImageProvider(PluginServices services);
    ~ThumbnailImageProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

private:
    /// Lives on the thread that built the provider, which is the UI thread. Owned
    /// here rather than by the engine because the engine deletes the provider.
    ThumbnailPump* m_pump = nullptr;
};

} // namespace mole
