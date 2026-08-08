#include "ui/AppController.h"

#include "host/ActionRegistry.h"
#include "host/FeatureRegistry.h"
#include "host/PluginManager.h"
#include "host/PreviewRegistry.h"
#include "ui/FileLauncher.h"
#include "ui/SessionStore.h"
#include "ui/models/MountListModel.h"
#include "ui/models/TabsModel.h"
#include "ui/models/TaskListModel.h"

#include "core/alerts/AlertStore.h"
#include "core/analysis/AnalysisStore.h"
#include "core/automation/ScheduleStore.h"
#include "core/credentials/SecretStore.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/ScanTask.h"
#include "core/sets/FileSetStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/IFileSystemFactory.h"
#include "core/vfs/RemoteRegistry.h"
#include "core/vfs/SystemVolumes.h"
#include "core/vfs/VfsManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>

namespace mole {

AppController::AppController(QObject* parent)
    : QObject(parent)
{
}

bool AppController::restoreSession()
{
    if (!m_session)
        return false;

    const Session saved = m_session->load();
    m_window = saved.window;
    if (saved.tabs.isEmpty())
        return false;

    // Opening tabs marks the session dirty; suppress that so a restore cannot
    // overwrite the file it is still reading from.
    m_restoring = true;
    const int restored = m_tabs->restoreSession(saved);
    m_restoring = false;

    return restored > 0;
}

void AppController::saveSessionNow()
{
    if (!m_session || !m_tabs || m_restoring)
        return;

    Session session = m_tabs->captureSession();
    session.window = m_window;
    m_session->save(session);
}

bool AppController::geometryIsOnScreen(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;

    const QList<QScreen*> screens = QGuiApplication::screens();
    // No screens at all means this is not a GUI process and there is nothing
    // to check against. Refusing the position would be a guess dressed up as
    // caution.
    if (screens.isEmpty())
        return true;

    // Overlap, not containment: a window nudged half off the edge is fine, a
    // window on a monitor that is no longer plugged in is not.
    const QRect wanted(x, y, width, height);
    for (const QScreen* screen : screens) {
        if (screen->availableGeometry().intersects(wanted))
            return true;
    }
    return false;
}

QVariantMap AppController::savedWindowGeometry() const
{
    if (!m_window.isValid())
        return {};

    QVariantMap out;
    out[QStringLiteral("width")] = m_window.width;
    out[QStringLiteral("height")] = m_window.height;
    out[QStringLiteral("maximized")] = m_window.maximized;

    // The size is always safe to restore; the position only when it still
    // lands somewhere visible.
    if (m_window.hasPosition()
        && geometryIsOnScreen(m_window.x, m_window.y, m_window.width, m_window.height)) {
        out[QStringLiteral("x")] = m_window.x;
        out[QStringLiteral("y")] = m_window.y;
    }
    return out;
}

void AppController::rememberWindowGeometry(int x, int y, int width, int height, bool maximized)
{
    // A maximised window reports the screen size; keeping the size it had
    // before is what makes un-maximising return somewhere sensible.
    WindowGeometry updated = m_window;
    updated.maximized = maximized;
    if (!maximized) {
        updated.x = x;
        updated.y = y;
        updated.width = width;
        updated.height = height;
    }

    if (updated.x == m_window.x && updated.y == m_window.y && updated.width == m_window.width
        && updated.height == m_window.height && updated.maximized == m_window.maximized) {
        return;
    }

    m_window = updated;
    if (!m_restoring && m_sessionSaveTimer)
        m_sessionSaveTimer->start();
}

AppController::~AppController()
{
    // The debounce may still be pending; losing the last few seconds of
    // navigation because the user quit quickly would be its own small bug.
    saveSessionNow();

    // Order matters: running tasks may still be writing to the index, so the
    // task manager (which joins its pool) has to go first. Leaving this to the
    // QObject child list would be a coin flip.
    delete m_taskManager;
    m_taskManager = nullptr;
    m_index.reset();
}

bool AppController::initialise(std::vector<std::unique_ptr<IPlugin>> builtIns, QString* errorOut)
{
    m_events = new EventBus(this);
    m_vfs = new VfsManager(this);
    m_taskManager = new TaskManager(this);
    m_features = new FeatureRegistry(this);
    m_previews = new PreviewRegistry(this);
    m_actions = new ActionRegistry(this);

    m_index = std::make_unique<IndexDatabase>(IndexDatabase::defaultFilePath());
    if (Result<void> opened = m_index->open(); !opened.ok()) {
        if (errorOut)
            *errorOut = opened.error().message;
        return false;
    }

    m_schedules = new ScheduleStore(ScheduleStore::defaultPath(), this);
    m_schedules->load();
    m_scheduler = new Scheduler(m_schedules, this);

    m_reports = std::make_unique<AnalysisStore>(AnalysisStore::defaultDirectory());

    m_sets = new FileSetStore(FileSetStore::defaultPath(), this);
    m_sets->load();

    m_secrets = new SecretStore(SecretStore::defaultPath(), this);
    connect(m_secrets, &SecretStore::unlockedChanged, this, &AppController::credentialsChanged);

    m_remotes = new RemoteRegistry(RemoteRegistry::defaultPath(), m_secrets, this);
    m_remotes->load();

    m_alerts = new AlertStore(AlertStore::defaultPath(), this);
    m_alerts->load();
    // An alert nobody is told about is not an alert. Only transitions are
    // announced -- the store takes care of not repeating itself.
    connect(m_alerts, &AlertStore::alertRaised, this, [this](const AlertRule& rule) {
        emit notification(static_cast<int>(EventBus::Severity::Warning), rule.label,
            rule.message.isEmpty() ? rule.describe() : rule.message);
    });

    m_services.vfs = m_vfs;
    m_services.tasks = m_taskManager;
    m_services.index = m_index.get();
    m_services.events = m_events;
    m_services.previews = m_previews;
    m_services.scheduler = m_scheduler;
    m_services.alerts = m_alerts;
    m_services.reports = m_reports.get();
    m_services.sets = m_sets;

    // The mount table is the source of truth; the bus just broadcasts it so
    // views do not have to know about VfsManager.
    connect(m_vfs, &VfsManager::mountsChanged, this, [this] { m_events->postMountsChanged(); });

    PluginManager::Destinations destinations;
    destinations.vfs = m_vfs;
    destinations.features = m_features;
    destinations.previews = m_previews;
    destinations.actions = m_actions;

    m_plugins = new PluginManager(m_services, destinations, this);

    for (auto& plugin : builtIns)
        m_plugins->addBuiltIn(std::move(plugin));
    m_plugins->loadFromDefaultPaths();

    m_bookmarks = new BookmarkModel(BookmarkModel::defaultFilePath(), this);
    connect(m_bookmarks, &BookmarkModel::countChanged, this, &AppController::refreshBookmarkActions);

    m_launcher = new FileLauncher(m_services, this);
    connect(m_launcher, &FileLauncher::failed, this, [this](const QString& uri, const QString& reason) {
        emit notification(static_cast<int>(EventBus::Severity::Warning),
            QStringLiteral("Cannot open %1").arg(VfsUri::fromString(uri).fileName()), reason);
    });

    m_mounts = new MountListModel(m_vfs, m_taskManager, this);
    m_taskModel = new TaskListModel(m_taskManager, this);
    m_terminal = new TerminalController(this);
    m_tabs = new TabsModel(m_features, this);

    // Registered after the plugins so the "new tab" entries cover every
    // feature that exists, built-in or not.
    registerShellActions();

    mountDefaultDrives();

    // Drives that need no credentials connect straight away. The rest wait for
    // the store to be opened, and the interface asks once rather than failing
    // one drive at a time.
    for (const RemoteDrive& drive : m_remotes->drives()) {
        if (drive.mountAtStartup && drive.secretFields.isEmpty())
            connectDrive(drive.id);
    }

    // Started only now: the poll runs immediately, and a rule whose handler
    // had not been registered yet would be filed as "nothing handles this".
    // Drives are mounted first for the same reason -- a report on a disk that
    // is not mounted yet would be recorded as a failure it never had.
    connect(
        m_scheduler, &Scheduler::runFinished, this, [this](const QString&, bool ok, const QString& message) {
            if (!ok) {
                emit notification(static_cast<int>(EventBus::Severity::Warning),
                    QStringLiteral("A scheduled job failed"), message);
            }
        });
    m_scheduler->start();

    // Debounced: a tab reports state changes on every navigation, and the
    // session file is not worth rewriting for each keystroke.
    m_session = std::make_unique<SessionStore>(SessionStore::defaultFilePath());
    m_sessionSaveTimer = new QTimer(this);
    m_sessionSaveTimer->setSingleShot(true);
    m_sessionSaveTimer->setInterval(1500);
    connect(m_sessionSaveTimer, &QTimer::timeout, this, &AppController::saveSessionNow);
    connect(m_tabs, &TabsModel::sessionDirty, this, [this] {
        if (!m_restoring)
            m_sessionSaveTimer->start();
    });

    if (!restoreSession()) {
        // Nothing to come back to: start on something useful rather than an
        // empty window. Falling back to whatever feature exists keeps a
        // stripped-down build usable.
        if (m_tabs->openTab(QStringLiteral("mole.browser")) < 0) {
            const QList<IFeature*> available = m_features->features();
            if (!available.isEmpty())
                m_tabs->openTab(available.first()->id());
        }
    }
    return true;
}

void AppController::mountDefaultDrives()
{
    IFileSystemFactory* factory = m_vfs->factoryFor(QStringLiteral("file"));
    if (!factory)
        return;

    const auto mountLocal = [&](const QString& displayName, const QString& path) {
        QString error;
        FileSystemPtr fs = factory->create({}, &error);
        if (!fs)
            return;

        Mount mount;
        mount.displayName = displayName;
        mount.root = VfsUri::fromLocalPath(path);
        mount.fileSystem = std::move(fs);
        m_vfs->addMount(std::move(mount));
    };

    // Home first: it is where the user actually keeps things, whatever the
    // partition layout underneath happens to be.
    mountLocal(QStringLiteral("Home"), QDir::homePath());

    // Then whatever the operating system has mounted -- the real disks, USB
    // sticks and network shares, not the sixty pseudo filesystems next to them.
    for (const SystemVolume& volume : SystemVolumes::enumerate())
        mountLocal(volume.name, volume.rootPath);
}

bool AppController::credentialsAvailable() const
{
    return SecretStore::isAvailable();
}

bool AppController::credentialsExist() const
{
    return m_secrets && m_secrets->exists();
}

bool AppController::credentialsUnlocked() const
{
    return m_secrets && m_secrets->isUnlocked();
}

bool AppController::credentialsNeeded() const
{
    return m_remotes && m_remotes->needsUnlocking();
}

bool AppController::unlockCredentials(const QString& passphrase)
{
    if (!m_secrets)
        return false;

    m_credentialsError.clear();
    const bool ok = m_secrets->exists() ? m_secrets->unlock(passphrase, &m_credentialsError)
                                        : m_secrets->create(passphrase, &m_credentialsError);
    if (!ok) {
        emit credentialsChanged();
        emit drivesChanged();
        return false;
    }

    // Everything that was waiting for a password can connect now.
    for (const RemoteDrive& drive : m_remotes->drives()) {
        if (drive.mountAtStartup && !drive.secretFields.isEmpty())
            connectDrive(drive.id);
    }

    emit credentialsChanged();
    emit drivesChanged();
    return true;
}

QVariantList AppController::driveKinds() const
{
    QVariantList out;
    if (!m_vfs)
        return out;

    for (IFileSystemFactory* factory : m_vfs->factories()) {
        // A factory whose library is missing says so instead of offering drives
        // that cannot connect. Listing it greyed out beats leaving a silent gap.
        const QVariantMap common { { QStringLiteral("factory"), factory->scheme() },
            { QStringLiteral("available"), factory->isAvailable() },
            { QStringLiteral("unavailableReason"), factory->unavailableReason() } };

        const QList<BackendVariant> variants = factory->variants();
        if (variants.isEmpty()) {
            if (factory->connectionFields().isEmpty())
                continue; // nothing to configure, so nothing to add
            QVariantMap kind = common;
            kind.insert(QStringLiteral("variant"), QString());
            kind.insert(QStringLiteral("label"), factory->displayName());
            kind.insert(QStringLiteral("description"), QString());
            out.append(kind);
            continue;
        }

        for (const BackendVariant& variant : variants) {
            QVariantMap kind = common;
            kind.insert(QStringLiteral("variant"), variant.id);
            kind.insert(QStringLiteral("label"), variant.label);
            kind.insert(QStringLiteral("description"), variant.description);
            out.append(kind);
        }
    }
    return out;
}

QVariantList AppController::driveFields(const QString& factoryScheme, const QString& variant) const
{
    QVariantList out;
    if (!m_vfs)
        return out;

    IFileSystemFactory* factory = nullptr;
    for (IFileSystemFactory* candidate : m_vfs->factories()) {
        if (candidate->scheme() == factoryScheme) {
            factory = candidate;
            break;
        }
    }
    if (!factory)
        return out;

    QList<ConnectionField> fields = factory->connectionFields();
    const QList<BackendVariant> variants = factory->variants();
    for (const BackendVariant& candidate : variants) {
        if (candidate.id == variant) {
            fields = candidate.fields;
            break;
        }
    }

    for (const ConnectionField& field : fields) {
        out.append(
            QVariantMap { { QStringLiteral("key"), field.key }, { QStringLiteral("label"), field.label },
                { QStringLiteral("kind"), static_cast<int>(field.kind) },
                { QStringLiteral("secret"), field.kind == ConnectionField::Password },
                { QStringLiteral("help"), field.help }, { QStringLiteral("required"), field.required },
                { QStringLiteral("advanced"), field.advanced }, { QStringLiteral("choices"), field.choices },
                { QStringLiteral("choiceLabels"), field.choiceLabels },
                { QStringLiteral("defaultValue"), field.defaultValue },
                { QStringLiteral("dependsOnKey"), field.dependsOnKey },
                { QStringLiteral("dependsOnValues"), field.dependsOnValues } });
    }
    return out;
}

QVariantList AppController::configuredDrives() const
{
    QVariantList out;
    if (!m_remotes)
        return out;

    for (const RemoteDrive& drive : m_remotes->drives()) {
        const bool mounted = m_vfs && m_vfs->resolve(drive.rootUri()) != nullptr;
        out.append(QVariantMap { { QStringLiteral("id"), drive.id }, { QStringLiteral("name"), drive.name },
            { QStringLiteral("factory"), drive.factoryScheme }, { QStringLiteral("variant"), drive.variant },
            { QStringLiteral("root"), drive.root }, { QStringLiteral("uri"), drive.rootUri().toString() },
            { QStringLiteral("settings"), drive.settings },
            { QStringLiteral("secretFields"), drive.secretFields },
            { QStringLiteral("needsUnlock"), !drive.secretFields.isEmpty() && !credentialsUnlocked() },
            { QStringLiteral("connected"), mounted } });
    }
    return out;
}

bool AppController::saveDrive(const QString& id, const QString& name, const QString& factoryScheme,
    const QString& variant, const QString& root, const QVariantMap& values)
{
    if (!m_remotes || !m_vfs)
        return false;

    RemoteDrive drive = m_remotes->drive(id);
    drive.id = id;
    drive.name = name.trimmed();
    drive.factoryScheme = factoryScheme;
    drive.variant = variant;
    drive.root = root;

    // Which values are secret is decided here, from the factory's own field
    // descriptions, rather than by whoever filled in the form. A view that had
    // to remember would eventually forget.
    QSet<QString> secretKeys;
    const QVariantList fields = driveFields(factoryScheme, variant);
    for (const QVariant& value : fields) {
        const QVariantMap field = value.toMap();
        if (field.value(QStringLiteral("secret")).toBool())
            secretKeys.insert(field.value(QStringLiteral("key")).toString());
    }

    QVariantMap settings;
    QVariantMap secrets;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (it.value().toString().isEmpty())
            continue;
        if (secretKeys.contains(it.key()))
            secrets.insert(it.key(), it.value());
        else
            settings.insert(it.key(), it.value());
    }
    drive.settings = settings;

