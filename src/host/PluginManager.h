#pragma once

#include "sdk/PluginApi.h"

#include <QObject>
#include <QStringList>

#include <memory>
#include <vector>

namespace mole {

class FeatureRegistry;
class PreviewRegistry;
class MetadataRegistry;
class ActionRegistry;

/// Loads plugins and wires whatever they contribute into the host.
///
/// Built-in functionality goes through exactly the same path as third-party
/// code: the browser, the searches and the archive support are all plugins
/// registered here. If the API is good enough for them, it is good enough for
/// the community -- and if it is not, we find out immediately rather than
/// after someone tries to write one.
class PluginManager : public QObject
{
    Q_OBJECT

public:
    struct LoadedPlugin
    {
        PluginMetadata metadata;
        QString filePath; ///< empty for statically linked built-ins
        bool builtIn = false;
    };

    struct Destinations
    {
        VfsManager* vfs = nullptr;
        FeatureRegistry* features = nullptr;
        PreviewRegistry* previews = nullptr;
        MetadataRegistry* metadata = nullptr;
        ActionRegistry* actions = nullptr;
    };

    PluginManager(PluginServices services, Destinations destinations, QObject* parent = nullptr);
    ~PluginManager() override;

    /// Registers a plugin compiled into the application. Takes ownership.
    bool addBuiltIn(std::unique_ptr<IPlugin> plugin);

    /// Loads every shared library in `directory` that exports a plugin.
    /// Returns how many loaded successfully. Missing directories are not an
    /// error -- most installations will not have all of them.
    int loadFromDirectory(const QString& directory);

    /// The conventional search path: alongside the executable, in the user's
    /// data directory, and anything in MOLE_PLUGIN_PATH.
    static QStringList defaultSearchPaths();
    int loadFromDefaultPaths();

    const std::vector<LoadedPlugin>& loaded() const { return m_loaded; }
    /// Problems encountered while loading, for display in an about box.
    const QStringList& errors() const { return m_errors; }

private:
    bool acceptPlugin(IPlugin* plugin, const QString& filePath, bool builtIn);

    PluginServices m_services;
    Destinations m_destinations;
    std::vector<std::unique_ptr<IPlugin>> m_ownedPlugins;
    std::vector<LoadedPlugin> m_loaded;
    QStringList m_errors;
};

} // namespace mole
