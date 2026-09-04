#pragma once

#include "sdk/PluginApi.h"

#include <QObject>
#include <QStringList>

#include <memory>
#include <vector>

class QPluginLoader;

namespace mole {

class FeatureRegistry;
class PreviewRegistry;
class MetadataRegistry;
class ThumbnailRegistry;
class ActionRegistry;
class ArchiveRegistry;

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
        /// The plugin itself, so shutdown() can be called on it.
        ///
        /// Never owned by this: a built-in belongs to m_ownedPlugins and one from
        /// disk to its QPluginLoader. It is kept because `shutdown()` was called
        /// on built-ins only -- the destructor iterated m_ownedPlugins, which only
        /// addBuiltIn() ever writes -- so the network plugin's libcurl handle
        /// pools and the global Samba context were never told to let go, against
        /// what PluginApi.h promises. See MOLE-365.
        IPlugin* plugin = nullptr;
    };

    struct Destinations
    {
        VfsManager* vfs = nullptr;
        FeatureRegistry* features = nullptr;
        PreviewRegistry* previews = nullptr;
        MetadataRegistry* metadata = nullptr;
        ThumbnailRegistry* thumbnails = nullptr;
        ActionRegistry* actions = nullptr;
        /// Null in a host that has no compress dialog -- `mole-tasks` wires none
        /// -- and an archiver offered to it is reported as having nowhere to go
        /// rather than as a failure.
        ArchiveRegistry* archives = nullptr;
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
    /// Things worth saying that are nobody's fault.
    ///
    /// A host with no registry for a kind of contribution -- `mole-tasks` wires
    /// only the drives -- is not a plugin doing something wrong, and it used to
    /// share a message with a plugin passing a null pointer. So `mole-tasks`
    /// printed "rejected a null feature" once per feature, preview, reader and
    /// thumbnailer of every plugin it loaded, a list of faults the plugins did not
    /// have, and printed it when a `--drive` failed for an unrelated reason. See
    /// MOLE-365.
    const QStringList& notes() const { return m_notes; }

private:
    bool acceptPlugin(IPlugin* plugin, const QString& filePath, bool builtIn);

    PluginServices m_services;
    Destinations m_destinations;
    std::vector<std::unique_ptr<IPlugin>> m_ownedPlugins;
    /// The loaders for the plugins that came from disk, kept as children so a
    /// library outlives everything it contributed.
    ///
    /// **Never unloaded.** A registered filesystem factory, preview provider or
    /// metadata reader lives in a registry the host owns, and those registries
    /// outlive this object -- so unloading the code behind them would be a
    /// use-after-free at exit. QPluginLoader's destructor does not unload either,
    /// which is why this worked when the loader was a local variable. Kept
    /// anyway, because "nothing can unload a plugin" should be a decision that is
    /// written down rather than a side effect of where a variable lived.
    std::vector<QPluginLoader*> m_loaders;
    std::vector<LoadedPlugin> m_loaded;
    QStringList m_errors;
    QStringList m_notes;
};

} // namespace mole