    m_credentialsError.clear();
    if (!m_remotes->put(drive, secrets, &m_credentialsError)) {
        emit notification(static_cast<int>(EventBus::Severity::Warning),
            QStringLiteral("Could not save the drive"), m_credentialsError);
        return false;
    }
    emit drivesChanged();
    return true;
}

bool AppController::removeDrive(const QString& id)
{
    if (!m_remotes)
        return false;
    const RemoteDrive drive = m_remotes->drive(id);
    if (drive.isValid())
        disconnectDrive(id);
    const bool removed = m_remotes->remove(id);
    if (removed)
        emit drivesChanged();
    return removed;
}

QString AppController::connectDrive(const QString& id)
{
    if (!m_remotes || !m_vfs)
        return QStringLiteral("Not ready");

    const RemoteDrive drive = m_remotes->drive(id);
    if (!drive.isValid())
        return QStringLiteral("No such drive");

    QString error;
    const QVariantMap config = m_remotes->configFor(drive, &error);
    if (config.isEmpty())
        return error.isEmpty() ? QStringLiteral("This drive has no configuration") : error;

    IFileSystemFactory* factory = nullptr;
    for (IFileSystemFactory* candidate : m_vfs->factories()) {
        if (candidate->scheme() == drive.factoryScheme) {
            factory = candidate;
            break;
        }
    }
    if (!factory)
        return QStringLiteral("Nothing here can serve a %1 drive").arg(drive.factoryScheme);

    FileSystemPtr fs = factory->create(config, &error);
    if (!fs)
        return error.isEmpty() ? QStringLiteral("Could not connect") : error;

    Mount mount;
    mount.id = drive.id;
    mount.displayName = drive.name;
    mount.root = drive.rootUri();
    mount.fileSystem = std::move(fs);
    m_vfs->addMount(mount);

    emit credentialsChanged();
    emit drivesChanged();
    return {};
}

