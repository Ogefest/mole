#pragma once

#include "sdk/PluginServices.h"

#include "core/vfs/VfsManager.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>

namespace mole {

class TaskManager;
class IndexDatabase;
class EventBus;
class SecretStore;
class RemoteRegistry;
class PluginManager;

namespace tools {

    /// A drive described entirely on the command line, as `--mount` gives it:
    ///
    ///     name=nas,type=sftp,host=…,user=…,password=@SFTP_PASSWORD,root=/data
    ///
    /// `name` and `type` are required, `root` is optional, and everything else
    /// goes to the factory as it stands.
    struct MountSpec
    {
        QString name;
        QString type;
        QString root;
        QVariantMap config;
        /// Why this spec is unusable, or empty when it is fine.
        QString problem;

        bool isValid() const { return problem.isEmpty(); }
    };

    /// Reads one. A value written `@SOMETHING` is taken from that environment
    /// variable, which is the only way to give this tool a password without
    /// putting it in an argument list every other process can read, and in a
    /// shell history that outlives the run.
    ///
    /// A free function so the parsing can be tested without a drive anywhere.
    MountSpec parseMountSpec(const QString& text);

    /// A secret named by the environment: `@SOMETHING` is the value of that
    /// variable, and nothing else is accepted.
    ///
    /// ADR-0028 says it in as many words -- "Secrets never appear in an
    /// argument" -- and an argument is in `ps` for every user on the machine, in
    /// the shell history that outlives the run, and in any CI log that echoes
    /// the command it is about to run. So a literal is refused rather than used.
    ///
    /// Empty with `problemOut` set for a literal, for a bare `@`, and for a
    /// variable that is not set: an unset variable quietly meaning "no
    /// passphrase" would write an unencrypted archive where one was asked for.
    QString secretFromEnvironment(const QString& raw, QString* problemOut);

    /// Everything `mole-tasks` needs to reach a drive, with no window anywhere.
    ///
    /// The console binary exists for the half of the work that a C++ test cannot
    /// do: reproducing a fault by hand against a real server, driving a transfer
    /// under `tc netem`, and running the scale tier on a machine with no display.
    /// So it reaches drives exactly as the application does -- the same
    /// factories, the same plugins, the same configuration file, the same
    /// credential store -- because a runner that connected differently would
    /// reproduce a different fault.
    ///
    /// Local disk and the in-memory scratch drive are mounted from the start, so
    /// `file:///tmp/x` and `mem:///x` need no configuration at all. Everything
    /// else is mounted on request, from the store (`--drive`) or from the
    /// command line (`--mount`).
    class ToolEnvironment
    {
    public:
        ToolEnvironment();
        ~ToolEnvironment();

        ToolEnvironment(const ToolEnvironment&) = delete;
        ToolEnvironment& operator=(const ToolEnvironment&) = delete;

        /// Loads the shared-library backends: sftp, ftp, s3, webdav, archives.
        /// A missing one is not an error -- the tool still copies local files --
        /// but it is reported, because "no such drive" is a poor way to find out
        /// that a plugin did not load.
        void loadPlugins();
        QStringList pluginErrors() const;
        /// Where plugins were looked for and what was found, so `drives
        /// --plugins` can answer the first question of any report about a
        /// package: was the network backend there at all. Empty until
        /// loadPlugins() has run.
        QStringList pluginSearchPaths() const;
        QStringList loadedPlugins() const;

        VfsManager& drives() { return m_vfs; }
        TaskManager& tasks() { return *m_tasks; }
        /// The services the plugins are given, so a command can build the same
        /// scan the window builds rather than a poorer one of its own. Valid
        /// whether or not loadPlugins() has run. See ADR-0056.
        PluginServices services() const { return m_services; }
        /// Opened on first use: scanning is the only command that needs it, and
        /// creating the file for a copy would be rude.
        IndexDatabase* index(QString* errorOut);

        /// Mounts a drive out of the configuration the application uses, by id
        /// or by name. The secrets come from the credential store, which is
        /// unlocked with the passphrase in `MOLE_PASSPHRASE` -- never from an
        /// argument, which every other process on the machine can read.
        bool mountConfigured(const QString& idOrName, QString* errorOut);

        /// Mounts a drive described entirely on the command line:
        ///
        ///     name=nas,type=sftp,host=…,user=…,password=@SFTP_PASSWORD,root=/data
        ///
        /// `name` and `type` are required; `root` is optional; everything else
        /// goes to the factory as it stands. A value written `@SOMETHING` is
        /// read from that environment variable, so a password never appears in
        /// an argument list or in a shell history.
        bool mountFromSpec(const QString& text, QString* errorOut);

        /// What each mounted drive is called and how to address it, which is
        /// what a caller has to know to write the next argument.
        QStringList mountSummary() const;

    private:
        /// Builds the backend and mounts it at `root`.
        ///
        /// `root` is the *mount* root, which is not the drive's root inside the
        /// remote: the remote root travels in the configuration as `__root` and
        /// every network backend prefixes its own paths with it. Applying it in
        /// both places is what made `--drive nas` reach /data/data/photos, and
        /// only for a drive rooted below `/`, which is why the local and mem
        /// cases never showed it. See MOLE-391.
        bool mountBuilt(const QString& id, const QString& name, const QString& scheme,
            const QVariantMap& config, const VfsUri& root, QString* errorOut);

        PluginServices m_services;
        VfsManager m_vfs;
        std::unique_ptr<TaskManager> m_tasks;
        std::unique_ptr<EventBus> m_events;
        std::unique_ptr<IndexDatabase> m_index;
        std::unique_ptr<SecretStore> m_secrets;
        std::unique_ptr<RemoteRegistry> m_remotes;
        std::unique_ptr<PluginManager> m_plugins;
    };

} // namespace tools
} // namespace mole
