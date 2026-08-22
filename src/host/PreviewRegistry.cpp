#include "host/PreviewRegistry.h"

#include <algorithm>

namespace mole {

PreviewRegistry::PreviewRegistry(QObject* parent)
    : QObject(parent)
{
}

PreviewRegistry::~PreviewRegistry() = default;

bool PreviewRegistry::addProvider(std::unique_ptr<IPreviewProvider> provider)
{
    if (!provider || provider->id().isEmpty())
        return false;
    if (this->provider(provider->id()))
        return false;

    m_providers.push_back(std::move(provider));
    std::stable_sort(m_providers.begin(), m_providers.end(),
        [](const std::unique_ptr<IPreviewProvider>& a, const std::unique_ptr<IPreviewProvider>& b) {
            return a->priority() > b->priority();
        });
    return true;
}

IPreviewProvider* PreviewRegistry::providerFor(const FileEntry& entry) const
{
    if (entry.isDir)
        return nullptr;
    for (const auto& provider : m_providers) {
        if (provider->canPreview(entry))
            return provider.get();
    }
    return nullptr;
}

IPreviewProvider* PreviewRegistry::providerBelow(const FileEntry& entry, const IPreviewProvider* above) const
{
    if (entry.isDir)
        return nullptr;

    // Position in the sorted list rather than a comparison of priorities. Two
    // providers can share a priority -- the database, Parquet and video viewers
    // all sit at 60 -- and "below" has to mean one step, not a whole tier, or a
    // decline would skip a viewer that might have shown the file.
    bool passed = above == nullptr;
    for (const auto& provider : m_providers) {
        if (!passed) {
            passed = provider.get() == above;
            continue;
        }
        if (provider->canPreview(entry))
            return provider.get();
    }
    // Nothing below it, or `above` is not one of ours -- in which case there is
    // no position to step down from and refusing is the honest answer.
    return nullptr;
}

IPreviewProvider* PreviewRegistry::provider(const QString& id) const
{
    for (const auto& provider : m_providers) {
        if (provider->id() == id)
            return provider.get();
    }
    return nullptr;
}

QList<IPreviewProvider*> PreviewRegistry::providers() const
{
    QList<IPreviewProvider*> out;
    out.reserve(static_cast<qsizetype>(m_providers.size()));
    for (const auto& provider : m_providers)
        out.append(provider.get());
    return out;
}

} // namespace mole
