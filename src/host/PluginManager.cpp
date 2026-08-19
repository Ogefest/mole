#include "host/PluginManager.h"

#include "host/ActionRegistry.h"
#include "host/FeatureRegistry.h"
#include "host/MetadataRegistry.h"
#include "host/PreviewRegistry.h"
#include "host/ThumbnailRegistry.h"

#include "core/vfs/VfsManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibrary>
#include <QPluginLoader>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace mole {
namespace {

    /// The concrete PluginRegistry handed to one plugin during registration.
    /// Scoping it per plugin lets errors name the culprit.
    class ScopedRegistry final : public PluginRegistry
    {
    public:
        ScopedRegistry(const PluginServices& services, PluginManager::Destinations destinations,
            QString pluginId, QStringList* errors)
            : m_services(services)
            , m_destinations(destinations)
            , m_pluginId(std::move(pluginId))
            , m_errors(errors)
        {
        }

        bool addFileSystemFactory(std::unique_ptr<IFileSystemFactory> factory) override
        {
            if (!factory || !m_destinations.vfs) {
                reportError(QStringLiteral("rejected a null filesystem factory"));
                return false;
            }
            const QString scheme = factory->scheme();
            if (scheme.isEmpty()) {
                reportError(QStringLiteral("filesystem factory has no scheme"));
                return false;
            }
            if (m_destinations.vfs->factoryFor(scheme)) {
                reportError(QStringLiteral("scheme '%1' is already provided by another plugin").arg(scheme));
                return false;
            }
            m_destinations.vfs->registerFactory(std::move(factory));
            return true;
        }

        bool addFeature(std::unique_ptr<IFeature> feature) override
        {
            if (!feature || !m_destinations.features) {
                reportError(QStringLiteral("rejected a null feature"));
                return false;
            }
            const QString id = feature->id();
            if (!m_destinations.features->registerFeature(std::move(feature))) {
                reportError(QStringLiteral("feature id '%1' is already taken").arg(id));
                return false;
            }
            return true;
        }

        bool addPreviewProvider(std::unique_ptr<IPreviewProvider> provider) override
        {
            if (!provider || !m_destinations.previews) {
                reportError(QStringLiteral("rejected a null preview provider"));
                return false;
            }
            const QString id = provider->id();
            if (!m_destinations.previews->addProvider(std::move(provider))) {
                reportError(QStringLiteral("preview provider id '%1' is already taken").arg(id));
                return false;
            }
            return true;
        }

        bool addMetadataReader(std::unique_ptr<IMetadataReader> reader) override
        {
            if (!reader || !m_destinations.metadata) {
                reportError(QStringLiteral("rejected a null metadata reader"));
                return false;
            }
            const QString id = reader->id();
            if (!m_destinations.metadata->addReader(std::move(reader))) {
                reportError(QStringLiteral("metadata reader id '%1' is already taken").arg(id));
                return false;
            }
            return true;
        }

        bool addThumbnailer(std::unique_ptr<IThumbnailer> thumbnailer) override
        {
            if (!thumbnailer || !m_destinations.thumbnails) {
                reportError(QStringLiteral("rejected a null thumbnailer"));
                return false;
            }
            const QString id = thumbnailer->id();
            if (!m_destinations.thumbnails->addThumbnailer(std::move(thumbnailer))) {
                reportError(QStringLiteral("thumbnailer id '%1' is already taken").arg(id));
                return false;
            }
            return true;
        }

        bool addMenuAction(MenuAction action) override
        {
            if (!m_destinations.actions) {
                reportError(QStringLiteral("menu actions are not available"));
                return false;
            }
            const QString id = action.id;
            if (!m_destinations.actions->addAction(std::move(action))) {
                reportError(QStringLiteral("menu action '%1' was rejected (duplicate id, or "
                                           "missing title or handler)")
                                .arg(id));
                return false;
            }
            return true;
        }

        const PluginServices& services() const override { return m_services; }

        void reportError(const QString& message) override
        {
            if (m_errors)
                m_errors->append(QStringLiteral("[%1] %2").arg(m_pluginId, message));
        }

    private:
        PluginServices m_services;
        PluginManager::Destinations m_destinations;
        QString m_pluginId;
        QStringList* m_errors = nullptr;
    };

} // namespace