void AppController::disconnectDrive(const QString& id)
{
    if (m_vfs)
        m_vfs->removeMount(id);
    emit credentialsChanged();
    emit drivesChanged();
}

QString AppController::monospaceFont() const
{
    // A preferred list first: the fonts people install for code are markedly
    // better at distinguishing 0/O and 1/l/I than the system default, and that
    // distinction is the whole reason a preview is monospaced.
    static const QString family = [] {
        static const QStringList preferred = { QStringLiteral("JetBrains Mono"), QStringLiteral("Fira Code"),
            QStringLiteral("Cascadia Mono"), QStringLiteral("Source Code Pro"), QStringLiteral("Hack"),
            QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono"),
            QStringLiteral("Noto Sans Mono"), QStringLiteral("Ubuntu Mono"), QStringLiteral("Menlo"),
            QStringLiteral("Consolas") };

        const QStringList available = QFontDatabase::families();
        for (const QString& candidate : preferred) {
            if (available.contains(candidate, Qt::CaseInsensitive))
                return candidate;
        }
        // Whatever this platform calls its fixed-width font. Never empty.
        return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    }();
    return family;
}

QStringList AppController::pluginSummary() const
{
    QStringList out;
    if (!m_plugins)
        return out;
    for (const PluginManager::LoadedPlugin& plugin : m_plugins->loaded()) {
        out.append(QStringLiteral("%1 %2%3").arg(plugin.metadata.name, plugin.metadata.version,
            plugin.builtIn ? QStringLiteral(" (built-in)") : QString()));
    }
    return out;
}

