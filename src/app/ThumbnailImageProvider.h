#pragma once

#include "ui/ThumbnailCache.h"
#include "ui/ThumbnailSource.h"

#include <QQuickAsyncImageProvider>

#include <memory>

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

    /// The queue behind the provider. Exposed so a test can drive the real view
    /// and still see how many decodes are running, which is a claim about the two
    /// together and cannot be made about either alone.
    ThumbnailPump* pump() const { return m_pump; }
    ThumbnailCache* cache() const { return m_cache.get(); }

private:
    /// Both live on the thread that built the provider, which is the UI thread.
    /// Owned here rather than by the engine because the engine deletes the
    /// provider, and one cache serves every pane in the window.
    std::unique_ptr<ThumbnailCache> m_cache;
    ThumbnailPump* m_pump = nullptr;
};

} // namespace mole