PluginManager::PluginManager(PluginServices services, Destinations destinations, QObject* parent)
    : QObject(parent)
    , m_services(services)
    , m_destinations(destinations)
{
}

PluginManager::~PluginManager()
{
    for (const auto& plugin : m_ownedPlugins)
        plugin->shutdown();
}

bool PluginManager::acceptPlugin(IPlugin* plugin, const QString& filePath, bool builtIn)
{
    if (!plugin)
        return false;

    const PluginMetadata metadata = plugin->metadata();

    if (metadata.id.isEmpty()) {
        m_errors.append(QStringLiteral("%1: plugin has no id")
                            .arg(filePath.isEmpty() ? QStringLiteral("<built-in>") : filePath));
        return false;
    }

    // Refusing a mismatched API version turns a future crash deep inside a
    // vtable into one clear line at startup.
    if (metadata.apiVersion != kPluginApiVersion) {
        m_errors.append(QStringLiteral("%1: built against plugin API %2, host provides %3")
                            .arg(metadata.id)
                            .arg(metadata.apiVersion)
                            .arg(kPluginApiVersion));
        return false;
    }

    for (const LoadedPlugin& existing : m_loaded) {
        if (existing.metadata.id == metadata.id) {
            m_errors.append(QStringLiteral("%1: a plugin with this id is already loaded").arg(metadata.id));
            return false;
        }
    }

    ScopedRegistry registry(m_services, m_destinations, metadata.id, &m_errors);
    plugin->registerExtensions(registry);

    m_loaded.push_back(LoadedPlugin { metadata, filePath, builtIn });
    return true;
}

bool PluginManager::addBuiltIn(std::unique_ptr<IPlugin> plugin)
{
    IPlugin* raw = plugin.get();
    if (!acceptPlugin(raw, {}, true))
        return false;
    m_ownedPlugins.push_back(std::move(plugin));
    return true;
}

int PluginManager::loadFromDirectory(const QString& directory)
{
    QDir dir(directory);
    if (!dir.exists())
        return 0;

    int loaded = 0;
    const QStringList entries = dir.entryList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QString& entry : entries) {
        const QString path = dir.absoluteFilePath(entry);
        if (!QLibrary::isLibrary(path))
            continue;

        QPluginLoader loader(path);
        QObject* instance = loader.instance();
        if (!instance) {
            m_errors.append(QStringLiteral("%1: %2").arg(path, loader.errorString()));
            continue;
        }

        auto* plugin = qobject_cast<IPlugin*>(instance);
        if (!plugin) {
            m_errors.append(QStringLiteral("%1: does not implement the Mole plugin "
                                           "interface")
                                .arg(path));
            loader.unload();
            continue;
        }

        if (acceptPlugin(plugin, path, false))
            ++loaded;
        else
            loader.unload();
    }
    return loaded;
}

QStringList PluginManager::defaultSearchPaths()
{
    QStringList paths;

    const QDir appDir(QCoreApplication::applicationDirPath());

    // Next to the executable, so a build tree works without installing.
    paths.append(appDir.filePath(QStringLiteral("plugins")));

    // Installed layout: <prefix>/bin/mole finds
    // <prefix>/lib/mole/plugins. Resolved relative to the binary
    // rather than baked in, so the same build works from any prefix and from
    // inside an AppImage.
    paths.append(QDir::cleanPath(appDir.absoluteFilePath(QStringLiteral("../lib/mole/plugins"))));

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!dataDir.isEmpty())
        paths.append(QDir(dataDir).filePath(QStringLiteral("plugins")));

    // An escape hatch for development and for testing an unreleased plugin.
    const QString fromEnv
        = QProcessEnvironment::systemEnvironment().value(QStringLiteral("MOLE_PLUGIN_PATH"));
    if (!fromEnv.isEmpty())
        paths.append(fromEnv.split(QDir::listSeparator(), Qt::SkipEmptyParts));

    paths.removeDuplicates();
    return paths;
}

int PluginManager::loadFromDefaultPaths()
{
    int loaded = 0;
    for (const QString& path : defaultSearchPaths())
        loaded += loadFromDirectory(path);
    return loaded;
}

} // namespace mole