QStringList AppController::pluginErrors() const
{
    return m_plugins ? m_plugins->errors() : QStringList {};
}

int AppController::openFeatureTab(const QString& featureId)
{
    // Read before opening, because opening changes which tab is current -- and
    // asking afterwards would read the new tab's empty selection back to itself.
    const QString here = currentLocation();
    const QStringList targets = currentTargets();

    const int row = m_tabs->openTab(featureId);
    if (row < 0)
        return row;

    QObject* controller = m_tabs->controllerAt(row);
    // Asked for by property rather than by type: any tab that declares a place
    // to start from gets seeded, including one from a plugin the shell has
    // never heard of.
    if (!controller)
        return row;

    if (!here.isEmpty() && controller->property("rootUri").isValid())
        controller->setProperty("rootUri", here);
    // The same idea for a tab that wants a starting suggestion rather than a
    // location: an alert opened from a folder should already be pointed at it.
    if (!here.isEmpty() && controller->property("suggestedTarget").isValid())
        controller->setProperty("suggestedTarget", here);
    // And a tab that renames, analyses or otherwise acts on a list is handed
    // whatever the previous tab was aimed at -- a selection or a set, without
    // this layer knowing which of the two it was.
    if (!targets.isEmpty() && controller->metaObject()->indexOfMethod("setTargets(QStringList)") >= 0) {
        QMetaObject::invokeMethod(controller, "setTargets", Q_ARG(QStringList, targets));
    }

    return row;
}

void AppController::openLocation(const QString& uri)
{
    const int row = m_tabs->openTab(QStringLiteral("mole.browser"));
    if (row < 0)
        return;
    if (auto* controller = m_tabs->controllerAt(row))
        QMetaObject::invokeMethod(controller, "navigateActive", Q_ARG(QString, uri));
}

void AppController::goTo(const QString& uri)
{
    QObject* current = m_tabs->currentController();
    // Reuse the current tab when it knows how to navigate; otherwise the user
    // asked to go somewhere a search tab cannot take them, so open a browser.
    if (current && current->metaObject()->indexOfMethod("navigateActive(QString)") >= 0) {
        QMetaObject::invokeMethod(current, "navigateActive", Q_ARG(QString, uri));
        return;
    }
    openLocation(uri);
}

bool AppController::isMountableArchive(const QString& uri) const
{
    const VfsUri target = VfsUri::fromString(uri);
    if (!target.isValid())
        return false;

    const QString suffix = target.suffix();
    if (suffix.isEmpty())
        return false;

    // Ask the factories rather than hardcoding a list, so a plugin that mounts
    // .iso or .sqlite works here without the host being taught about it.
    for (IFileSystemFactory* factory : m_vfs->factories()) {
        if (factory->mountableFileSuffixes().contains(suffix))
            return true;
    }
    return false;
}

