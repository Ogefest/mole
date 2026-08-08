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
