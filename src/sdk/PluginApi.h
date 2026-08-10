#pragma once

#include "sdk/IFeature.h"
#include "sdk/IMetadataReader.h"
#include "sdk/IPreviewProvider.h"
#include "sdk/MenuAction.h"
#include "sdk/PluginServices.h"

#include "core/vfs/IFileSystemFactory.h"

#include <QString>
#include <QtPlugin>

#include <memory>

namespace mole {

/// Bumped whenever anything a plugin can see changes shape. The host refuses
/// to load a plugin built against a different major version, which turns a
/// mysterious crash into a clear message at startup.
inline constexpr int kPluginApiVersion = 9;

struct PluginMetadata
{
    /// Reverse-DNS and unique, e.g. "org.example.gitlab".
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
    /// Must equal kPluginApiVersion of the host, or the plugin is rejected.
    int apiVersion = kPluginApiVersion;
};

/// Handed to a plugin during registration. Everything a plugin contributes
/// goes through here, and nothing else in the host is reachable -- which is
/// what keeps the API a contract rather than an invitation to poke around.
///
/// Adding a new extension point means adding a method here and bumping
/// kPluginApiVersion.
class PluginRegistry
{
public:
    virtual ~PluginRegistry() = default;

    /// Contribute a new kind of drive: sftp, s3, a git forge, a database.
    /// Its scheme() must be unique across all loaded plugins.
    virtual bool addFileSystemFactory(std::unique_ptr<IFileSystemFactory> factory) = 0;

    /// Contribute a new kind of tab.
    virtual bool addFeature(std::unique_ptr<IFeature> feature) = 0;

    /// Contribute a viewer for some family of file types.
    virtual bool addPreviewProvider(std::unique_ptr<IPreviewProvider> provider) = 0;

    /// Contribute a reader of what a file says about itself. Unlike a preview
    /// provider, every reader that claims a file is used.
    virtual bool addMetadataReader(std::unique_ptr<IMetadataReader> reader) = 0;

    /// Contribute an entry to the application menu. Most plugins want
    /// MenuAction::Section::Workflows.
    virtual bool addMenuAction(MenuAction action) = 0;

    /// The host services this plugin may use. Valid for the plugin's lifetime.
    virtual const PluginServices& services() const = 0;

    /// Records a problem with this plugin without aborting the whole load.
    virtual void reportError(const QString& message) = 0;
};

/// What a shared library must implement to be a Mole plugin.
///
/// Minimal example:
///
///     class GitLabPlugin : public QObject, public mole::IPlugin {
///         Q_OBJECT
///         Q_PLUGIN_METADATA(IID MOLE_PLUGIN_IID)
///         Q_INTERFACES(mole::IPlugin)
///     public:
///         mole::PluginMetadata metadata() const override { return {...}; }
///         void registerExtensions(mole::PluginRegistry& registry) override {
///             registry.addFileSystemFactory(std::make_unique<GitLabFsFactory>());
///             registry.addFeature(std::make_unique<BranchBrowserFeature>());
///         }
///     };
class IPlugin
{
public:
    virtual ~IPlugin() = default;

    virtual PluginMetadata metadata() const = 0;

    /// Called once at load time. Do registration only -- no I/O, no threads,
    /// no dialogs. Anything expensive belongs in a Task started later.
    virtual void registerExtensions(PluginRegistry& registry) = 0;

    /// Called before unload. Release anything the host cannot reclaim itself.
    virtual void shutdown() { }
};

} // namespace mole

#define MOLE_PLUGIN_IID "io.github.ogefest.mole.Plugin/1.0"

Q_DECLARE_INTERFACE(mole::IPlugin, MOLE_PLUGIN_IID)