QString AppController::openArchive(const QString& archiveUri)
{
    const VfsUri target = VfsUri::fromString(archiveUri);
    const QString localPath = target.toLocalPath();
    if (localPath.isEmpty()) {
        emit notification(static_cast<int>(EventBus::Severity::Warning),
            QStringLiteral("Cannot open as a drive"),
            QStringLiteral("Only local files can be mounted for now: %1").arg(archiveUri));
        return {};
    }

    const QString suffix = target.suffix();
    for (IFileSystemFactory* factory : m_vfs->factories()) {
        if (!factory->mountableFileSuffixes().contains(suffix))
            continue;

        const VfsUri root = factory->rootUriForFile(localPath);
        // Mounting the same archive twice would just duplicate the sidebar row.
        for (const Mount& existing : m_vfs->mounts()) {
            if (existing.root == root) {
                openLocation(root.toString());
                return root.toString();
            }
        }

        QString error;
        const QString id = m_vfs->addMount(
            factory->scheme(), QFileInfo(localPath).fileName(), factory->configForFile(localPath), &error);
        if (id.isEmpty()) {
            emit notification(static_cast<int>(EventBus::Severity::Error),
                QStringLiteral("Cannot mount %1").arg(QFileInfo(localPath).fileName()), error);
            return {};
        }

        openLocation(m_vfs->mount(id).root.toString());
        return m_vfs->mount(id).root.toString();
    }

    emit notification(static_cast<int>(EventBus::Severity::Warning),
        QStringLiteral("No plugin can mount this file"),
        QStringLiteral("Nothing registered handles .%1").arg(suffix));
    return {};
}

QVariant AppController::currentTabProperty(const char* name, const QVariant& fallback) const
{
    QObject* current = m_tabs ? m_tabs->currentController() : nullptr;
    if (!current)
        return fallback;

    // Asking the meta-object rather than casting keeps the menu working for
    // tab kinds the shell has never heard of -- a plugin's tab simply does not
    // answer, and the entry greys out.
    const QVariant value = current->property(name);
    return value.isValid() ? value : fallback;
}

void AppController::setCurrentTabProperty(const char* name, const QVariant& value)
{
    if (QObject* current = m_tabs ? m_tabs->currentController() : nullptr)
        current->setProperty(name, value);
}

