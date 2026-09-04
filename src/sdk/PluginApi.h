#pragma once

#include "sdk/IFeature.h"
#include "sdk/IMetadataReader.h"
#include "sdk/IPreviewProvider.h"
#include "sdk/IThumbnailer.h"
#include "sdk/MenuAction.h"
#include "sdk/PluginServices.h"

#include "core/vfs/IFileSystemFactory.h"

#include <QString>
#include <QtPlugin>

#include <memory>

/// Bumped whenever anything a plugin can see changes shape: PluginMetadata, the
/// interfaces in `sdk/`, PluginServices, or the meaning of any of it.
///
/// **A macro, because the number is pasted into the Qt interface identifier as
/// well as compared as a number.** That is what makes the check work before any
/// plugin code runs: the identifier is in the library's Qt metadata, which
/// QPluginLoader reads without loading the library, so a plugin built against
/// another version is refused before its constructor -- let alone before its
/// `metadata()`. The identifier said `/1.0` while this number went 8, 9, 10, 11,
/// so `qobject_cast<IPlugin*>` accepted every one of them and the only check left
/// was one that had to speak to the plugin to find out whether it could be spoken
/// to. See ADR-0098.
#define MOLE_PLUGIN_API_VERSION 12

#define MOLE_STRINGIFY_INNER(number) #number
#define MOLE_STRINGIFY(number) MOLE_STRINGIFY_INNER(number)

/// The Qt interface identifier, carrying the version above.
#define MOLE_PLUGIN_IID "io.github.ogefest.mole.Plugin/" MOLE_STRINGIFY(MOLE_PLUGIN_API_VERSION)

namespace mole {

inline constexpr int kPluginApiVersion = MOLE_PLUGIN_API_VERSION;

struct PluginMetadata
{
    /// **First, and deliberately.** The host reads this field out of a struct the
    /// plugin returned, so the two have to agree about where it is before they
    /// can agree about anything else. Last -- after five QStrings -- it moved
    /// whenever a field was appended in front of it, and the natural append
    /// (another QString after `description`) would have put an old plugin's
    /// version where the host reads a pointer. First, an append can never move
    /// it, so the version check survives the change that makes it necessary.
    /// See ADR-0098.
    ///
    /// Must equal kPluginApiVersion of the host, or the plugin is rejected.
    int apiVersion = kPluginApiVersion;
    /// Reverse-DNS and unique, e.g. "org.example.gitlab".
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
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

    // Every `add…` below answers false when the contribution was not taken --
    // because it was refused, or because this host has nowhere to put that kind
    // of thing. `mole-tasks` wires only the drives, for instance.
    //
    // **A contribution that was not taken has been destroyed.** The
    // `unique_ptr` was moved into the call and there is nowhere for it to go
    // back to, so a plugin that kept a raw pointer to what it handed over must
    // drop that pointer when the call returns false. See MOLE-365.

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

    /// Contribute a maker of small pictures of files. Unlike a metadata reader,
    /// only the highest-priority one that claims a file is asked: a file has one
    /// picture. See docs/adr/0058-a-file-can-say-what-it-looks-like.md.
    virtual bool addThumbnailer(std::unique_ptr<IThumbnailer> thumbnailer) = 0;

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
    ///
    /// Called on every plugin, in reverse load order, when the host lets go of
    /// them -- a plugin loaded from a shared library as much as one compiled in.
    /// It was the built-ins only until MOLE-365, so a plugin holding connection
    /// pools or a global library context was never told.
    ///
    /// May throw; the host catches it and logs. Nothing else is protected by
    /// then, and taking the process down on the way out is not an improvement.
    virtual void shutdown() { }
};

} // namespace mole

Q_DECLARE_INTERFACE(mole::IPlugin, MOLE_PLUGIN_IID)
