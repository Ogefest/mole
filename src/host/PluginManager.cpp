#include "host/PluginManager.h"

#include "host/ActionRegistry.h"
#include "host/FeatureRegistry.h"
#include "host/MetadataRegistry.h"
#include "host/PreviewRegistry.h"
#include "host/ThumbnailRegistry.h"

#include "core/vfs/VfsManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonObject>
#include <QLibrary>
#include <QPluginLoader>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <exception>

namespace mole {
namespace {

    /// What a library says it was built against, read from its Qt metadata --
    /// which QPluginLoader has without loading the library, so nothing in the
    /// plugin has run when this answers.
    ///
    /// Empty when the identifier is not a Mole one at all. The version is the
    /// part after the last `/`, which is where MOLE_PLUGIN_IID puts it.
    QString moleApiVersionOf(const QString& iid)
    {
        const QString prefix = QStringLiteral("io.github.ogefest.mole.Plugin/");
        if (!iid.startsWith(prefix))
            return {};
        return iid.mid(prefix.size());
    }

    /// The concrete PluginRegistry handed to one plugin during registration.
    /// Scoping it per plugin lets errors name the culprit.
    class ScopedRegistry final : public PluginRegistry
    {
    public:
        ScopedRegistry(const PluginServices& services, PluginManager::Destinations destinations,
            QString pluginId, QStringList* errors, QStringList* notes)
            : m_services(services)
            , m_destinations(destinations)
            , m_pluginId(std::move(pluginId))
            , m_errors(errors)
            , m_notes(notes)
        {
        }

        bool addFileSystemFactory(std::unique_ptr<IFileSystemFactory> factory) override
        {
            if (!factory) {
                reportError(QStringLiteral("rejected a null filesystem factory"));
                return false;
            }
            if (!m_destinations.vfs)
                return nowhereToPutIt(QStringLiteral("filesystem factory"));
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
            if (!feature) {
                reportError(QStringLiteral("rejected a null feature"));
                return false;
            }
            if (!m_destinations.features)
                return nowhereToPutIt(QStringLiteral("feature"));
            const QString id = feature->id();
            if (!m_destinations.features->registerFeature(std::move(feature))) {
                reportError(QStringLiteral("feature id '%1' is already taken").arg(id));
                return false;
            }
            return true;
        }

        bool addPreviewProvider(std::unique_ptr<IPreviewProvider> provider) override
        {
            if (!provider) {
                reportError(QStringLiteral("rejected a null preview provider"));
                return false;
            }
            if (!m_destinations.previews)
                return nowhereToPutIt(QStringLiteral("preview provider"));
            const QString id = provider->id();
            if (!m_destinations.previews->addProvider(std::move(provider))) {
                reportError(QStringLiteral("preview provider id '%1' is already taken").arg(id));
                return false;
            }
            return true;
        }

        bool addMetadataReader(std::unique_ptr<IMetadataReader> reader) override
        {
            if (!reader) {
                reportError(QStringLiteral("rejected a null metadata reader"));
                return false;
            }
            if (!m_destinations.metadata)
                return nowhereToPutIt(QStringLiteral("metadata reader"));
            const QString id = reader->id();
            if (!m_destinations.metadata->addReader(std::move(reader))) {
                reportError(QStringLiteral("metadata reader id '%1' is already taken").arg(id));
                return false;
            }
            return true;
        }

        bool addThumbnailer(std::unique_ptr<IThumbnailer> thumbnailer) override
        {
            if (!thumbnailer) {
                reportError(QStringLiteral("rejected a null thumbnailer"));
                return false;
            }
            if (!m_destinations.thumbnails)
                return nowhereToPutIt(QStringLiteral("thumbnailer"));
            const QString id = thumbnailer->id();
            if (!m_destinations.thumbnails->addThumbnailer(std::move(thumbnailer))) {
                reportError(QStringLiteral("thumbnailer id '%1' is already taken").arg(id));
                return false;
            }
            return true;
        }

