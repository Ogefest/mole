#pragma once

#include "sdk/IPreviewProvider.h"

#include <QObject>

#include <memory>
#include <vector>

namespace mole {

/// Picks the viewer for a file. Providers are consulted highest priority
/// first, so a plugin can override a built-in viewer just by outranking it.
class PreviewRegistry : public QObject, public IPreviewLookup
{
    Q_OBJECT

public:
    explicit PreviewRegistry(QObject* parent = nullptr);
    ~PreviewRegistry() override;

    bool addProvider(std::unique_ptr<IPreviewProvider> provider);

    /// Best provider for `entry`, or nullptr when nothing can render it.
    IPreviewProvider* providerFor(const FileEntry& entry) const override;
    IPreviewProvider* providerBelow(const FileEntry& entry, const IPreviewProvider* above) const override;
    IPreviewProvider* provider(const QString& id) const;
    QList<IPreviewProvider*> providers() const;

private:
    /// Kept sorted by descending priority so lookup is a linear scan that
    /// stops at the first match.
    std::vector<std::unique_ptr<IPreviewProvider>> m_providers;
};

} // namespace mole
