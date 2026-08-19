#include "host/ThumbnailRegistry.h"

#include <algorithm>

namespace mole {

ThumbnailRegistry::ThumbnailRegistry(QObject* parent)
    : QObject(parent)
{
}

ThumbnailRegistry::~ThumbnailRegistry() = default;

bool ThumbnailRegistry::addThumbnailer(std::unique_ptr<IThumbnailer> thumbnailer)
{
    if (!thumbnailer || thumbnailer->id().isEmpty())
        return false;
    if (this->thumbnailer(thumbnailer->id()))
        return false;

    m_thumbnailers.push_back(std::move(thumbnailer));
    // Stable, so two thumbnailers of equal priority stay in registration order
    // and which one answers does not depend on the sort.
    std::stable_sort(m_thumbnailers.begin(), m_thumbnailers.end(),
        [](const std::unique_ptr<IThumbnailer>& a, const std::unique_ptr<IThumbnailer>& b) {
            return a->priority() > b->priority();
        });
    return true;
}

IThumbnailer* ThumbnailRegistry::thumbnailerFor(const FileEntry& entry) const
{
    if (entry.isDir)
        return nullptr;
    for (const auto& thumbnailer : m_thumbnailers) {
        if (thumbnailer->canThumbnail(entry))
            return thumbnailer.get();
    }
    return nullptr;
}

IThumbnailer* ThumbnailRegistry::thumbnailer(const QString& id) const
{
    for (const auto& thumbnailer : m_thumbnailers) {
        if (thumbnailer->id() == id)
            return thumbnailer.get();
    }
    return nullptr;
}

QList<IThumbnailer*> ThumbnailRegistry::thumbnailers() const
{
    QList<IThumbnailer*> out;
    out.reserve(static_cast<qsizetype>(m_thumbnailers.size()));
    for (const auto& thumbnailer : m_thumbnailers)
        out.append(thumbnailer.get());
    return out;
}

} // namespace mole
