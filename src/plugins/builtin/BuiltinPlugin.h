#pragma once

#include "sdk/PluginApi.h"

#include <memory>

namespace mole {

class AnalysisJob;
class IndexScanJob;

/// Registers everything that ships in the box.
///
/// It goes through PluginRegistry exactly like a third-party plugin would.
/// Keeping the built-ins on the public path is the cheapest way to guarantee
/// the API stays capable enough to build a real feature with -- if the browser
/// itself can be written against it, so can anything else.
class BuiltinPlugin final : public IPlugin
{
public:
    /// `defaultUri` is where a fresh browser tab starts.
    explicit BuiltinPlugin(QString defaultUri);
    ~BuiltinPlugin() override;

    PluginMetadata metadata() const override;
    void registerExtensions(PluginRegistry& registry) override;

private:
    QString m_defaultUri;
    /// Outlives every tab: a scheduled report must run whether or not the user
    /// has an analysis tab open.
    std::unique_ptr<AnalysisJob> m_analysisJob;
    std::unique_ptr<IndexScanJob> m_indexJob;
};

} // namespace mole
