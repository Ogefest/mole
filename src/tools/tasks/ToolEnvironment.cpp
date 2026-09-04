#include "tools/tasks/ToolEnvironment.h"

#include "host/PluginManager.h"

#include "core/credentials/SecretStore.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/RemoteRegistry.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

namespace mole::tools {
namespace {

    /// A value written `@SOMETHING` comes from that environment variable.
    ///
    /// It is the only way to give this tool a password without putting it in an
    /// argument list, which every other process on the machine can read, and in
    /// a shell history, which outlives the run.
    QString resolveValue(const QString& raw)
    {
        if (!raw.startsWith(QLatin1Char('@')))
            return raw;
        return QString::fromLocal8Bit(qgetenv(raw.mid(1).toLocal8Bit().constData()));
    }

} // namespace

QString secretFromEnvironment(const QString& raw, QString* problemOut)
{
    const auto refuse = [problemOut](const QString& why) {
        if (problemOut)
            *problemOut = why;
        return QString();
    };

    if (!raw.startsWith(QLatin1Char('@'))) {
        return refuse(QStringLiteral("a secret has to be named, not typed: write @NAME and put the "
                                     "value in that environment variable. An argument is visible in "
                                     "ps, in the shell history and in any log that echoes the "
                                     "command."));
    }
    const QString name = raw.mid(1);
    if (name.isEmpty())
        return refuse(QStringLiteral("@ needs the name of an environment variable after it"));

    const QString value = QString::fromLocal8Bit(qgetenv(name.toLocal8Bit().constData()));
    if (value.isEmpty())
        return refuse(QStringLiteral("%1 is not set, or is empty").arg(name));
    return value;
}

ToolEnvironment::ToolEnvironment()
    : m_tasks(std::make_unique<TaskManager>())
    , m_events(std::make_unique<EventBus>())
{
    // Built here rather than inside loadPlugins(), because a command that never
    // loads a plugin still has to be able to build the readers a scan needs --
    // containerReaderFor() asks only for the drives. See ADR-0056.
    m_services.vfs = &m_vfs;
    m_services.tasks = m_tasks.get();
    m_services.events = m_events.get();

    m_vfs.registerFactory(std::make_unique<LocalFileSystemFactory>());
    m_vfs.registerFactory(std::make_unique<MemoryFileSystemFactory>());

    // Mounted from the start rather than on request: `file:///tmp/x` is what
    // anybody types first, and asking them to configure the disk they are
    // sitting in front of would be absurd.
    Mount local;
    local.id = QStringLiteral("local");
    local.displayName = QStringLiteral("Local disk");
    local.root = VfsUri(QStringLiteral("file"), QString(), QStringLiteral("/"));
    local.fileSystem = std::make_shared<LocalFileSystem>();
    m_vfs.addMount(std::move(local));

    // The scratch drive costs nothing until something is written to it, and it
    // is what a `--to mem:///` smoke test needs.
    Mount memory;
    memory.id = QStringLiteral("memory");
    memory.displayName = QStringLiteral("In-memory scratch");
    memory.root = VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/"));
    memory.fileSystem = std::make_shared<MemoryFileSystem>();
    m_vfs.addMount(std::move(memory));
}

ToolEnvironment::~ToolEnvironment() = default;

void ToolEnvironment::loadPlugins()
{
    if (m_plugins)
        return;

    // No previews, no features, no scheduler: this binary runs tasks. A plugin
    // that offers one is not rejected, it simply has nowhere to put it, and
    // says so in the errors list.
    PluginManager::Destinations destinations;
    destinations.vfs = &m_vfs;

    m_plugins = std::make_unique<PluginManager>(m_services, destinations);
    m_plugins->loadFromDefaultPaths();
}

QStringList ToolEnvironment::pluginErrors() const
{
    return m_plugins ? m_plugins->errors() : QStringList {};
}

QStringList ToolEnvironment::pluginNotes() const
{
    return m_plugins ? m_plugins->notes() : QStringList {};
}

QStringList ToolEnvironment::pluginSearchPaths() const
{
    return PluginManager::defaultSearchPaths();
}

QStringList ToolEnvironment::loadedPlugins() const
{
    QStringList names;
    if (!m_plugins)
        return names;
    for (const PluginManager::LoadedPlugin& plugin : m_plugins->loaded()) {
        names.append(plugin.filePath.isEmpty()
                ? plugin.metadata.name
                : QStringLiteral("%1  %2").arg(plugin.metadata.name, plugin.filePath));
    }
    return names;
}

IndexDatabase* ToolEnvironment::index(QString* errorOut)
{
    if (m_index)
        return m_index.get();

    m_index = std::make_unique<IndexDatabase>(IndexDatabase::defaultFilePath());
    if (Result<void> opened = m_index->open(); !opened.ok()) {
        if (errorOut)
            *errorOut = opened.error().message;
        m_index.reset();
        return nullptr;
    }
    return m_index.get();
}

bool ToolEnvironment::mountBuilt(const QString& id, const QString& name, const QString& scheme,
    const QVariantMap& config, const VfsUri& root, QString* errorOut)
{
    IFileSystemFactory* factory = m_vfs.factoryFor(scheme);
    if (!factory) {
        if (errorOut) {
            *errorOut
                = QStringLiteral("Nothing here can serve a %1 drive. Is its plugin installed?").arg(scheme);
        }
        return false;
    }

    QVariantMap settings = config;
    // The backend stamps its own uris with this, so the drive answers to the
    // name it was given rather than to its protocol -- two SFTP drives would
    // otherwise both be sftp:// and only one of them reachable.
    settings.insert(QStringLiteral("__scheme"), driveSchemeFor(name));

    QString error;
    FileSystemPtr fs = factory->create(settings, &error);
    if (!fs) {
        if (errorOut)
            *errorOut = error.isEmpty() ? QStringLiteral("Could not build the drive") : error;
        return false;
    }

    Mount mount;
    mount.id = id;
    mount.displayName = name;
    mount.root = root;
    mount.fileSystem = std::move(fs);
    m_vfs.addMount(std::move(mount));
    return true;
}

bool ToolEnvironment::mountConfigured(const QString& idOrName, QString* errorOut)
{
    if (!m_secrets) {
        m_secrets = std::make_unique<SecretStore>(SecretStore::defaultPath());
        m_remotes = std::make_unique<RemoteRegistry>(RemoteRegistry::defaultPath(), m_secrets.get());
        m_remotes->load();
    }

    RemoteDrive drive = m_remotes->drive(idOrName);
    if (!drive.isValid()) {
        for (const RemoteDrive& candidate : m_remotes->drives()) {
            if (candidate.name.compare(idOrName, Qt::CaseInsensitive) == 0) {
                drive = candidate;
                break;
            }
        }
    }
    if (!drive.isValid()) {
        if (errorOut)
            *errorOut = QStringLiteral("No drive called '%1' is configured").arg(idOrName);
        return false;
    }

    // Only when the drive actually needs a secret, so a drive that keeps none
    // works on a machine where nothing was ever unlocked.
    if (m_remotes->needsUnlocking(drive) && !m_secrets->isUnlocked()) {
        const QString passphrase = QString::fromLocal8Bit(qgetenv("MOLE_PASSPHRASE"));
        if (passphrase.isEmpty()) {
            if (errorOut) {
                *errorOut = QStringLiteral("%1 keeps a secret in the credential store, and the store "
                                           "is locked. Put the passphrase in MOLE_PASSPHRASE.")
                                .arg(drive.name);
            }
            return false;
        }
        QString error;
        if (!m_secrets->unlock(passphrase, &error)) {
            if (errorOut)
                *errorOut = error.isEmpty() ? QStringLiteral("The passphrase was refused") : error;
            return false;
        }
    }

    QString error;
    const QVariantMap config = m_remotes->configFor(drive, &error);
    if (config.isEmpty()) {
        if (errorOut)
            *errorOut = error.isEmpty() ? QStringLiteral("This drive has no configuration") : error;
        return false;
    }

    // drive.rootUri() and not drive.root: exactly what AppController::connectDrive()
    // mounts, and the remote root is already in the configuration as __root.
    return mountBuilt(drive.id, drive.name, drive.factoryScheme, config, drive.rootUri(), errorOut);
}

MountSpec parseMountSpec(const QString& text)
{
    MountSpec spec;

    for (const QString& field : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const qsizetype split = field.indexOf(QLatin1Char('='));
        if (split <= 0) {
            spec.problem = QStringLiteral("'%1' is not key=value").arg(field);
            return spec;
        }
        const QString key = field.left(split).trimmed();
        const QString value = resolveValue(field.mid(split + 1).trimmed());

        if (key == QLatin1String("name"))
            spec.name = value;
        else if (key == QLatin1String("type"))
            spec.type = value;
        else if (key == QLatin1String("root"))
            spec.root = value;
        else
            spec.config.insert(key, value);
    }

    if (spec.name.isEmpty() || spec.type.isEmpty())
        spec.problem = QStringLiteral("A mount needs at least name= and type=");
    return spec;
}

bool ToolEnvironment::mountFromSpec(const QString& text, QString* errorOut)
{
    const MountSpec spec = parseMountSpec(text);
    if (!spec.isValid()) {
        if (errorOut)
            *errorOut = spec.problem;
        return false;
    }

    // `root=` means what it means in drives.json: where inside the remote this
    // drive starts. That is the backend's business, so it travels in the
    // configuration under the key every network backend reads -- the same key
    // RemoteRegistry::configFor() puts it under. It used to become the mount
    // root instead, so "root" meant one thing here and another there.
    QVariantMap config = spec.config;
    if (!spec.root.isEmpty())
        config.insert(QStringLiteral("__root"), spec.root);

    return mountBuilt(spec.name, spec.name, spec.type, config,
        VfsUri(driveSchemeFor(spec.name), spec.name, QStringLiteral("/")), errorOut);
}

QStringList ToolEnvironment::mountSummary() const
{
    QStringList lines;
    for (const Mount& mount : m_vfs.mounts())
        lines.append(QStringLiteral("%1  %2").arg(mount.root.toString(), mount.displayName));
    return lines;
}

} // namespace mole::tools
