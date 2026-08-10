#include "tools/tasks/ToolEnvironment.h"

#include "host/PluginManager.h"

#include "core/credentials/SecretStore.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/RemoteRegistry.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QRegularExpression>

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

    /// The uri scheme a drive is addressed by, derived from its name exactly as
    /// the application derives it -- so the same drive is the same uri in both.
    QString schemeForName(const QString& name)
    {
        QString slug = name.toLower();
        slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QString());
        return slug.isEmpty() ? QStringLiteral("drive") : slug;
    }

} // namespace

ToolEnvironment::ToolEnvironment()
    : m_tasks(std::make_unique<TaskManager>())
    , m_events(std::make_unique<EventBus>())
{
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

    PluginServices services;
    services.vfs = &m_vfs;
    services.tasks = m_tasks.get();
    services.events = m_events.get();
    // No previews, no features, no scheduler: this binary runs tasks. A plugin
    // that offers one is not rejected, it simply has nowhere to put it, and
    // says so in the errors list.

    PluginManager::Destinations destinations;
    destinations.vfs = &m_vfs;

    m_plugins = std::make_unique<PluginManager>(services, destinations);
    m_plugins->loadFromDefaultPaths();
}

QStringList ToolEnvironment::pluginErrors() const
{
    return m_plugins ? m_plugins->errors() : QStringList {};
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

bool ToolEnvironment::mountBuilt(const QString& name, const QString& scheme, const QVariantMap& config,
    const QString& root, QString* errorOut)
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
    settings.insert(QStringLiteral("__scheme"), schemeForName(name));

    QString error;
    FileSystemPtr fs = factory->create(settings, &error);
    if (!fs) {
        if (errorOut)
            *errorOut = error.isEmpty() ? QStringLiteral("Could not build the drive") : error;
        return false;
    }

    Mount mount;
    mount.displayName = name;
    mount.root = VfsUri(schemeForName(name), name, root.isEmpty() ? QStringLiteral("/") : root);
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

    return mountBuilt(drive.name, drive.factoryScheme, config, drive.root, errorOut);
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
    return mountBuilt(spec.name, spec.type, spec.config, spec.root, errorOut);
}

QStringList ToolEnvironment::mountSummary() const
{
    QStringList lines;
    for (const Mount& mount : m_vfs.mounts())
        lines.append(QStringLiteral("%1  %2").arg(mount.root.toString(), mount.displayName));
    return lines;
}

} // namespace mole::tools