        bool addMenuAction(MenuAction action) override
        {
            if (!m_destinations.actions)
                return nowhereToPutIt(QStringLiteral("menu action"));
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
        /// This host has nowhere to put that kind of contribution, which is not
        /// the plugin's fault and is worth saying once rather than as an error.
        ///
        /// `mole-tasks` wires only the drives, so it used to print "rejected a
        /// null feature" for every feature, preview, reader and thumbnailer of
        /// every plugin it loaded -- a list of faults the plugins did not have --
        /// and print it when a `--drive` failed for an unrelated reason.
        ///
        /// False is still returned, because the contribution really was not
        /// taken: the unique_ptr was moved into the parameter and is destroyed
        /// here, so a plugin that kept a raw pointer to it must drop that pointer
        /// when this returns false. PluginApi.h says so.
        bool nowhereToPutIt(const QString& what)
        {
            if (m_notes) {
                m_notes->append(QStringLiteral("[%1] this host has nowhere to put a %2, so it was "
                                               "not taken")
                                    .arg(m_pluginId, what));
            }
            return false;
        }

        PluginServices m_services;
        PluginManager::Destinations m_destinations;
        QString m_pluginId;
        QStringList* m_errors = nullptr;
        QStringList* m_notes = nullptr;
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
    // Every plugin, in reverse load order, and not only the built-ins. This used
    // to iterate m_ownedPlugins, which only addBuiltIn() writes -- so a plugin
    // loaded from disk was never told to let go of anything, and PluginApi.h
    // promises "called before unload. Release anything the host cannot reclaim
    // itself." The network plugin keeps libcurl handle pools and SMB serialises
    // behind a global Samba context (ADR-0048); neither ever heard. See MOLE-365.
    //
    // Reverse order because a plugin registered later may be using something an
    // earlier one contributed, and shutdown is the mirror of registration.
    //
    // shutdown() is plugin code, so it can throw like any other. A plugin that
    // falls over on the way out must not take the process with it -- there is
    // nothing left to protect by then.
    for (auto it = m_loaded.rbegin(); it != m_loaded.rend(); ++it) {
        if (!it->plugin)
            continue;
        try {
            it->plugin->shutdown();
        } catch (const std::exception& problem) {
            qWarning("%s threw on shutdown: %s", qPrintable(it->metadata.id), problem.what());
        } catch (...) {
            qWarning("%s threw on shutdown", qPrintable(it->metadata.id));
        }
    }
}

bool PluginManager::acceptPlugin(IPlugin* plugin, const QString& filePath, bool builtIn)
{
    if (!plugin)
        return false;

    const QString where = filePath.isEmpty() ? QStringLiteral("<built-in>") : filePath;

    // metadata() and registerExtensions() below are plugin code, and nothing here
    // used to catch what plugin code throws -- so an exception went through
    // acceptPlugin, loadFromDirectory, AppController::initialise and main, and
    // ended the process at startup. One third-party plugin with a bug therefore
    // stopped Mole from opening at all. Task::run() and ReadMetadataTask both
    // catch exactly this and turn it into a reported failure; the loader was the
    // one place plugin code runs where nothing did. See MOLE-365.
    PluginMetadata metadata;
    try {
        metadata = plugin->metadata();
    } catch (const std::exception& problem) {
        m_errors.append(QStringLiteral("%1: threw while being asked what it is: %2")
                            .arg(where, QString::fromUtf8(problem.what())));
        return false;
    } catch (...) {
        m_errors.append(QStringLiteral("%1: threw while being asked what it is").arg(where));
        return false;
    }

    if (metadata.id.isEmpty()) {
        m_errors.append(QStringLiteral("%1: plugin has no id").arg(where));
        return false;
    }

    // The second line, and it is a second line rather than the only one: the
    // identifier check in loadFromDirectory() has already refused a library built
    // against another version, before anything in it ran. This one catches a
    // built-in -- which has no identifier to read -- and a plugin whose metadata
    // says something other than what it was compiled with. See ADR-0098.
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

    ScopedRegistry registry(m_services, m_destinations, metadata.id, &m_errors, &m_notes);
    try {
        plugin->registerExtensions(registry);
    } catch (const std::exception& problem) {
        m_errors.append(QStringLiteral("%1: threw during registration: %2")
                            .arg(metadata.id, QString::fromUtf8(problem.what())));
        return false;
    } catch (...) {
        m_errors.append(QStringLiteral("%1: threw during registration").arg(metadata.id));
        return false;
    }

    m_loaded.push_back(LoadedPlugin { metadata, filePath, builtIn, plugin });
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
    // Symbolic links included. QDir::NoSymLinks dropped every one of them before
    // QLibrary::isLibrary was ever asked, so a developer linking a build-tree .so
    // into MOLE_PLUGIN_PATH -- the documented escape hatch -- and a distribution
    // installing a versioned .so with an unversioned link beside it both got no
    // plugin, no line in errors() and nothing under `--plugins`. A link to
    // something that is not a library is still refused by isLibrary(), and a
    // broken one fails to instantiate and says so, so nothing is lost by looking.
    // The lib/lib64 comment below is about the same failure one function up: a
    // plugin that is not found is not a plugin that failed. See MOLE-296 and
    // MOLE-365.
    const QStringList entries = dir.entryList(QDir::Files, QDir::Name);
    for (const QString& entry : entries) {
        const QString path = dir.absoluteFilePath(entry);
        if (!QLibrary::isLibrary(path))
            continue;

        auto* loader = new QPluginLoader(path, this);

        // **What it was built against, before any of its code runs.** The check
        // that decides whether a plugin may be spoken to used to be made by
        // speaking to it: `plugin->metadata()` is a virtual call through the
        // plugin's own vtable, returning a struct by value, so a plugin built
        // against a different API was asked a question in a shape it did not
        // have -- and if IPlugin had ever gained a virtual before `metadata()`,
        // the call itself would have gone somewhere else entirely. The version
        // is in the interface identifier now, and QPluginLoader has the
        // identifier out of the library's Qt metadata without loading it. See
        // ADR-0098 and MOLE-366.
        const QString declaredIid = loader->metaData().value(QStringLiteral("IID")).toString();
        if (declaredIid != QLatin1String(MOLE_PLUGIN_IID)) {
            const QString version = moleApiVersionOf(declaredIid);
            if (version.isEmpty()) {
                m_errors.append(
                    QStringLiteral("%1: does not implement the Mole plugin "
                                   "interface (it declares %2)")
                        .arg(path, declaredIid.isEmpty() ? QStringLiteral("nothing") : declaredIid));
            } else {
                m_errors.append(QStringLiteral("%1: built against plugin API %2, host provides %3")
                                    .arg(path, version)
                                    .arg(kPluginApiVersion));
            }
            delete loader;
            continue;
        }

        QObject* instance = loader->instance();
        if (!instance) {
            m_errors.append(QStringLiteral("%1: %2").arg(path, loader->errorString()));
            delete loader;
            continue;
        }

        auto* plugin = qobject_cast<IPlugin*>(instance);
        if (!plugin) {
            m_errors.append(QStringLiteral("%1: does not implement the Mole plugin "
                                           "interface")
                                .arg(path));
            loader->unload();
            delete loader;
            continue;
        }

        if (acceptPlugin(plugin, path, false)) {
            ++loaded;
            m_loaders.push_back(loader);
        } else {
            // Refused, so nothing it contributed is in a registry and the code
            // behind it is safe to let go of.
            loader->unload();
            delete loader;
        }
    }
    return loaded;
}

QStringList PluginManager::defaultSearchPaths()
{
    QStringList paths;

    const QDir appDir(QCoreApplication::applicationDirPath());

    // Next to the executable, so a build tree works without installing.
    paths.append(appDir.filePath(QStringLiteral("plugins")));

    // Installed layout: <prefix>/bin/mole finds <prefix>/lib/mole/plugins.
    // Resolved relative to the binary rather than baked in, so the same build works
    // from any prefix and from inside an AppImage.
    //
    // Both lib and lib64, and that is not belt-and-braces. GNUInstallDirs gives
    // CMAKE_INSTALL_LIBDIR as `lib64` on every RPM distribution, so the install
    // rules put the plugins in <prefix>/lib64/mole/plugins there -- and with only
    // `lib` searched, the .rpm and the AppImage each shipped an archive plugin and
    // a network plugin that nothing ever looked for. No archive browsing and no
    // sftp, ftp, s3, webdav, smb or nfs drives, reported as nothing at all rather
    // than as a failure, because a plugin that is not found is not a plugin that
    // failed. Found while measuring the artefacts for MOLE-296.
    for (const char* libdir : { "lib", "lib64" }) {
        paths.append(QDir::cleanPath(
            appDir.absoluteFilePath(QStringLiteral("../%1/mole/plugins").arg(QLatin1String(libdir)))));
    }

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