void AppController::registerShellActions()
{
    // --- File ---------------------------------------------------------
    //
    // One entry per registered feature, so a plugin that adds a tab kind shows
    // up here without touching the shell. Shortcut labels mirror the Shortcut
    // items declared in QML; they are display only.
    static const QHash<QString, QString> knownShortcuts {
        { QStringLiteral("mole.browser"), QStringLiteral("Ctrl+T") },
        { QStringLiteral("mole.commander"), QStringLiteral("Ctrl+Shift+T") },
        { QStringLiteral("mole.livesearch"), QStringLiteral("Ctrl+F") },
        { QStringLiteral("mole.indexsearch"), QStringLiteral("Ctrl+Shift+I") },
    };

    int order = 10;
    for (IFeature* feature : m_features->features()) {
        MenuAction action;
        action.id = QStringLiteral("mole.file.newTab.%1").arg(feature->id());
        action.section = MenuAction::Section::File;
        action.title = QStringLiteral("New %1 tab").arg(feature->title());
        action.iconText = feature->iconText();
        action.shortcut = knownShortcuts.value(feature->id());
        action.sortOrder = order;
        const QString featureId = feature->id();
        action.trigger = [this, featureId] { openFeatureTab(featureId); };
        m_actions->addAction(std::move(action));
        order += 10;
    }

    {
        MenuAction action;
        action.id = QStringLiteral("mole.file.closeTab");
        action.section = MenuAction::Section::File;
        action.title = QStringLiteral("Close tab");
        action.shortcut = QStringLiteral("Ctrl+W");
        action.sortOrder = 200;
        action.separatorBefore = true;
        action.enabled = [this] { return m_tabs->rowCount() > 0; };
        action.trigger = [this] { m_tabs->closeCurrentTab(); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.file.quit");
        action.section = MenuAction::Section::File;
        action.title = QStringLiteral("Quit");
        action.shortcut = QStringLiteral("Ctrl+Q");
        action.sortOrder = 900;
        action.separatorBefore = true;
        action.trigger = [] { QCoreApplication::quit(); };
        m_actions->addAction(std::move(action));
    }

    // --- View ---------------------------------------------------------
    //
    // These act on whichever tab is open. A tab that has no such property
    // simply greys the entry out.
    // Three ways of looking at the same tab. Presented as a set so it is
    // obvious they are alternatives, not independent switches.
    struct LayoutEntry
    {
        const char* id;
        const char* title;
        int mode;
    };
    int layoutOrder = 10;
    for (const LayoutEntry& layout : { LayoutEntry { "mole.view.singlePane", "Single pane", 0 },
             LayoutEntry { "mole.view.dualPane", "Dual pane", 1 },
             LayoutEntry { "mole.view.gridView", "Grid of icons", 2 } }) {
        MenuAction action;
        action.id = QString::fromLatin1(layout.id);
        action.section = MenuAction::Section::View;
        action.title = QString::fromLatin1(layout.title);
        action.sortOrder = layoutOrder;
        const int mode = layout.mode;
        action.enabled = [this] { return currentTabProperty("viewMode").isValid(); };
        action.checked = [this, mode] { return currentTabProperty("viewMode", -1).toInt() == mode; };
        action.trigger = [this, mode] { setCurrentTabProperty("viewMode", mode); };
        m_actions->addAction(std::move(action));
        layoutOrder += 10;
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.view.hiddenFiles");
        action.section = MenuAction::Section::View;
        action.title = QStringLiteral("Show hidden files");
        action.sortOrder = 20;
        action.separatorBefore = true;

        const auto paneFiles = [this]() -> QObject* {
            const QVariant pane = currentTabProperty("activePane");
            QObject* controller = pane.value<QObject*>();
            return controller ? controller->property("files").value<QObject*>() : nullptr;
        };

        action.enabled = [paneFiles] { return paneFiles() != nullptr; };
        action.checked = [paneFiles] {
            QObject* files = paneFiles();
            return files && files->property("showHidden").toBool();
        };
        action.trigger = [paneFiles] {
            if (QObject* files = paneFiles())
                files->setProperty("showHidden", !files->property("showHidden").toBool());
        };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.view.filter");
        action.section = MenuAction::Section::View;
        action.title = QStringLiteral("Filter this folder");
        action.shortcut = QStringLiteral("type to filter");
        action.sortOrder = 25;
        action.enabled = [this] { return currentTabProperty("activePane").value<QObject*>() != nullptr; };
        // The bar itself lives in QML, so the shell only asks for it.
        action.trigger = [this] { emit dialogRequested(QStringLiteral("mole.view.filter")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.view.refresh");
        action.section = MenuAction::Section::View;
        action.title = QStringLiteral("Refresh");
        action.shortcut = QStringLiteral("Ctrl+R");
        action.sortOrder = 30;
        action.enabled = [this] { return currentTabProperty("activePane").value<QObject*>() != nullptr; };
        action.trigger = [this] {
            if (QObject* pane = currentTabProperty("activePane").value<QObject*>())
                QMetaObject::invokeMethod(pane, "refresh");
        };
        m_actions->addAction(std::move(action));
    }

    // --- Bookmarks ----------------------------------------------------
    //
    // The saved places themselves are appended below, rebuilt whenever the
    // list changes, so they are reachable from the keyboard like anything else.
    {
        MenuAction action;
        action.id = QStringLiteral("mole.bookmarks.add");
        action.section = MenuAction::Section::Bookmarks;
        action.title = QStringLiteral("Add current folder");
        action.shortcut = QStringLiteral("Ctrl+D");
        action.sortOrder = 10;
        action.enabled = [this] {
            const QString here = currentLocation();
            return !here.isEmpty() && !m_bookmarks->contains(here);
        };
        action.trigger = [this] { m_bookmarks->add(currentLocation()); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.bookmarks.remove");
        action.section = MenuAction::Section::Bookmarks;
        action.title = QStringLiteral("Remove current folder");
        action.sortOrder = 20;
        action.enabled = [this] { return m_bookmarks->contains(currentLocation()); };
        action.trigger = [this] { m_bookmarks->removeUri(currentLocation()); };
        m_actions->addAction(std::move(action));
    }
    refreshBookmarkActions();

    // --- Tools --------------------------------------------------------
    //
    // Where plugins land. Built-ins leave room above and below.
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.preview");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Preview this file");
        action.shortcut = QStringLiteral("F3");
        action.sortOrder = 5;
        action.enabled = [this] { return !currentFile().isEmpty(); };
        action.trigger = [this] { previewFile(currentFile()); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.analyse");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Analyse folder");
        action.shortcut = QStringLiteral("Ctrl+Shift+A");
        action.sortOrder = 8;
        action.enabled = [this] { return !currentLocation().isEmpty(); };
        action.trigger = [this] { analyseSelection(); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.file.drives");
        action.section = MenuAction::Section::File;
        action.title = QStringLiteral("Drives…");
        action.sortOrder = 30;
        action.enabled = [] { return true; };
        action.trigger = [this] { emit drivesRequested(); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.terminal");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Terminal here");
        action.shortcut = QStringLiteral("Ctrl+`");
        action.sortOrder = 2;
        action.enabled = [this] { return m_terminal && m_terminal->isAvailable(); };
        action.trigger = [this] {
            if (!m_terminal)
                return;
            if (m_terminal->isVisible() && m_terminal->isRunning()) {
                m_terminal->setVisible(false);
                return;
            }
            m_terminal->setVisible(true);
            // Opening it from a folder starts the shell there. Navigating
            // afterwards does not drag it along -- a shell has its own idea of
            // where it is, and fighting that is worse than leaving it alone.
            m_terminal->open(currentLocation());
        };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.sync");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Sync folders");
        action.sortOrder = 4;
        action.enabled = [this] { return !currentLocation().isEmpty(); };
        action.trigger = [this] {
            // Read before opening, or the new tab is asked to seed itself from
            // its own empty state.
            const QString here = currentLocation();
            const int row = openFeatureTab(QStringLiteral("core.sync"));
            if (row >= 0 && !here.isEmpty()) {
                if (QObject* controller = m_tabs->controllerAt(row)) {
                    // Opening from a folder makes that folder the source, which
                    // is the only reading that does not surprise anybody.
                    if (controller->property("sourceUri").toString().isEmpty())
                        controller->setProperty("sourceUri", here);
                }
            }
        };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.duplicates");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Find duplicates");
        action.sortOrder = 5;
        action.enabled = [this] { return !currentLocation().isEmpty(); };
        action.trigger = [this] {
            // Read before opening: afterwards the current tab is the new one,
            // and it would be asked to seed itself from its own empty state.
            const QString here = currentLocation();
            const bool hadSelection = !currentTargets().isEmpty();

            const int row = openFeatureTab(QStringLiteral("core.duplicates"));
            // Nothing selected means "this folder", the same reading the report
            // takes -- and the one people expect from a scan.
            if (row >= 0 && !hadSelection && !here.isEmpty()) {
                if (QObject* controller = m_tabs->controllerAt(row)) {
                    QMetaObject::invokeMethod(
                        controller, "setTargets", Q_ARG(QStringList, QStringList { here }));
                }
            }
        };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.bulkRename");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Bulk rename");
        action.shortcut = QStringLiteral("Ctrl+Shift+R");
        action.sortOrder = 6;
        action.enabled = [this] { return !currentTargets().isEmpty(); };
        action.trigger = [this] { openFeatureTab(QStringLiteral("core.bulkrename")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.addToSet");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Add to set");
        action.shortcut = QStringLiteral("Ctrl+Shift+S");
        action.sortOrder = 7;
        action.enabled = [this] { return !currentTargets().isEmpty(); };
        action.trigger = [this] {
            // Into the most recent set, or a new one when there is none. The
            // set tab is then opened so the result is visible rather than
            // silently filed away.
            const QVariantList choices = setChoices();
            const QString id = choices.isEmpty()
                ? QString()
                : choices.last().toMap().value(QStringLiteral("id")).toString();
            addToSet(id, QStringLiteral("New set"));
            openFeatureTab(QStringLiteral("core.filesets"));
        };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.reports");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Saved reports");
        action.sortOrder = 8;
        action.enabled = [] { return true; };
        action.trigger = [this] { openFeatureTab(QStringLiteral("core.reports")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.alerts");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Alerts");
        action.shortcut = QStringLiteral("Ctrl+Shift+L");
        action.sortOrder = 9;
        action.enabled = [] { return true; };
        action.trigger = [this] { openFeatureTab(QStringLiteral("core.alerts")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.automation");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Scheduled jobs");
        action.shortcut = QStringLiteral("Ctrl+Shift+J");
        action.sortOrder = 9;
        // Always available: the reason to open it is usually that something is
        // failing, which is exactly when nothing else points you at it.
        action.enabled = [] { return true; };
        action.trigger = [this] { openFeatureTab(QStringLiteral("core.automation")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.indexFolder");
        action.section = MenuAction::Section::Tools;
        action.title = QStringLiteral("Index this folder");
        action.iconText = QStringLiteral("\u26C1");
        action.sortOrder = 10;
        action.enabled = [this] { return currentTabProperty("activePane").value<QObject*>() != nullptr; };
        action.trigger = [this] {
            QObject* pane = currentTabProperty("activePane").value<QObject*>();
            if (!pane)
                return;
            queueScan(pane->property("currentUri").toString(), pane->property("locationName").toString());
        };
        m_actions->addAction(std::move(action));
    }

    // --- Help ---------------------------------------------------------
    //
    // The two dialogs live in QML, so these only announce that they were
    // picked and the shell opens the right one.
    for (const auto& [id, title, shortcut, sortOrder] :
        { std::tuple { QStringLiteral("mole.help.shortcuts"), QStringLiteral("Keyboard shortcuts"),
              QStringLiteral("F1"), 10 },
            std::tuple { QStringLiteral("mole.help.plugins"), QStringLiteral("Plugins"), QString(), 20 },
            std::tuple { QStringLiteral("mole.help.about"), QStringLiteral("About"), QString(), 30 } }) {
        MenuAction action;
        action.id = id;
        action.section = MenuAction::Section::Help;
        action.title = title;
        action.shortcut = shortcut;
        action.sortOrder = sortOrder;
        const QString actionId = id;
        action.trigger = [this, actionId] { emit dialogRequested(actionId); };
        m_actions->addAction(std::move(action));
    }
}

QString AppController::currentLocation() const
{
    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    return pane ? pane->property("currentUri").toString() : QString();
}

QString AppController::currentFile() const
{
    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    if (!pane)
        return {};

    QObject* files = pane->property("files").value<QObject*>();
    if (!files)
        return {};

    const int row = pane->property("currentIndex").toInt();
    QString uri;
    QMetaObject::invokeMethod(files, "uriAt", Q_RETURN_ARG(QString, uri), Q_ARG(int, row));

    bool isDir = false;
    QMetaObject::invokeMethod(files, "isDirAt", Q_RETURN_ARG(bool, isDir), Q_ARG(int, row));
    return isDir ? QString() : uri;
}

void AppController::previewFile(const QString& uri)
{
    if (uri.isEmpty())
        return;

    // One preview tab, reused. Pressing F3 on twenty files should not leave
    // twenty tabs behind.
    for (int row = 0; row < m_tabs->rowCount(); ++row) {
        if (m_tabs->index(row, 0).data(TabsModel::FeatureIdRole).toString() != QLatin1String("mole.preview")) {
            continue;
        }
        if (QObject* controller = m_tabs->controllerAt(row)) {
            QMetaObject::invokeMethod(controller, "open", Q_ARG(QString, uri));
            m_tabs->setCurrentIndex(row);
            return;
        }
    }

    const int row = m_tabs->openTab(QStringLiteral("mole.preview"));
    if (row < 0)
        return;
    if (QObject* controller = m_tabs->controllerAt(row))
        QMetaObject::invokeMethod(controller, "open", Q_ARG(QString, uri));
}

QStringList AppController::currentTargets() const
{
    QObject* controller = m_tabs ? m_tabs->currentController() : nullptr;
    if (!controller)
        return {};

    // Asked for by name, not by type. Anything that can say what it is aimed at
    // -- a browser pane's selection, a set's members, whatever a plugin adds --
    // answers the same question, so an operation never learns what a set is.
    if (controller->metaObject()->indexOfMethod("targetUris()") >= 0) {
        QStringList uris;
        if (QMetaObject::invokeMethod(controller, "targetUris", Q_RETURN_ARG(QStringList, uris))
            && !uris.isEmpty()) {
            return uris;
        }
    }

    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    if (!pane)
        return {};
    QObject* files = pane->property("files").value<QObject*>();
    if (!files)
        return {};

    QStringList selected;
    QMetaObject::invokeMethod(files, "selectedUris", Q_RETURN_ARG(QStringList, selected));
    return selected;
}

QStringList AppController::selectedFolders() const
{
    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    if (!pane)
        return {};

    QObject* files = pane->property("files").value<QObject*>();
    if (!files)
        return {};

    QStringList selected;
    QMetaObject::invokeMethod(files, "selectedUris", Q_RETURN_ARG(QStringList, selected));

    QStringList folders;
    for (const QString& uri : selected) {
        int row = -1;
        QMetaObject::invokeMethod(files, "rowOfUri", Q_RETURN_ARG(int, row), Q_ARG(QString, uri));
        bool isDir = false;
        QMetaObject::invokeMethod(files, "isDirAt", Q_RETURN_ARG(bool, isDir), Q_ARG(int, row));
        if (isDir)
            folders.append(uri);
    }
    return folders;
}

void AppController::analyseSelection()
{
    QStringList targets = selectedFolders();
    // A tab that names its own targets -- a set, say -- is taken at its word,
    // because there is no pane behind it to fall back to.
    if (targets.isEmpty())
        targets = currentTargets();
    // Nothing ticked means "this folder", which is what the user is looking at.
    if (targets.isEmpty()) {
        const QString here = currentLocation();
        if (here.isEmpty())
            return;
        targets.append(here);
    }

    const int row = m_tabs->openTab(QStringLiteral("mole.analysis"));
    if (row < 0)
        return;
    if (QObject* controller = m_tabs->controllerAt(row))
        QMetaObject::invokeMethod(controller, "analyse", Q_ARG(QStringList, targets));
}

void AppController::openReportFor(const QString& uri)
{
    if (uri.isEmpty())
        return;

    // Reuses an analysis tab already showing this folder rather than opening a
    // second one on the same thing.
    for (int row = 0; row < m_tabs->rowCount(); ++row) {
        QObject* controller = m_tabs->controllerAt(row);
        if (!controller || controller->metaObject()->indexOfMethod("setTargets(QStringList)") < 0)
            continue;
        const QVariant targets = controller->property("targetUris");
        if (targets.isValid() && targets.toStringList() == QStringList { uri }) {
            m_tabs->setCurrentIndex(row);
            return;
        }
    }

    const int row = m_tabs->openTab(QStringLiteral("mole.analysis"));
    if (row < 0)
        return;
    if (QObject* controller = m_tabs->controllerAt(row))
        QMetaObject::invokeMethod(controller, "setTargets", Q_ARG(QStringList, QStringList { uri }));
}

int AppController::addToSet(const QString& setId, const QString& newSetName)
{
    if (!m_sets)
        return 0;

    const QStringList targets = currentTargets();
    if (targets.isEmpty())
        return 0;

    QString id = setId;
    if (id.isEmpty()) {
        const FileSet created = m_sets->create(
            newSetName.trimmed().isEmpty() ? QStringLiteral("New set") : newSetName, targets);
        return created.count();
    }
    return m_sets->addTo(id, targets);
}

QVariantList AppController::setChoices() const
{
    QVariantList out;
    if (!m_sets)
        return out;
    const QList<FileSet> sets = m_sets->sets();
    for (const FileSet& set : sets) {
        out.append(QVariantMap { { QStringLiteral("id"), set.id }, { QStringLiteral("name"), set.name },
            { QStringLiteral("count"), set.count() } });
    }
    return out;
}

void AppController::refreshBookmarkActions()
{
    if (!m_actions || !m_bookmarks)
        return;

    // Rebuilt wholesale rather than kept in sync row by row: the list is a
    // handful of entries and correctness beats cleverness here.
    static const QString prefix = QStringLiteral("mole.bookmarks.go.");
    m_actions->removeActionsStartingWith(prefix);

    int order = 100;
    for (const Bookmark& bookmark : m_bookmarks->bookmarks()) {
        MenuAction action;
        action.id = prefix + bookmark.uri;
        action.section = MenuAction::Section::Bookmarks;
        action.title = bookmark.name;
        action.sortOrder = order;
        action.separatorBefore = order == 100;
        const QString uri = bookmark.uri;
        action.trigger = [this, uri] { goTo(uri); };
        m_actions->addAction(std::move(action));
        order += 10;
    }
}

QVariantList AppController::buildMenu() const
{
    return m_actions ? m_actions->buildModel() : QVariantList {};
}

bool AppController::triggerAction(const QString& id)
{
    return m_actions && m_actions->trigger(id);
}

QString AppController::defaultLocation() const
{
    return VfsUri::fromLocalPath(QDir::homePath()).toString();
}

void AppController::openExternally(const QString& uri)
{
    if (m_launcher)
        m_launcher->open(VfsUri::fromString(uri));
}

void AppController::queueScan(const QString& uri, const QString& label)
{
    const VfsUri root = VfsUri::fromString(uri);
    FileSystemPtr fs = m_vfs->resolve(root);
    if (!fs) {
        emit notification(static_cast<int>(EventBus::Severity::Warning), QStringLiteral("Cannot scan"),
            QStringLiteral("No drive is mounted for %1").arg(uri));
        return;
    }

    auto* task = new ScanTask(std::move(fs), root, label.isEmpty() ? uri : label, m_index.get());
    connect(task, &Task::finished, this, [this, task] {
        if (task->state() == Task::State::Succeeded)
            m_events->postIndexUpdated(-1, task->filesIndexed());
    });
    m_taskManager->submit(task);
}

} // namespace mole
