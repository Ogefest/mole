#include "ui/AppController.h"

#include "host/ActionRegistry.h"
#include "host/FeatureRegistry.h"
#include "host/MetadataRegistry.h"
#include "host/PluginManager.h"
#include "host/PreviewRegistry.h"
#include "host/ThumbnailRegistry.h"
#include "sdk/FeatureController.h"
#include "sdk/ScanReaders.h"
#include "ui/DragSource.h"
#include "ui/FileLauncher.h"
#include "ui/SessionStore.h"
#include "ui/models/CommandPaletteModel.h"
#include "ui/models/DriveListModel.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TabsModel.h"
#include "ui/models/TaskListModel.h"

#include "core/alerts/AlertStore.h"
#include "core/analysis/AnalysisStore.h"
#include "core/automation/ScheduleStore.h"
#include "core/credentials/SecretStore.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/IndexSummary.h"
#include "core/index/ScanTask.h"
#include "core/sets/FileSetStore.h"
#ifdef MOLE_HAVE_ARCHIVE
#include "plugins/archive/CompressTask.h"
#endif

#include "ui/UpdateCheck.h"

#include "core/settings/Preferences.h"
#include "core/tasks/DriveCheckTask.h"
#include "core/tasks/FolderSizesTask.h"
#include "core/tasks/SweepLeftoversTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/IFileSystemFactory.h"
#include "core/vfs/RemoteRegistry.h"
#include "core/vfs/SystemVolumes.h"
#include "core/vfs/VfsManager.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QScreen>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace mole {

AppController::AppController(QObject* parent)
    : QObject(parent)
    , m_colour(new Palette(this))
{
    // Built here rather than in initialise(), because every binding in the window
    // reaches through it: a null palette during startup would leave the first
    // frame painted in Qt's defaults.
}

bool AppController::restoreSession()
{
    if (!m_session)
        return false;

    const Session saved = m_session->load();
    m_window = saved.window;
    if (saved.tabs.isEmpty()) {
        // Said rather than left out. A window that came up on a default tab is
        // either a first run or a session file that went missing, and those are
        // different problems.
        qInfo("Session: nothing to restore");
        return false;
    }

    // Opening tabs marks the session dirty; suppress that so a restore cannot
    // overwrite the file it is still reading from.
    m_restoring = true;
    const int restored = m_tabs->restoreSession(saved);
    m_restoring = false;

    // Logged here rather than in recordStartup(), because this is where the fact
    // is. A tab in the file that no longer has a feature to open -- a plugin that
    // did not load this time -- is silently absent otherwise, and "4 of 5" is the
    // line that would explain a window that came back missing something.
    if (restored == saved.tabs.size())
        qInfo("Session: %d tabs restored", restored);
    else
        qInfo("Session: %d of %lld tabs restored", restored, static_cast<long long>(saved.tabs.size()));

    return restored > 0;
}

void AppController::watchStore(JsonFileStore* store)
{
    // A change the interface has already shown and the disk has not taken is the
    // failure nobody can see. Every store says so the same way, through one
    // place, rather than each caller of save() deciding for itself -- which is
    // what "nothing at all" turned out to mean. See ADR-0089.
    connect(store, &JsonFileStore::saveFailed, this, [this](const QString& reason) {
        emit notification(static_cast<int>(EventBus::Severity::Warning), QStringLiteral("Not saved"), reason);
    });
    // Named by file rather than by path: this goes on a screen and into the
    // session log, and neither may carry a directory off somebody's machine.
    connect(store, &JsonFileStore::loadFoundDamage, this, [this](const QString& kept) {
        emit notification(static_cast<int>(EventBus::Severity::Warning),
            QStringLiteral("A settings file could not be read"),
            QStringLiteral("It was kept as %1, and nothing has been written over it.")
                .arg(QFileInfo(kept).fileName()));
    });
}

void AppController::saveSessionNow()
{
    if (!m_session || !m_tabs || m_restoring)
        return;

    Session session = m_tabs->captureSession();
    session.window = m_window;
    QString reason;
    if (!m_session->save(session, &reason)) {
        // Said out loud. This runs on the way out as well as during a run, and a
        // session that did not reach the disk is a window that comes back wrong
        // with nothing having been said at any point.
        emit notification(static_cast<int>(EventBus::Severity::Warning), QStringLiteral("Not saved"), reason);
    }
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

/// Qt's own visibility, reduced to the three states worth restoring.
///
/// Minimized deliberately reads as Normal, which is what it did when this was a
/// boolean: a minimised window is not a state to come back in, and the metrics
/// it reports are the ones it will be restored to anyway.
WindowState AppController::windowStateOf(int visibility)
{
    switch (static_cast<QWindow::Visibility>(visibility)) {
    case QWindow::Maximized:
        return WindowState::Maximized;
    case QWindow::FullScreen:
        return WindowState::FullScreen;
    default:
        break;
    }
    return WindowState::Normal;
}

QVariantMap AppController::savedWindowGeometry() const
{
    if (!m_window.isValid())
        return {};

    QVariantMap out;
    out[QStringLiteral("width")] = m_window.width;
    out[QStringLiteral("height")] = m_window.height;
    out[QStringLiteral("windowState")] = windowStateName(m_window.state);

    // The size is always safe to restore; the position only when it still
    // lands somewhere visible.
    if (m_window.hasPosition()
        && geometryIsOnScreen(m_window.x, m_window.y, m_window.width, m_window.height)) {
        out[QStringLiteral("x")] = m_window.x;
        out[QStringLiteral("y")] = m_window.y;
    }
    return out;
}

void AppController::rememberWindowGeometry(int x, int y, int width, int height, int visibility)
{
    WindowGeometry updated = m_window;
    updated.state = windowStateOf(visibility);

    // A maximised window reports the screen size, and so does a full-screen
    // one; keeping the size it had before is what makes coming back out of
    // either return somewhere sensible rather than filling the display again.
    if (updated.state == WindowState::Normal) {
        updated.x = x;
        updated.y = y;
        updated.width = width;
        updated.height = height;
    }

    if (updated.x == m_window.x && updated.y == m_window.y && updated.width == m_window.width
        && updated.height == m_window.height && updated.state == m_window.state) {
        return;
    }

    m_window = updated;
    if (!m_restoring && m_sessionSaveTimer) {
        m_sessionTouched = true;
        m_sessionSaveTimer->start();
    }
}

AppController::~AppController()
{
    // The debounce may still be pending; losing the last few seconds of
    // navigation because the user quit quickly would be its own small bug.
    //
    // Only when something actually changed, though. `mole --plugins` and
    // `mole --diagnostics` build this controller, initialise it, print what they
    // were asked for and return -- and this destructor then wrote the user's
    // session from a run that opened no window. Harmless while the file came back
    // whole; not harmless at all with a plugin missing, and the person typing
    // `mole --plugins` to find out why a plugin will not load is exactly the
    // person whose tabs it would have taken. Restoring does not count as a
    // change: it sets m_restoring, which is what suppresses the marker below.
    // See MOLE-350.
    if (m_sessionTouched)
        saveSessionNow();

    // Order matters: running tasks may still be writing to the index, so the
    // task manager (which joins its pool) has to go first. Leaving this to the
    // QObject child list would be a coin flip.
    delete m_taskManager;
    m_taskManager = nullptr;
    if (m_index)
        m_index->doNotQueryFrom(nullptr);
    m_index.reset();
    IFileSystem::doNotCallFrom(nullptr);
}

bool AppController::initialise(std::vector<std::unique_ptr<IPlugin>> builtIns, QString* errorOut)
{
    m_events = new EventBus(this);
    m_vfs = new VfsManager(this);
    m_taskManager = new TaskManager(this);
    m_features = new FeatureRegistry(this);
    m_previews = new PreviewRegistry(this);
    m_metadata = new MetadataRegistry(this);
    m_thumbnails = new ThumbnailRegistry(this);
    m_actions = new ActionRegistry(this);

    m_index = std::make_unique<IndexDatabase>(IndexDatabase::defaultFilePath());
    if (Result<void> opened = m_index->open(); !opened.ok()) {
        if (errorOut)
            *errorOut = opened.error().message;
        return false;
    }

    m_schedules = new ScheduleStore(ScheduleStore::defaultPath(), this);
    watchStore(m_schedules);
    m_schedules->load();
    m_scheduler = new Scheduler(m_schedules, this);

    m_reports = std::make_unique<AnalysisStore>(AnalysisStore::defaultDirectory());

    m_sets = new FileSetStore(FileSetStore::defaultPath(), this);
    watchStore(m_sets);
    m_sets->load();
    m_preferences = new Preferences(Preferences::defaultPath(), this);
    watchStore(m_preferences);

    // What the window is painted in, from the last time somebody chose. Applied
    // here rather than in the constructor because that is where the preferences
    // are, and before the engine loads a line of QML, so the first frame is
    // already the right colour. An absent or unknown value opens on the default.
    m_colour->setTheme(m_preferences->value(QStringLiteral("ui.theme"), Palette::defaultTheme()).toString());

    // Built here, where the preferences it reads and writes already exist, and
    // started later -- see startUpdateCheck(). Building it sends nothing.
    m_updateCheck = new UpdateCheck(m_preferences, QStringLiteral(MOLE_VERSION), this);
    // The page is kept here rather than handed to the window: what the window
    // needs is a version to name, and openReleasePage() is the only thing that
    // ever opens a URL that arrived over the network.
    connect(
        m_updateCheck, &UpdateCheck::newVersionFound, this, [this](const QString& version, const QUrl& page) {
            m_releasePage = page;
            emit versionAvailable(version);
        });

    m_secrets = new SecretStore(SecretStore::defaultPath(), this);
    connect(m_secrets, &SecretStore::unlockedChanged, this, &AppController::credentialsChanged);

    m_remotes = new RemoteRegistry(RemoteRegistry::defaultPath(), m_secrets, this);
    watchStore(m_remotes);
    m_remotes->load();

    m_alerts = new AlertStore(AlertStore::defaultPath(), this);
    watchStore(m_alerts);
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
    m_services.metadata = m_metadata;
    m_services.thumbnails = m_thumbnails;
    m_services.scheduler = m_scheduler;
    m_services.alerts = m_alerts;
    m_services.reports = m_reports.get();
    m_services.sets = m_sets;
    m_services.preferences = m_preferences;

    // What the interface knows about the index, so that nothing on this thread
    // has to ask the database -- see ADR-0066. Created after the task manager
    // and the bus, because it needs both: one to read on, one to be told when
    // the index has changed under it.
    m_indexSummary = new IndexSummary(m_index.get(), m_taskManager, m_events, this);
    m_services.indexSummary = m_indexSummary;
    // From here on a read of the index from this thread is a fault and says so.
    // Set after open(), which legitimately runs here.
    m_index->doNotQueryFrom(QThread::currentThread());
    // And the same for a drive, which is the other thing the interface must not
    // touch: ARCHITECTURE.md's first rule, and seven places were breaking it.
    // See IFileSystem::doNotCallFrom() and MOLE-360.
    IFileSystem::doNotCallFrom(QThread::currentThread());
    m_indexSummary->refresh();

    // The mount table is the source of truth; the bus just broadcasts it so
    // views do not have to know about VfsManager.
    connect(m_vfs, &VfsManager::mountsChanged, this, [this] { m_events->postMountsChanged(); });
    // And the question that comes back the other way: somewhere in the window is
    // pointed at a drive that is not mounted, and connecting one is this object's
    // job rather than a pane's.
    connect(m_events, &EventBus::driveNeeded, this, &AppController::arrangeDriveFor);

    PluginManager::Destinations destinations;
    destinations.vfs = m_vfs;
    destinations.features = m_features;
    destinations.previews = m_previews;
    destinations.metadata = m_metadata;
    destinations.thumbnails = m_thumbnails;
    destinations.actions = m_actions;

    m_plugins = new PluginManager(m_services, destinations, this);

    for (auto& plugin : builtIns)
        m_plugins->addBuiltIn(std::move(plugin));
    m_plugins->loadFromDefaultPaths();

    // Handed the sets store, because a set bookmark's name and its liveness are
    // read from it rather than copied. The store exists by now -- see above. See
    // ADR-0061.
    m_bookmarks = new BookmarkModel(BookmarkModel::defaultFilePath(), m_sets, this);
    // The bookmarks are a list model and cannot derive from JsonFileStore, so
    // they carry the same signal by hand. The loading happens in their
    // constructor, which is before anything could hear about damage -- a
    // bookmarks file that could not be read is in the session log and will not
    // be written over, which is the half that matters.
    connect(m_bookmarks, &BookmarkModel::saveFailed, this, [this](const QString& reason) {
        emit notification(static_cast<int>(EventBus::Severity::Warning), QStringLiteral("Not saved"), reason);
    });
    connect(m_bookmarks, &BookmarkModel::countChanged, this, &AppController::refreshBookmarkActions);
    // A set bookmark's name comes from the store, so renaming a set changes what
    // the Bookmarks menu should say without any bookmark being added or removed.
    connect(m_bookmarks, &BookmarkModel::dataChanged, this, &AppController::refreshBookmarkActions);

    // Before the palette, which is handed a pointer to it. Built after, the palette
    // held a null one for the lifetime of the application and quietly offered no
    // drives at all -- everything else about it worked, which is why nobody noticed.
    // The preference is what makes a hidden drive still hidden tomorrow, and it
    // is the same store the theme choice lives in. See MOLE-311.
    m_drives = new DriveListModel(m_vfs, m_remotes, m_taskManager, m_preferences, this);

    // The dialog lists what somebody set up; the sidebar lists everything. One
    // model underneath both, filtered rather than rebuilt, so there is a single
    // answer to what state a drive is in.
    m_configuredDrives = new QSortFilterProxyModel(this);
    m_configuredDrives->setSourceModel(m_drives);
    m_configuredDrives->setFilterRole(DriveListModel::ConfiguredIdRole);
    // Any non-empty id: a mount nobody configured has none.
    m_configuredDrives->setFilterRegularExpression(QRegularExpression(QStringLiteral(".+")));

    // What the *sidebar* draws, which is no longer everything: a drive somebody
    // has taken out of the list is still configured, still mounted and still
    // reachable, and simply not on the strip. The dialog goes on showing the whole
    // model, because a list you cannot see is a list you cannot tick. See
    // MOLE-311.
    //
    // Filtered on the role rather than rebuilt, the way the configured list above
    // is, so every row in either place is the same answer about the same drive.
    m_sidebarDrives = new QSortFilterProxyModel(this);
    m_sidebarDrives->setSourceModel(m_drives);
    m_sidebarDrives->setFilterRole(DriveListModel::ShownRole);
    m_sidebarDrives->setFilterRegularExpression(QRegularExpression(QStringLiteral("^true$")));

    // The palette knows nothing about tabs or navigation: it says what was chosen
    // and the shell does it, which is why the model can be a plain view over the
    // registries.
    m_commands = new CommandPaletteModel(m_actions, m_bookmarks, m_drives, this);
    connect(m_commands, &CommandPaletteModel::actionRequested, this, &AppController::triggerAction);
    connect(m_commands, &CommandPaletteModel::locationRequested, this, &AppController::goTo);
    connect(m_commands, &CommandPaletteModel::bookmarkRequested, this, &AppController::openPlace);
    // The same calls the sidebar buttons make, so the two cannot drift into
    // doing subtly different things to the same drive.
    connect(m_commands, &CommandPaletteModel::driveCommandRequested, this,
        [this](CommandPaletteModel::DriveCommand what, const QString& driveId) {
            switch (what) {
            case CommandPaletteModel::DriveCommand::Connect:
                connectDrive(driveId);
                return;
            case CommandPaletteModel::DriveCommand::Eject:
                disconnectDrive(driveId);
                return;
            case CommandPaletteModel::DriveCommand::Check:
                checkDrive(driveId);
                return;
            case CommandPaletteModel::DriveCommand::Unlock:
                emit credentialsRequested();
                return;
            }
        });

    m_launcher = new FileLauncher(m_services, this);
    connect(m_launcher, &FileLauncher::failed, this, [this](const QString& uri, const QString& reason) {
        emit notification(static_cast<int>(EventBus::Severity::Warning),
            QStringLiteral("Cannot open %1").arg(VfsUri::fromString(uri).fileName()), reason);
        if (m_drives) {
            m_drives->noteFailureAt(VfsUri::fromString(uri), VfsError::make(VfsError::IoError, reason));
        }
    });

    // A drag has no result to look at afterwards, so both outcomes it can have
    // besides working are said out loud. Silence would be indistinguishable from
    // a pointer that missed.
    m_dragSource = new DragSource(m_services, this);
    connect(m_dragSource, &DragSource::refused, this, [this](const QString& reason) {
        emit notification(
            static_cast<int>(EventBus::Severity::Warning), QStringLiteral("Nothing was dragged"), reason);
    });
    connect(m_dragSource, &DragSource::leftBehind, this, [this](int sent, int left) {
        emit notification(static_cast<int>(EventBus::Severity::Warning),
            QStringLiteral("Dragging %1 of %2").arg(sent).arg(sent + left),
            QStringLiteral("No drive is mounted for the other %1, so there is nothing to fetch them "
                           "from.")
                .arg(left));
    });
    // Not a failure: the drag will work, a moment from now. Said in the same place
    // as the rest, because a gesture that appears to have done nothing is the
    // thing this sentence exists to prevent.
    connect(m_dragSource, &DragSource::staging, this, [this](int count) {
        emit notification(static_cast<int>(EventBus::Severity::Info),
            count == 1 ? QStringLiteral("Fetching 1 file") : QStringLiteral("Fetching %1 files").arg(count),
            QStringLiteral("These are not on this computer yet. The drag will work once they are "
                           "here -- the task strip shows how far it has got."));
    });

    // Whatever failed, wherever it was tried from. A listing that came back
    // with an error is the plainest evidence there is that a drive is not
    // answering, and it happens in a tab that has never heard of the sidebar.
    connect(m_events, &EventBus::operationFailed, this, [this](const VfsUri& target, const VfsError& error) {
        if (m_drives)
            m_drives->noteFailureAt(target, error);
    });

    // A plugin's voice. `EventBus::postNotification()` was declared for exactly
    // this from the start and nothing was listening, so anything a feature said
    // through the bus went nowhere -- a view has no toast of its own and the
    // shell owns the one there is. See MOLE-205.
    connect(m_events, &EventBus::notificationPosted, this,
        [this](EventBus::Severity severity, const QString& title, const QString& detail) {
            emit notification(static_cast<int>(severity), title, detail);
        });

    m_taskModel = new TaskListModel(m_taskManager, this);
    m_terminal = new TerminalController(this);
    m_tabs = new TabsModel(m_features, this);
    // What Ctrl+D is about -- a folder or a set -- depends on which tab is in
    // front of you, so the two entries that act on where you are are rebuilt when
    // that changes. See MOLE-209.
    connect(m_tabs, &TabsModel::currentIndexChanged, this, &AppController::refreshBookmarkActions);
    // What a drive offers belongs to the row the cursor is on, so the entries
    // follow the pane rather than waiting for somebody to open the menu: a
    // shortcut and the command palette reach an action by id, and an entry that
    // exists only once the menu has been built is one neither can find.
    connect(m_tabs, &TabsModel::currentIndexChanged, this, &AppController::watchActivePane);
    connect(m_tabs, &TabsModel::rowsInserted, this, &AppController::watchActivePane);

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
    // Before the scheduler, and the comment on recordStartup() says why: start()
    // runs checkDue() synchronously, and a due index rule would have a scan holding
    // the index's mutex by the time the block below tried to read it.
    recordStartup();

    m_scheduler->start();

    // Debounced: a tab reports state changes on every navigation, and the
    // session file is not worth rewriting for each keystroke.
    m_session = std::make_unique<SessionStore>(SessionStore::defaultFilePath());
    m_sessionSaveTimer = new QTimer(this);
    m_sessionSaveTimer->setSingleShot(true);
    m_sessionSaveTimer->setInterval(1500);
    connect(m_sessionSaveTimer, &QTimer::timeout, this, &AppController::saveSessionNow);
    connect(m_tabs, &TabsModel::sessionDirty, this, [this] {
        if (!m_restoring) {
            m_sessionTouched = true;
            m_sessionSaveTimer->start();
        }
        // The same signal answers a second question: a tab that moved may have
        // moved onto or off a drive, and the sidebar's dot says which drives are
        // in use. Nothing is polled and no drive is contacted -- see
        // DriveListModel::noteOpenLocations().
        refreshOpenDrives();
        pruneBrowsingMounts();
    });
    // Opening and closing tabs changes the answer too, and neither goes through
    // a controller's own signal.
    connect(m_tabs, &TabsModel::countChanged, this, &AppController::refreshOpenDrives);
    // A tab closing is how the last reader leaves an archive, which is the moment
    // an unlisted mount has nobody left inside it.
    connect(m_tabs, &TabsModel::countChanged, this, &AppController::pruneBrowsingMounts);
    connect(m_vfs, &VfsManager::mountsChanged, this, &AppController::refreshOpenDrives);
    // A drive that appears an hour later is the same fact arriving late, and a
    // report about "the drive was there a minute ago" needs it. Named by its root
    // rather than by its scheme: the log is read when something has gone wrong,
    // and "sftp" does not say which of two servers it was. See ADR-0012's
    // amendment of 2026-09-04, which settles that a log line may carry a uri and
    // that credentials are what stays out.
    connect(m_vfs, &VfsManager::mountAdded, this, [this](const QString& id) {
        const Mount mount = m_vfs->mount(id);
        if (mount.isValid() && !mount.internal && !mount.unlisted) {
            qInfo("Drive added: %s (%s)", qPrintable(mount.displayName), qPrintable(mount.root.toString()));
        }
    });
    connect(m_vfs, &VfsManager::mountRemoved, this,
        [](const QString& id) { qInfo("Drive removed: %s", qPrintable(id)); });

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

void AppController::startUpdateCheck()
{
    if (!m_updateCheck)
        return;
    // False when nothing was sent -- switched off, or a notice still inside its
    // week -- and there is nothing to do about that: it is the ordinary answer.
    m_updateCheck->start();
}

void AppController::setLinkHook(LinkHook hook)
{
    m_linkHook = std::move(hook);
}

bool AppController::openReleasePage()
{
    if (!m_releasePage.isValid() || m_releasePage.isEmpty())
        return false;
    if (m_linkHook)
        return m_linkHook(m_releasePage);
    return QDesktopServices::openUrl(m_releasePage);
}

void AppController::mountDefaultDrives()
{
    IFileSystemFactory* factory = m_vfs->factoryFor(QStringLiteral("file"));
    if (!factory)
        return;

    const auto mountLocal = [&](const QString& displayName, const QString& path, SpaceInfo declared = {}) {
        QString error;
        FileSystemPtr fs = factory->create({}, &error);
        if (!fs)
            return;

        Mount mount;
        mount.displayName = displayName;
        mount.root = VfsUri::fromLocalPath(path);
        mount.fileSystem = std::move(fs);
        mount.declaredSpace = declared;
        m_vfs->addMount(std::move(mount));
    };

    // A fixed list, when one is given, instead of whatever this machine has
    // mounted. Written as "Name=/path;Other=/path", and a path may be followed by
    // "|free:total" to say what the drive should report having rather than what is
    // measured underneath it.
    //
    // For the screenshots, and for any test that photographs the window. The
    // sidebar otherwise lists the volumes of whoever ran it, by their own names
    // and with their own capacities -- which then went into a public repository
    // as an illustration of what the application looks like. A picture of the
    // fixture says the same thing about the software and nothing about a desk.
    //
    // The capacities are why the sizes are here too. Fixing the names left the
    // figures coming from the real disk, so "135,81 GiB free" became "135,80 GiB
    // free" between one regeneration and the next and thirty-nine of the guide's
    // fifty-three pictures changed with nothing else different -- which made
    // `make guide-images` unreviewable. See MOLE-255.
    const QByteArray fixed = qgetenv("MOLE_DRIVES");
    if (!fixed.isEmpty()) {
        const QStringList entries = QString::fromLocal8Bit(fixed).split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& entry : entries) {
            const qsizetype split = entry.indexOf(QLatin1Char('='));
            if (split <= 0)
                continue;
            QString path = entry.mid(split + 1);
            SpaceInfo declared;
            // Only the last "|free:total" counts, and only when it is exactly
            // two numbers -- a bar is a legal character in a path, and a path
            // that happens to end in one keeps it.
            const qsizetype bar = path.lastIndexOf(QLatin1Char('|'));
            if (bar > 0) {
                static const QRegularExpression sizes(QStringLiteral("^(\\d+):(\\d+)$"));
                const QRegularExpressionMatch match = sizes.match(path.mid(bar + 1));
                if (match.hasMatch()) {
                    declared.freeBytes = match.captured(1).toLongLong();
                    declared.totalBytes = match.captured(2).toLongLong();
                    path.truncate(bar);
                }
            }
            mountLocal(entry.left(split), path, declared);
        }
        return;
    }

    // Home first: it is where the user actually keeps things, whatever the
    // partition layout underneath happens to be.
    const QString home = QDir::homePath();
    mountLocal(QStringLiteral("Home"), home);

    // Then whatever the operating system has mounted -- the real disks, USB
    // sticks and network shares, not the sixty pseudo filesystems next to them.
    //
    // Asked here, on the startup path, and that is now safe where it was not:
    // enumerate() reads the mount table as text and touches none of the
    // filesystems in it. It used to construct a QStorageInfo per entry -- a
    // statvfs apiece, including on the mount that had stopped answering -- so
    // one stale share meant Mole starting with no window and no message for as
    // long as the kernel took. Twenty-four lines above recordStartup(), whose
    // comment says a synchronous storage read does not belong here. See
    // MOLE-361.
    //
    // A task was tried instead and taken out again: mounting the drives as the
    // answer arrived left eleven suites failing or timing out, because a drive
    // list that is empty for the first instant of a run is a different
    // application from the one they were written against -- and the benefit
    // would only reach the platforms without a mount table, which are the ones
    // this checkout cannot measure. TODO.md records that half.
    for (const SystemVolume& volume : SystemVolumes::enumerate()) {
        // The volume carrying home is in that list now, and it was not before:
        // on a machine where /home is its own dataset it is a real disk holding
        // everything the user owns. It is the same place as the row above, so it
        // gets one row, under the name people look for rather than the name the
        // dataset happens to have.
        if (volume.rootPath == home)
            continue;
        mountLocal(volume.name, volume.rootPath);
    }
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

void AppController::unlockCredentials(const QString& passphrase)
{
    if (!m_secrets || m_credentialsBusy)
        return;

    m_credentialsError.clear();
    m_credentialsBusy = true;
    emit credentialsChanged();

    // On a task, because scrypt at this cost is a noticeable fraction of a
    // second and this used to be a button handler on the thread that draws --
    // the window stopped, with nothing saying it was working. The store takes
    // its own lock, so the window is free to go on asking it questions in the
    // meantime. See ADR-0090.
    SecretStore* secrets = m_secrets;
    const bool creating = !secrets->exists();

    auto* watcher = new QFutureWatcher<QPair<bool, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<bool, QString>>::finished, this, [this, watcher] {
        const QPair<bool, QString> answer = watcher->result();
        watcher->deleteLater();
        m_credentialsBusy = false;
        m_credentialsError = answer.second;
        finishUnlock(answer.first);
    });
    watcher->setFuture(QtConcurrent::run([secrets, passphrase, creating] {
        QString error;
        const bool ok = creating ? secrets->create(passphrase, &error) : secrets->unlock(passphrase, &error);
        return QPair<bool, QString> { ok, error };
    }));
}

void AppController::finishUnlock(bool ok)
{
    if (!ok) {
        emit credentialsChanged();
        emit drivesChanged();
        emit credentialsAttempted(false);
        return;
    }

    // Everything that was waiting for a password can connect now. Typing the
    // passphrase once and having the drives somebody marked "connect at startup"
    // come up is what that setting means.
    for (const RemoteDrive& drive : m_remotes->drives()) {
        if (drive.mountAtStartup && !drive.secretFields.isEmpty())
            connectDrive(drive.id);
    }

    emit credentialsChanged();
    emit drivesChanged();

    // A tab restored onto a drive whose secret was in the store asked for it while
    // the store was shut, and now it can be built. Nothing is navigated: the pane
    // kept where it meant to be and comes back on the mount appearing, which is
    // also what brings a second pane on the same drive back.
    for (const QString& wanted : std::exchange(m_drivesWanted, QStringList()))
        prepareDriveFor(wanted);

    // And whoever opened a drive to get here is taken there, which is the whole
    // point of asking at that moment rather than at startup.
    if (!m_pendingNavigation.isEmpty()) {
        const QString waiting = std::exchange(m_pendingNavigation, QString());
        goTo(waiting);
    }
    emit credentialsAttempted(true);
}

QVariantList AppController::driveKinds() const
{
    QVariantList out;
    if (!m_vfs)
        return out;

    for (IFileSystemFactory* factory : m_vfs->factories()) {
        // A kind of drive this platform does not have is left out entirely
        // rather than greyed out. The greyed-out row is for a library somebody
        // could install; there is nothing to install for a Windows share on
        // Windows, where \\server\share is served by the local filesystem, and
        // a row saying otherwise sends people looking for a package.
        if (!factory->isApplicable())
            continue;

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

QAbstractItemModel* AppController::configuredDrives() const
{
    return m_configuredDrives;
}

QAbstractItemModel* AppController::sidebarDrives() const
{
    return m_sidebarDrives;
}

QVariantMap AppController::driveConfiguration(const QString& id) const
{
    if (!m_remotes)
        return {};
    const RemoteDrive drive = m_remotes->drive(id);
    if (!drive.isValid())
        return {};

    // Deliberately no "connected" and no "needsUnlock". What state a drive is
    // in has one answer and it is the drive model's; a second copy here is how
    // the two come to disagree.
    return QVariantMap { { QStringLiteral("id"), drive.id }, { QStringLiteral("name"), drive.name },
        { QStringLiteral("factory"), drive.factoryScheme }, { QStringLiteral("variant"), drive.variant },
        { QStringLiteral("root"), drive.root }, { QStringLiteral("uri"), drive.rootUri().toString() },
        { QStringLiteral("settings"), drive.settings },
        { QStringLiteral("secretFields"), drive.secretFields } };
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
    QSet<QString> knownKeys;
    const QVariantList fields = driveFields(factoryScheme, variant);
    for (const QVariant& value : fields) {
        const QVariantMap field = value.toMap();
        const QString key = field.value(QStringLiteral("key")).toString();
        knownKeys.insert(key);
        if (field.value(QStringLiteral("secret")).toBool())
            secretKeys.insert(key);
    }

    QVariantMap settings;
    QVariantMap secrets;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        if (it.value().toString().isEmpty())
            continue;
        if (secretKeys.contains(it.key())) {
            secrets.insert(it.key(), it.value());
            continue;
        }
        // A value the backend never asked for is dropped rather than written.
        // Anything else means a caller can put a key of its own choosing into a
        // file meant to be readable -- and if that value happened to be a
        // password, it would sit there in the clear precisely because no field
        // declared it secret. The safe reading of an unrecognised key is that it
        // does not belong in the settings at all.
        if (!knownKeys.contains(it.key()))
            continue;
        settings.insert(it.key(), it.value());
    }
    drive.settings = settings;

    m_credentialsError.clear();
    QString storedId;
    if (!m_remotes->put(drive, secrets, &m_credentialsError, &storedId)) {
        emit notification(static_cast<int>(EventBus::Severity::Warning),
            QStringLiteral("Could not save the drive"), m_credentialsError);
        return false;
    }
    emit drivesChanged();

    // Verified here, where it was typed. Nothing in saving a drive tests it, and
    // nothing in connecting one does either -- so a wrong endpoint or a refused
    // password used to surface much later, as an error about a certificate in the
    // middle of browsing. The check runs in the background because it does real
    // I/O; the drive is saved either way, so a failure costs nothing that was
    // typed.
    checkDrive(storedId);
    return true;
}

void AppController::checkDrive(const QString& id)
{
    if (!m_remotes || !m_vfs || !m_taskManager)
        return;

    const RemoteDrive drive = m_remotes->drive(id);
    if (!drive.isValid())
        return;

    const auto report = [this, id, name = drive.name](bool reachable, const QString& message) {
        if (m_drives)
            m_drives->noteCheckResult(id, reachable, message);
        emit driveChecked(id, reachable, message);
        emit notification(
            static_cast<int>(reachable ? EventBus::Severity::Info : EventBus::Severity::Warning),
            reachable ? QStringLiteral("%1 is reachable").arg(name)
                      : QStringLiteral("%1 cannot be reached").arg(name),
            message);
    };

    QString error;
    const QVariantMap config = m_remotes->configFor(drive, &error);
    if (config.isEmpty()) {
        report(false, error.isEmpty() ? QStringLiteral("This drive has no configuration") : error);
        return;
    }

    IFileSystemFactory* factory = nullptr;
    for (IFileSystemFactory* candidate : m_vfs->factories()) {
        if (candidate->scheme() == drive.factoryScheme) {
            factory = candidate;
            break;
        }
    }
    if (!factory) {
        report(false, QStringLiteral("Nothing here can serve a %1 drive").arg(drive.factoryScheme));
        return;
    }

    // Built on this thread, which performs no I/O; only the listing does, and that
    // happens inside the task.
    FileSystemPtr fs = factory->create(config, &error);
    if (!fs) {
        report(false, error.isEmpty() ? QStringLiteral("That configuration is not usable") : error);
        return;
    }

    // Not marked background: it is short, it was asked for by saving, and seeing
    // it in the task strip is part of the point.
    auto* task = new DriveCheckTask(drive.name, std::move(fs), drive.rootUri());
    connect(task, &DriveCheckTask::checked, this,
        [report](bool reachable, const QString& message) { report(reachable, message); });
    m_taskManager->submit(task);
}

void AppController::sweepDrive(const QString& id, bool discard, int olderThanHours)
{
    if (!m_remotes || !m_vfs || !m_taskManager)
        return;

    const RemoteDrive drive = m_remotes->drive(id);
    if (!drive.isValid())
        return;

    const auto report = [this, id, discard](int found, const QString& message) {
        emit driveSwept(id, found, discard, message);
        emit notification(static_cast<int>(EventBus::Severity::Info),
            discard ? QStringLiteral("Cleared up") : QStringLiteral("Looked over"), message);
    };

    QString error;
    const QVariantMap config = m_remotes->configFor(drive, &error);
    if (config.isEmpty()) {
        report(0, error.isEmpty() ? QStringLiteral("This drive has no configuration") : error);
        return;
    }

    IFileSystemFactory* factory = nullptr;
    for (IFileSystemFactory* candidate : m_vfs->factories()) {
        if (candidate->scheme() == drive.factoryScheme) {
            factory = candidate;
            break;
        }
    }
    if (!factory) {
        report(0, QStringLiteral("Nothing here can serve a %1 drive").arg(drive.factoryScheme));
        return;
    }

    FileSystemPtr fs = factory->create(config, &error);
    if (!fs) {
        report(0, error.isEmpty() ? QStringLiteral("That configuration is not usable") : error);
        return;
    }

    auto* task = new SweepLeftoversTask(
        drive.name, std::move(fs), std::chrono::hours(qMax(0, olderThanHours)), discard);
    connect(task, &Task::finished, this, [this, task, report] {
        if (task->state() == Task::State::Failed)
            report(0, task->error().message);
        else
            report(static_cast<int>(task->found().size()), task->summary());
    });
    m_taskManager->submit(task);
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
    // Before the mount, not after. Mounted is not answered: building a backend
    // performs no I/O, so a drive pointed at a host that has been switched off
    // gets this far exactly as successfully as one that works. Marking the
    // question as out first means the row never reads Connected on its way to
    // reading Unreachable -- not even for the moment between two statements.
    if (m_drives)
        m_drives->noteCheckStarted(drive.id);

    m_vfs->addMount(mount);

    emit credentialsChanged();
    emit drivesChanged();

    checkDrive(drive.id);
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

int AppController::openStandingTab(const QString& featureId)
{
    const int existing = m_tabs->rowOfFeature(featureId);
    if (existing >= 0) {
        m_tabs->setCurrentIndex(existing);
        return existing;
    }
    return openFeatureTab(featureId);
}

int AppController::browserTabForCurrent()
{
    // The browser this tab has already opened, switched to; otherwise a new one.
    //
    // This is what makes examining a search's results leave one tab behind
    // rather than one per result. The search tab itself is never touched, so
    // its results, its narrowing and where it was scrolled to are all still
    // there to come back to.
    const int existing = m_tabs->rowOpenedFromCurrent(QStringLiteral("mole.browser"));
    if (existing >= 0) {
        m_tabs->setCurrentIndex(existing);
        return existing;
    }
    return m_tabs->openTab(QStringLiteral("mole.browser"));
}

int AppController::openSearchEverywhere()
{
    const int row = openFeatureTab(QStringLiteral("mole.livesearch"));
    if (row < 0)
        return row;
    // By property, not by type: the shell has never had to know what a search
    // is, and one preset is not a reason to start.
    if (QObject* controller = m_tabs->controllerAt(row))
        controller->setProperty("everywhere", true);
    return row;
}

void AppController::openLocation(const QString& uri)
{
    // A uri into an archive whose mount has gone -- a bookmark, a back step, a
    // session restored tomorrow. Rebuilt from the uri itself, which is what makes
    // an unlisted mount's disappearance safe rather than a trap. A no-op for
    // every other uri, and for one that still has its mount.
    m_vfs->remountFor(VfsUri::fromString(uri));

    const int row = browserTabForCurrent();
    if (row < 0)
        return;
    if (auto* controller = m_tabs->controllerAt(row))
        QMetaObject::invokeMethod(controller, "navigateActive", Q_ARG(QString, uri));
}

void AppController::revealFile(const QString& fileUri)
{
    // A browser tab, because that is what can show a folder. From one already
    // open it is that tab's pane that moves: the user is looking at a folder
    // and has asked to look at a different one.
    QObject* current = m_tabs->currentController();
    QObject* pane = current ? current->property("activePane").value<QObject*>() : nullptr;
    if (!pane) {
        // From anywhere else -- a search, above all -- the one browser this tab
        // opens for, reused. The shape previewFile() has always had.
        const int row = browserTabForCurrent();
        if (row < 0)
            return;
        QObject* opened = m_tabs->controllerAt(row);
        pane = opened ? opened->property("activePane").value<QObject*>() : nullptr;
    }
    if (!pane)
        return;

    QMetaObject::invokeMethod(pane, "revealFile", Q_ARG(QString, fileUri));
}

AppController::DriveReadiness AppController::prepareDriveFor(const QString& uri)
{
    if (!m_remotes || !m_vfs)
        return DriveReadiness::Ready;

    const VfsUri target = VfsUri::fromString(uri);
    if (!target.isValid())
        return DriveReadiness::Ready;

    // Already mounted, or somewhere that is not a configured drive at all -- a
    // local path, an archive, a bookmark on a disk. Nothing to arrange.
    if (m_vfs->resolve(target))
        return DriveReadiness::Ready;

    const RemoteDrive drive = m_remotes->driveForUri(target);
    if (!drive.isValid())
        return DriveReadiness::Ready;

    // A drive whose password is in the store cannot be built while the store is
    // shut. Asked for now rather than at startup, because now is the first moment
    // anybody has a reason to answer.
    if (m_remotes->needsUnlocking(drive))
        return DriveReadiness::Waiting;

    const QString error = connectDrive(drive.id);
    if (!error.isEmpty()) {
        // Said out loud. An unconnected drive navigated to in silence is a folder
        // with nothing in it, which reads as an empty drive.
        emit notification(static_cast<int>(EventBus::Severity::Warning),
            QStringLiteral("Cannot connect %1").arg(drive.name), error);
        return DriveReadiness::Failed;
    }
    return DriveReadiness::Ready;
}

void AppController::arrangeDriveFor(const VfsUri& target)
{
    const QString uri = target.toString();
    switch (prepareDriveFor(uri)) {
    case DriveReadiness::Ready:
        // Connected, or there was never a configured drive here -- a mount that
        // has gone for good, a folder deleted. Either way whoever asked finds out
        // from the mount table.
        break;
    case DriveReadiness::Waiting:
        // Asked for once, however many tabs are waiting on however many drives:
        // the store is one store, and every drive behind it comes up together.
        if (!m_drivesWanted.contains(uri))
            m_drivesWanted.append(uri);
        emit credentialsRequested();
        break;
    case DriveReadiness::Failed:
        // prepareDriveFor() has already said so out loud.
        break;
    }
}

bool AppController::goTo(const QString& uri)
{
    // Opening a drive is what connects it. Everything arrives here -- a sidebar
    // row, the command palette, a bookmark -- so this is the one place that has
    // to know, rather than each of them.
    switch (prepareDriveFor(uri)) {
    case DriveReadiness::Ready:
        break;
    case DriveReadiness::Waiting:
        m_pendingNavigation = uri;
        emit credentialsRequested();
        return false;
    case DriveReadiness::Failed:
        return false;
    }

    QObject* current = m_tabs->currentController();
    // Reuse the current tab when it knows how to navigate; otherwise the user
    // asked to go somewhere a search tab cannot take them, so open a browser.
    if (current && current->metaObject()->indexOfMethod("navigateActive(QString)") >= 0) {
        QMetaObject::invokeMethod(current, "navigateActive", Q_ARG(QString, uri));
        return true;
    }
    openLocation(uri);
    return true;
}

bool AppController::openPlace(const QString& kind, const QString& target)
{
    if (target.isEmpty())
        return false;
    if (kind != QLatin1String("set"))
        return goTo(target);

    const FileSet set = m_sets ? m_sets->set(target) : FileSet {};
    if (!set.isValid()) {
        // Said out loud, the way a navigation that cannot happen is. Opening the
        // Sets tab with nothing selected would look like the bookmark worked.
        emit notification(static_cast<int>(EventBus::Severity::Warning), QStringLiteral("That set is gone"),
            QStringLiteral("The bookmark still remembers it, so you can remove it when you are sure."));
        return false;
    }

    // One Sets tab, whatever the current tab is. currentSetId is already
    // writable and already saved with the session, so pointing a tab at a set
    // needs nothing new in the feature.
    const int row = openStandingTab(QStringLiteral("core.filesets"));
    if (row < 0)
        return false;
    if (QObject* controller = m_tabs->controllerAt(row))
        controller->setProperty("currentSetId", target);
    return true;
}

void AppController::recordStartup() const
{
    // One line per subject rather than a paragraph per drive: this block is read by
    // somebody scrolling past it to the interesting part.
    qInfo("Mole %s, Qt %s, %s", qPrintable(QCoreApplication::applicationVersion()), qVersion(),
        qPrintable(QSysInfo::productType()));

    const QStringList loaded = pluginSummary();
    const QStringList failed = pluginErrors();
    qInfo("Plugins: %lld loaded, %lld failed", static_cast<long long>(loaded.size()),
        static_cast<long long>(failed.size()));
    // Named individually, because "1 failed" without saying which is the report
    // arriving without the one fact it was sent for.
    //
    // At warning, not info. `mole --plugins` exits 1 when this list is not
    // empty, so the binary itself counts these as failures -- and ADR-0012 says a
    // failure is audible at warning. A line the default configuration hides is a
    // line nobody sees in the report they send. See MOLE-365.
    for (const QString& problem : failed)
        qWarning("Plugin problem: %s", qPrintable(problem));

    // And the things that are nobody's fault, kept apart from them: a host with
    // no registry for a kind of contribution is not a plugin misbehaving.
    if (m_plugins) {
        for (const QString& note : m_plugins->notes())
            qInfo("Plugin note: %s", qPrintable(note));
    }

    if (m_drives) {
        QStringList described;
        for (int row = 0; row < m_drives->rowCount(); ++row) {
            const QModelIndex at = m_drives->index(row, 0);
            const QString name = at.data(DriveListModel::DisplayNameRole).toString();
            // The root rather than the scheme, for the reason the mountAdded line
            // above gives: which drive it was is the fact a report needs.
            const QString root = at.data(DriveListModel::RootUriRole).toString();
            const QString state = at.data(DriveListModel::StateTextRole).toString();
            // Space is left out on purpose: it arrives from QuerySpaceTask, which
            // has not run yet at this point, and asking the filesystem here would
            // put a synchronous storage read on the startup path -- the shape of
            // fault MOLE-264 is about. A drive reading "Not connected" or "Locked"
            // is the operational fact wanted anyway.
            described.append(QStringLiteral("%1 (%2) %3").arg(name, root, state));
        }
        qInfo("Drives: %s",
            qPrintable(described.isEmpty() ? QStringLiteral("none") : described.join(QStringLiteral(" | "))));
    }

    // The indexes come from the snapshot, not the database -- the same reason the
    // drives' space is left out six lines above, which named this fault while the
    // block below it went on committing it. See ADR-0066.
    //
    // Which means the answer is not here yet: the snapshot is read on a pool
    // thread and this runs during startup. So the line waits for the first
    // answer instead of being dropped -- what the run started with is the whole
    // point of the log, and "Indexes: none" would be a false version of it.
    if (m_indexSummary) {
        if (m_indexSummary->isKnown()) {
            recordIndexes();
        } else {
            connect(
                m_indexSummary, &IndexSummary::changed, this, [this] { recordIndexes(); },
                Qt::SingleShotConnection);
        }
    }
}

void AppController::recordIndexes() const
{
    QStringList described;
    for (const IndexVolume& volume : m_indexSummary->volumes()) {
        described.append(QStringLiteral("%1 %2 files, scanned %3")
                             .arg(volume.label)
                             .arg(volume.fileCount)
                             .arg(volume.lastScan.isValid() ? volume.lastScan.toString(Qt::ISODate)
                                                            : QStringLiteral("never")));
    }
    // Said either way. "Indexes: none" answers "why did my search find nothing",
    // which an absent line does not.
    qInfo("Indexes: %s",
        qPrintable(described.isEmpty() ? QStringLiteral("none") : described.join(QStringLiteral(" | "))));
}

QString AppController::mountForBrowsing(
    IFileSystemFactory& factory, const QString& localPath, QString* errorOut)
{
    FileSystemPtr inside = factory.create(factory.configForFile(localPath), errorOut);
    if (!inside)
        return {};

    Mount mount;
    mount.displayName = QFileInfo(localPath).fileName();
    mount.iconName = factory.iconName();
    mount.root = factory.rootUriForFile(localPath);
    mount.fileSystem = std::move(inside);
    // Somewhere to stand, and not a drive: see Mount::unlisted. Nobody asked for
    // a sidebar row per zip they looked inside, and eleven more suffixes made it
    // a row per package as well.
    mount.unlisted = true;
    return m_vfs->addMount(std::move(mount));
}

void AppController::dropUnusedBrowsingMounts(const QList<VfsUri>& open)
{
    // The other half of an unlisted mount, and the half that matters: a mount
    // nobody can see and nobody removes is worse than the row it replaced --
    // forty of them each holding a file handle and a cached central directory,
    // with no way to eject one.
    //
    // **Removed when the last reader leaves, which is not the same as "nobody is
    // inside it right now".** A mount is made *before* anything navigates into
    // it, and navigation is queued -- so a mount that has never been occupied is
    // one on its way to being occupied, and taking it away then is taking it away
    // between the two halves of opening an archive. That is exactly what happened
    // the first time this was written: openArchive() mounted, a tab appeared, the
    // tab's first report arrived before it had moved, and the mount was gone by
    // the time the navigation asked for it.
    //
    // So a mount has to have been stood in once before it can be left. Somebody
    // is inside it when any tab's open location shares its scheme and authority,
    // which covers a pane in a subfolder of the archive and a preview showing a
    // member -- a tab holding it open just as much as a browser is.
    QStringList gone;
    for (const Mount& mount : m_vfs->mounts()) {
        if (!mount.unlisted)
            continue;
        bool inUse = false;
        for (const VfsUri& where : open) {
            if (where.scheme() == mount.root.scheme() && where.authority() == mount.root.authority()) {
                inUse = true;
                break;
            }
        }
        if (inUse) {
            m_enteredMounts.insert(mount.id);
            continue;
        }
        if (m_enteredMounts.contains(mount.id)) {
            m_enteredMounts.remove(mount.id);
            gone.append(mount.id);
        }
    }
    // Collected first: removing a mount emits mountsChanged, and iterating the
    // list while something reacts to it is how a walk over a container ends up
    // reading a container that has moved.
    for (const QString& id : gone)
        m_vfs->removeMount(id);
}

void AppController::refreshOpenDrives()
{
    if (!m_tabs || !m_drives)
        return;

    // Every tab, not only the visible one: the question is about the drive, not
    // about the window. A feature that is not anywhere -- a report, a rename --
    // answers with nothing and costs nothing.
    m_drives->noteOpenLocations(openLocationsNow());
}

void AppController::pruneBrowsingMounts()
{
    if (!m_tabs)
        return;
    // **Not on mountsChanged**, which is the trigger refreshOpenDrives() uses.
    // Adding a mount does not change who is standing inside anything -- and
    // openArchive() adds one *before* navigating into it, so pruning on that
    // signal took the mount away between the two statements. Where the answer can
    // actually change is a tab moving and a tab appearing or going.
    dropUnusedBrowsingMounts(openLocationsNow());
}

QList<VfsUri> AppController::openLocationsNow() const
{
    QList<VfsUri> open;
    if (!m_tabs)
        return open;
    // Every tab, not only the visible one, for the reason refreshOpenDrives()
    // gives: the question is about the place, not about the window.
    for (int row = 0; row < m_tabs->rowCount(); ++row) {
        auto* controller = qobject_cast<FeatureController*>(m_tabs->controllerAt(row));
        if (!controller)
            continue;
        for (const QString& uri : controller->openLocations()) {
            const VfsUri parsed = VfsUri::fromString(uri);
            if (parsed.isValid())
                open.append(parsed);
        }
    }
    return open;
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
        // An internal mount is not that: it is somebody reading the file and it
        // will go away again, so it must not be handed over as a place to browse.
        for (const Mount& existing : m_vfs->mounts()) {
            if (existing.internal)
                continue;
            if (existing.root == root) {
                openLocation(root.toString());
                return root.toString();
            }
        }

        QString error;
        const QString id = mountForBrowsing(*factory, localPath, &error);
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
    // One entry per feature that opens from nothing, so a plugin that adds a tab
    // kind of that sort shows up here without touching the shell -- and one that
    // does not is not advertised as something to open from nothing. This used to
    // be one entry per feature, full stop, which offered a preview of no file, a
    // duplicates view whose own first line says "Open this from a folder to search
    // it", and a sync with neither endpoint set. See ADR-0032.
    //
    // Shortcut labels mirror the Shortcut items declared in QML; they are display
    // only.
    static const QHash<QString, QString> knownShortcuts {
        { QStringLiteral("mole.browser"), QStringLiteral("Ctrl+T") },
        { QStringLiteral("mole.commander"), QStringLiteral("Ctrl+Shift+T") },
        { QStringLiteral("mole.livesearch"), QStringLiteral("Ctrl+F") },
    };

    // In the order somebody reaches for them -- the two browsers, then the two
    // searches -- rather than in the order the plugins happened to load, which is
    // what the registry hands back. `sortOrder()` has always been documented as
    // the hint for this menu and was never read.
    QList<IFeature*> openable;
    for (IFeature* feature : m_features->features()) {
        if (feature->opensFromNothing())
            openable.append(feature);
    }
    std::sort(openable.begin(), openable.end(), [](IFeature* left, IFeature* right) {
        if (left->sortOrder() != right->sortOrder())
            return left->sortOrder() < right->sortOrder();
        return left->title() < right->title();
    });

    int order = 10;
    for (IFeature* feature : openable) {
        MenuAction action;
        action.id = QStringLiteral("mole.file.newTab.%1").arg(feature->id());
        action.section = MenuAction::Section::File;
        action.title = QStringLiteral("New %1 tab").arg(feature->title());
        action.iconText = feature->iconText();
        action.shortcut = knownShortcuts.value(feature->id());
        action.sortOrder = order;
        action.opensFeature = feature->id();
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
             LayoutEntry { "mole.view.gridView", "Grid of icons", 2 },
             LayoutEntry { "mole.view.gallery", "Gallery", 3 } }) {
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
        // Find in a preview, by name, so the feature does not depend on a reader
        // guessing that typing does something. The gesture is start-typing, the
        // way filtering a folder is -- Ctrl+F is a search of the folder in a new
        // tab and is a more valuable meaning for that key. See MOLE-308.
        MenuAction action;
        action.id = QStringLiteral("mole.view.findInPreview");
        action.section = MenuAction::Section::View;
        action.title = QStringLiteral("Find in this file");
        action.shortcut = QStringLiteral("type to find, or /");
        action.sortOrder = 26;
        // The bar belongs to whichever viewer is showing text, so the shell asks
        // and QML answers -- and the entry is offered whatever is on screen,
        // because a reader who asks for it and gets nothing has learnt something
        // about the tab they are on.
        action.trigger = [this] { emit dialogRequested(QStringLiteral("mole.view.findInPreview")); };
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
    // How the *window* is painted, rather than how the current tab looks. That
    // stretches ADR-0003's wording for `View` rather than contradicting it: this
    // is still the section anybody would look in, and a fifth top-level heading
    // is the thing ADR-0003 exists to prevent.
    //
    // Numbered from 100 because the numbers above already collide twice -- `Dual
    // pane` and `Show hidden files` are both 20, `Grid of icons` and `Refresh`
    // are both 30. Those two are not this group's to fix; adding a third would be.
    int themeOrder = 100;
    for (const QString& theme : Palette::themeNames()) {
        MenuAction action;
        action.id = QStringLiteral("mole.view.theme.%1").arg(theme.toLower());
        action.section = MenuAction::Section::View;
        action.title = theme;
        action.sortOrder = themeOrder;
        // Set apart from the entries about the tab, because they are about
        // something else.
        action.separatorBefore = themeOrder == 100;
        action.checked = [this, theme] { return m_colour->theme() == theme; };
        action.trigger = [this, theme] { setTheme(theme); };
        m_actions->addAction(std::move(action));
        themeOrder += 10;
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.view.refresh");
        action.section = MenuAction::Section::View;
        action.title = QStringLiteral("Refresh");
        // No shortcut of its own any more: Ctrl+R opens the command palette, where
        // this entry is one row like everything else.
        action.sortOrder = 30;
        action.enabled = [this] { return currentTabProperty("activePane").value<QObject*>() != nullptr; };
        action.trigger = [this] {
            if (QObject* pane = currentTabProperty("activePane").value<QObject*>())
                QMetaObject::invokeMethod(pane, "refresh");
        };
        m_actions->addAction(std::move(action));
    }

    // --- Copying a location -------------------------------------------
    //
    // Three separate entries rather than one that guesses. They copy three
    // different things, and a single action that sometimes copied the file and
    // sometimes the folder would be a coin toss with no way to see the result
    // before pasting it.
    {
        MenuAction action;
        action.id = QStringLiteral("mole.path.copyFolder");
        action.section = MenuAction::Section::Operations;
        action.title = QStringLiteral("Copy path of this folder");
        action.shortcut = QStringLiteral("Ctrl+Shift+C");
        action.sortOrder = 60;
        action.enabled = [this] { return !currentLocation().isEmpty(); };
        action.trigger = [this] { copyCurrentFolderPath(); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.path.copyFile");
        action.section = MenuAction::Section::Operations;
        action.title = QStringLiteral("Copy path of the selected file");
        // Ctrl+Shift+F rather than the Ctrl+Alt+C other file managers use: Ctrl+Alt
        // is AltGr on Polish and many other layouts, where AltGr+C is a letter
        // people type.
        action.shortcut = QStringLiteral("Ctrl+Shift+F");
        action.sortOrder = 61;
        // Off when a folder is under the cursor rather than quietly copying that
        // folder: the entry above is what does folders.
        action.enabled = [this] { return !currentFile().isEmpty(); };
        action.trigger = [this] { copySelectedFilePath(); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.path.copyDriveRoot");
        action.section = MenuAction::Section::Operations;
        action.title = QStringLiteral("Copy path of the drive");
        action.sortOrder = 62;
        action.enabled = [this] { return !currentLocation().isEmpty(); };
        action.trigger = [this] { copyDriveRootPath(); };
        m_actions->addAction(std::move(action));
    }

    // --- Bookmarks ----------------------------------------------------
    //
    // The saved places themselves are appended below, rebuilt whenever the
    // list changes, so they are reachable from the keyboard like anything else.
    refreshBookmarkActions();

    // --- Tools --------------------------------------------------------
    //
    // Where plugins land. Built-ins leave room above and below.
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.preview");
        action.opensFeature = QStringLiteral("mole.preview");
        action.section = MenuAction::Section::Operations;
        action.title = QStringLiteral("Preview this file");
        action.shortcut = QStringLiteral("F3");
        action.sortOrder = 10;
        action.enabled = [this] { return !currentFile().isEmpty(); };
        action.trigger = [this] { previewFile(currentFile()); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.analyse");
        action.opensFeature = QStringLiteral("mole.analysis");
        action.section = MenuAction::Section::Workflows;
        action.title = QStringLiteral("Analyse folder");
        action.shortcut = QStringLiteral("Ctrl+Shift+A");
        action.sortOrder = 10;
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
        action.section = MenuAction::Section::Operations;
        action.title = QStringLiteral("Terminal here");
        action.shortcut = QStringLiteral("Ctrl+`");
        action.sortOrder = 20;
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
        action.opensFeature = QStringLiteral("core.sync");
        action.section = MenuAction::Section::Workflows;
        action.title = QStringLiteral("Sync folders");
        action.sortOrder = 40;
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
        action.opensFeature = QStringLiteral("core.duplicates");
        action.section = MenuAction::Section::Workflows;
        action.title = QStringLiteral("Find duplicates");
        action.sortOrder = 20;
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
        action.opensFeature = QStringLiteral("core.bulkrename");
        action.section = MenuAction::Section::Workflows;
        action.title = QStringLiteral("Bulk rename");
        action.shortcut = QStringLiteral("Ctrl+Shift+R");
        action.sortOrder = 30;
        action.enabled = [this] { return !currentTargets().isEmpty(); };
        action.trigger = [this] { openFeatureTab(QStringLiteral("core.bulkrename")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.addToSet");
        action.opensFeature = QStringLiteral("core.filesets");
        action.section = MenuAction::Section::Operations;
        action.title = QStringLiteral("Add to set");
        action.shortcut = QStringLiteral("Ctrl+Shift+S");
        action.sortOrder = 30;
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
            openStandingTab(QStringLiteral("core.filesets"));
        };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        // openStandingTab() for this and the three below: each is a tab ADR-0032 calls
        // a standing tool, and openFeatureTab() always builds a new one -- so
        // triggering the entry twice left two tabs over one store, each with its own
        // idea of what was current. MOLE-206 built openStandingTab() and gave it to
        // the sets; this is the rest of the list, four tickets later. See MOLE-259.
        action.id = QStringLiteral("mole.tools.reports");
        action.opensFeature = QStringLiteral("core.reports");
        action.section = MenuAction::Section::Workflows;
        action.title = QStringLiteral("Saved reports");
        action.sortOrder = 50;
        action.enabled = [] { return true; };
        action.trigger = [this] { openStandingTab(QStringLiteral("core.reports")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.indexes");
        action.opensFeature = QStringLiteral("core.indexes");
        action.section = MenuAction::Section::Workflows;
        action.title = QStringLiteral("Indexes");
        action.sortOrder = 55;
        // Always available, for the same reason as the tracking list below: the
        // reason to open it is usually that a search answered from something
        // older than you thought, and nothing else points you at it.
        action.enabled = [] { return true; };
        action.trigger = [this] { openStandingTab(QStringLiteral("core.indexes")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.alerts");
        action.opensFeature = QStringLiteral("core.alerts");
        action.section = MenuAction::Section::Workflows;
        action.title = QStringLiteral("Alerts");
        action.shortcut = QStringLiteral("Ctrl+Shift+L");
        action.sortOrder = 60;
        action.enabled = [] { return true; };
        action.trigger = [this] { openStandingTab(QStringLiteral("core.alerts")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.automation");
        action.opensFeature = QStringLiteral("core.automation");
        action.section = MenuAction::Section::Workflows;
        action.title = QStringLiteral("Scheduled jobs");
        action.shortcut = QStringLiteral("Ctrl+Shift+J");
        action.sortOrder = 70;
        // Always available: the reason to open it is usually that something is
        // failing, which is exactly when nothing else points you at it.
        action.enabled = [] { return true; };
        action.trigger = [this] { openStandingTab(QStringLiteral("core.automation")); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.compress");
        action.section = MenuAction::Section::Operations;
        action.title = QStringLiteral("Compress…");
        action.iconText = QStringLiteral("\U0001F5DC");
        action.sortOrder = 25;
        // Absent rather than greyed out in a build without libarchive: there is
        // nothing to explain, the feature simply is not there. See ADR-0007.
        action.enabled = [this] { return canCompress() && !currentLocation().isEmpty(); };
        action.trigger = [this] { emit compressionRequested(); };
        if (canCompress())
            m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.folderSizes");
        action.section = MenuAction::Section::Operations;
        action.title = QStringLiteral("Folder sizes");
        action.shortcut = QStringLiteral("Ctrl+Shift+S");
        action.iconText = QStringLiteral("\u2211");
        action.sortOrder = 35;
        action.enabled = [this] { return !currentLocation().isEmpty(); };
        action.trigger = [this] { measureFolderSizes(); };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.tools.indexFolder");
        action.section = MenuAction::Section::Operations;
        action.title = QStringLiteral("Index this folder");
        action.iconText = QStringLiteral("\u26C1");
        action.sortOrder = 40;
        action.enabled = [this] { return currentTabProperty("activePane").value<QObject*>() != nullptr; };
        action.trigger = [this] {
            QObject* pane = currentTabProperty("activePane").value<QObject*>();
            if (!pane)
                return;
            // Read before the tab changes under us, because opening one makes a
            // different pane current.
            const QString uri = pane->property("currentUri").toString();
            const QString label = pane->property("locationName").toString();
            // The dialog rather than a scan: this entry is the one somebody
            // finds first, and it used to start the slowest and least complete
            // scan Mole has with none of its four options asked for. Two doors
            // onto one operation, and now they are the same door.
            if (openFeatureTab(QStringLiteral("mole.livesearch")) < 0)
                return;
            emit indexFolderRequested(uri, label);
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

    // And the one Help entry that is not a dialog: whether Mole looks for a newer
    // version of itself when it starts.
    //
    // **A notice nobody can switch off is not a notice anybody should ship**, which
    // is why this is in the same ticket as the notice. Default on, which is the
    // author's decision of 2026-09-01 and the reason README.md has to say the
    // request happens at all.
    //
    // Unticking stops the check and does nothing else: it does not clear which
    // version was announced, does not clear the stored ETag, and does not take
    // anything off the screen. Ticking it again resumes, and the week of silence
    // still applies -- a switch that reset the state would be a switch people use
    // to force a check, which is not what it is. See UpdateCheck.h.
    {
        MenuAction action;
        action.id = QStringLiteral("mole.help.updates");
        action.section = MenuAction::Section::Help;
        action.title = QStringLiteral("Check for new versions");
        action.sortOrder = 40;
        // Set apart from the three dialogs above, because it is not one.
        action.separatorBefore = true;
        // Read when the menu opens rather than cached, which is what makes the tick
        // right after a restart without anybody invalidating anything.
        action.checked = [this] { return m_preferences->value(UpdateCheck::enabledKey(), true).toBool(); };
        action.trigger = [this] {
            const bool looking = m_preferences->value(UpdateCheck::enabledKey(), true).toBool();
            m_preferences->setValue(UpdateCheck::enabledKey(), !looking);
        };
        m_actions->addAction(std::move(action));
    }
}

QString AppController::currentLocation() const
{
    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    return pane ? pane->property("currentUri").toString() : QString();
}

namespace {

    /// Calls a method on whatever object is given, when there is one. The four
    /// routed keys differ only in the name, so the resolution lives in one place.
    void invokeOn(QObject* target, const char* method)
    {
        if (target)
            QMetaObject::invokeMethod(target, method);
    }

} // namespace

void AppController::previewCurrent()
{
    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    if (!pane)
        return;
    QObject* files = pane->property("files").value<QObject*>();
    if (!files)
        return;

    const int row = pane->property("currentIndex").toInt();
    bool isDir = false;
    QMetaObject::invokeMethod(files, "isDirAt", Q_RETURN_ARG(bool, isDir), Q_ARG(int, row));
    if (isDir) {
        bool opened = false;
        QMetaObject::invokeMethod(pane, "activate", Q_RETURN_ARG(bool, opened), Q_ARG(int, row));
        return;
    }

    const QString file = currentFile();
    if (!file.isEmpty())
        previewFile(file);
}

void AppController::goUpInCurrentPane()
{
    invokeOn(currentTabProperty("activePane").value<QObject*>(), "goUp");
}

void AppController::goBackInCurrentPane()
{
    invokeOn(currentTabProperty("activePane").value<QObject*>(), "goBack");
}

void AppController::goForwardInCurrentPane()
{
    invokeOn(currentTabProperty("activePane").value<QObject*>(), "goForward");
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

QString AppController::pathTextFor(const QString& uri) const
{
    const VfsUri parsed = VfsUri::fromString(uri);
    if (!parsed.isValid())
        return {};

    // Native where there is one, so it can be pasted into a terminal or a file
    // dialog; the whole uri otherwise, because the path on its own would read as
    // local and would not be.
    const QString local = parsed.toLocalPath();
    return local.isEmpty() ? parsed.toString() : local;
}

QString AppController::uriForPathText(const QString& text) const
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};

    // Anything with a scheme is a uri already, so remote drives stay reachable
    // by typing. Normalised through VfsUri so ".." and a trailing slash mean the
    // same thing however the location was reached -- and handed back untouched
    // when it does not parse, because the caller's error is more use to somebody
    // than an empty field.
    if (trimmed.contains(QLatin1String("://"))) {
        const VfsUri parsed = VfsUri::fromString(trimmed);
        return parsed.isValid() ? parsed.toString() : trimmed;
    }

    return VfsUri::fromLocalPath(trimmed).toString();
}

QString AppController::copyCurrentFolderPath()
{
    return copyPathAndSay(currentLocation(), QStringLiteral("Folder path copied"));
}

QString AppController::copySelectedFilePath()
{
    return copyPathAndSay(currentFile(), QStringLiteral("File path copied"));
}

QString AppController::copyDriveRootPath()
{
    if (!m_vfs)
        return {};

    const VfsUri here = VfsUri::fromString(currentLocation());
    if (!here.isValid())
        return {};

    const Mount mount = m_vfs->mountForUri(here);
    if (!mount.isValid())
        return {};

    return copyPathAndSay(mount.root.toString(), QStringLiteral("Drive path copied"));
}

QString AppController::copyPathAndSay(const QString& uri, const QString& title)
{
    const QString text = pathTextFor(uri);
    if (text.isEmpty())
        return {};

    // Guarded rather than assumed: there is no clipboard without a QGuiApplication,
    // and the headless tests run without one. The text is returned either way, so
    // what would have been copied is still testable.
    if (QClipboard* clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);

    // Said out loud, because a clipboard gives no feedback of its own and three
    // actions that all copy "a path" are easy to confuse.
    emit notification(static_cast<int>(EventBus::Severity::Info), title, text);
    return text;
}

void AppController::previewFile(const QString& uri)
{
    if (uri.isEmpty())
        return;

    // One preview tab, reused. Pressing F3 on twenty files should not leave
    // twenty tabs behind. Not openStandingTab(): a preview is not a standing
    // tool and has to be told which file it is about.
    const int existing = m_tabs->rowOfFeature(QStringLiteral("mole.preview"));
    if (existing >= 0) {
        if (QObject* controller = m_tabs->controllerAt(existing)) {
            QMetaObject::invokeMethod(controller, "open", Q_ARG(QString, uri));
            m_tabs->setCurrentIndex(existing);
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

    // The pane's own selection, for a tab that has a pane and did not answer
    // above. It is asked of the *current* controller, so a tab with no pane -- a
    // set, a duplicate scan, a list of results -- answers nothing here rather
    // than borrowing a browser's selection: currentTabProperty() reads the
    // property off the controller in front of the user, and a controller without
    // an activePane property gives an invalid QVariant. Written down because it
    // reads like a fallback to "whatever the file manager last had", and is not.
    // See ARCHITECTURE.md, "Acting on a list of things", and MOLE-408.
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

QStringList AppController::currentTargetsOrCursor() const
{
    QStringList ticked = currentTargets();
    if (!ticked.isEmpty())
        return ticked;

    // Nothing ticked means "whatever the cursor is on", which is what every other
    // operation here does and what a commander-style manager has always done.
    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    QObject* files = pane ? pane->property("files").value<QObject*>() : nullptr;
    if (!files)
        return {};

    const int row = pane->property("currentIndex").toInt();
    QString uri;
    QMetaObject::invokeMethod(files, "uriAt", Q_RETURN_ARG(QString, uri), Q_ARG(int, row));
    if (!uri.isEmpty())
        ticked.append(uri);
    return ticked;
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

QString AppController::compressionSubject() const
{
    const QStringList targets = currentTargetsOrCursor();
    if (targets.isEmpty()) {
        const QString here = currentLocation();
        return here.isEmpty() ? QString()
                              : QStringLiteral("the folder %1").arg(VfsUri::fromString(here).fileName());
    }
    if (targets.size() == 1)
        return VfsUri::fromString(targets.first()).fileName();
    return QStringLiteral("%1 selected items").arg(targets.size());
}

QVariantList AppController::compressionTargets() const
{
    QStringList uris = currentTargetsOrCursor();
    // The folder in view is what gets packed when there is no row at all, and it is
    // as much a target as anything ticked -- so it is listed the same way.
    if (uris.isEmpty() && !currentLocation().isEmpty())
        uris.append(currentLocation());

    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    QObject* files = pane ? pane->property("files").value<QObject*>() : nullptr;

    QVariantList out;
    out.reserve(uris.size());
    for (const QString& uri : uris) {
        bool isDir = false;
        if (files) {
            int row = -1;
            QMetaObject::invokeMethod(files, "rowOfUri", Q_RETURN_ARG(int, row), Q_ARG(QString, uri));
            if (row >= 0)
                QMetaObject::invokeMethod(files, "isDirAt", Q_RETURN_ARG(bool, isDir), Q_ARG(int, row));
            else
                isDir = uri == currentLocation(); // the folder in view is not a row in it
        }
        out.append(QVariantMap { { QStringLiteral("name"), VfsUri::fromString(uri).fileName() },
            { QStringLiteral("isDir"), isDir } });
    }
    return out;
}

bool AppController::formatSupportsPassword(const QString& format) const
{
#ifdef MOLE_HAVE_ARCHIVE
    // Only zip carries a password. A tar is a container with no notion of one, and
    // gzip and xz encrypt nothing -- so the box is not offered rather than being
    // offered and ignored.
    return CompressTask::formatSupportsPassword(CompressTask::formatFromName(format));
#else
    Q_UNUSED(format);
    return false;
#endif
}

bool AppController::canCompress() const
{
#ifdef MOLE_HAVE_ARCHIVE
    return true;
#else
    return false;
#endif
}

QStringList AppController::compressionFormats() const
{
#ifdef MOLE_HAVE_ARCHIVE
    return CompressTask::formatNames();
#else
    return {};
#endif
}

QString AppController::suggestedArchiveName(const QString& format) const
{
#ifdef MOLE_HAVE_ARCHIVE
    const QStringList targets = currentTargetsOrCursor();
    const QString here = currentLocation();
    // One item takes its own name; several take the folder's, because "3 items.zip"
    // tells you nothing a month later.
    QString base;
    if (targets.size() == 1)
        base = VfsUri::fromString(targets.first()).fileName();
    else if (!here.isEmpty())
        base = VfsUri::fromString(here).fileName();
    if (base.isEmpty())
        base = QStringLiteral("archive");

    // A name like "notes.txt" becomes "notes.zip" rather than "notes.txt.zip".
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        base = base.left(dot);
    return base + CompressTask::suffixFor(CompressTask::formatFromName(format));
#else
    Q_UNUSED(format);
    return {};
#endif
}

QString AppController::archiveNameForFormat(const QString& currentName, const QString& format) const
{
#ifdef MOLE_HAVE_ARCHIVE
    const QString renamed = CompressTask::nameWithSuffix(currentName, CompressTask::formatFromName(format));
    // Only when there is nothing to keep does it fall back to suggesting one.
    return renamed.isEmpty() ? suggestedArchiveName(format) : renamed;
#else
    Q_UNUSED(currentName);
    Q_UNUSED(format);
    return {};
#endif
}

bool AppController::formatTakesOneFileOnly(const QString& format) const
{
#ifdef MOLE_HAVE_ARCHIVE
    return CompressTask::takesOneFileOnly(CompressTask::formatFromName(format));
#else
    Q_UNUSED(format);
    return false;
#endif
}

void AppController::compressSelection(
    const QString& archiveName, const QString& format, const QString& passphrase, bool removeSources)
{
#ifdef MOLE_HAVE_ARCHIVE
    QStringList targets = currentTargetsOrCursor();
    const QString here = currentLocation();
    if (targets.isEmpty() && !here.isEmpty())
        targets.append(here);
    if (targets.isEmpty() || archiveName.trimmed().isEmpty())
        return;

    CompressTask::Request request;
    request.format = CompressTask::formatFromName(format);
    request.passphrase = passphrase;
    request.removeSourcesWhenDone = removeSources;
    for (const QString& uri : targets)
        request.sources.append(VfsUri::fromString(uri));

    request.sourceFileSystem = m_vfs->resolve(request.sources.first());
    // Beside what is being packed, which is where anyone would look for it.
    const VfsUri folder = here.isEmpty() ? request.sources.first().parent() : VfsUri::fromString(here);
    request.target = folder.child(archiveName.trimmed());
    request.targetFileSystem = m_vfs->resolve(request.target);

    if (!request.sourceFileSystem || !request.targetFileSystem) {
        emit notification(static_cast<int>(EventBus::Severity::Warning), QStringLiteral("Cannot compress"),
            QStringLiteral("No drive is mounted for this"));
        return;
    }

    auto* task = new CompressTask(request);
    const QString targetUri = request.target.toString();
    connect(task, &Task::finished, this, [this, task, targetUri] {
        if (task->state() == Task::State::Failed) {
            emit notification(static_cast<int>(EventBus::Severity::Warning),
                QStringLiteral("Compression failed"), task->error().message);
            return;
        }
        if (task->state() != Task::State::Succeeded)
            return;
        // Anything deleted afterwards is announced entry by entry, so a second pane
        // on the same folder stops showing files that are no longer there.
        for (const VfsUri& removed : task->removedSources())
            m_events->postEntryRemoved(removed);
        // The listing has a new file in it, and whoever asked wants to see it.
        m_events->postDirectoryChanged(VfsUri::fromString(targetUri).parent());
    });
    m_taskManager->submit(task);
#else
    Q_UNUSED(archiveName);
    Q_UNUSED(format);
    Q_UNUSED(removeSources);
    emit notification(static_cast<int>(EventBus::Severity::Warning), QStringLiteral("Cannot compress"),
        QStringLiteral("This build was made without libarchive"));
#endif
}

void AppController::measureFolderSizes()
{
    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    QObject* files = pane ? pane->property("files").value<QObject*>() : nullptr;
    if (!files)
        return;

    // Ticked folders if there are any; otherwise every folder in the listing,
    // because "which of these is the big one" is the question being asked.
    QStringList targets = selectedFolders();
    if (targets.isEmpty())
        QMetaObject::invokeMethod(files, "folderUris", Q_RETURN_ARG(QStringList, targets));
    if (targets.isEmpty()) {
        emit notification(static_cast<int>(EventBus::Severity::Info), QStringLiteral("Nothing to measure"),
            QStringLiteral("There are no folders in this listing"));
        return;
    }

    QList<VfsUri> folders;
    folders.reserve(targets.size());
    for (const QString& uri : targets)
        folders.append(VfsUri::fromString(uri));

    FileSystemPtr fs = m_vfs->resolve(folders.first());
    if (!fs) {
        emit notification(static_cast<int>(EventBus::Severity::Warning), QStringLiteral("Cannot measure"),
            QStringLiteral("No drive is mounted for %1").arg(folders.first().toString()));
        return;
    }

    auto* task = new FolderSizesTask(fs, folders);
    // Guarded by a QPointer: the answers arrive over seconds and the listing they
    // were asked about may be gone by then, which is not an error -- the user
    // navigated away, and the measurement simply has nowhere to land.
    QPointer<QObject> model(files);
    connect(task, &FolderSizesTask::folderSized, this, [model](const VfsUri& folder, qint64 bytes, qint64) {
        if (!model)
            return;
        QMetaObject::invokeMethod(
            model, "setMeasuredSize", Q_ARG(QString, folder.toString()), Q_ARG(qint64, bytes));
    });
    m_taskManager->submit(task);
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

QString AppController::currentSetId() const
{
    QObject* controller = m_tabs ? m_tabs->currentController() : nullptr;
    if (!controller)
        return {};
    // By property, not by type, the same way currentTargets() finds targetUris():
    // a controller with a non-empty currentSetId is a tab whose subject is a set,
    // and nothing here has to learn what a set is. See MOLE-209.
    return controller->property("currentSetId").toString();
}

void AppController::watchActivePane()
{
    // The tab first, because the pane the keyboard is in changes inside a tab as
    // well as between tabs. Asked by signature rather than by cast, so a tab kind
    // the shell has never heard of simply does not answer.
    if (QObject* tab = m_tabs ? m_tabs->currentController() : nullptr) {
        if (tab->metaObject()->indexOfSignal("activePaneChanged()") >= 0) {
            connect(tab, SIGNAL(activePaneChanged()), this, SLOT(watchActivePane()), Qt::UniqueConnection);
        }
    }

    if (QObject* pane = currentTabProperty("activePane").value<QObject*>()) {
        if (pane->metaObject()->indexOfSignal("driveActionsChanged()") >= 0) {
            connect(
                pane, SIGNAL(driveActionsChanged()), this, SLOT(refreshDriveActions()), Qt::UniqueConnection);
        }
    }

    refreshDriveActions();
}

void AppController::refreshDriveActions()
{
    if (!m_actions)
        return;

    // Rebuilt wholesale, like the bookmarks and for the same reason: it is a
    // handful of entries and correctness beats cleverness. What is on offer
    // changes with every step of the cursor.
    static const QString prefix = QStringLiteral("mole.drive.");
    m_actions->removeActionsStartingWith(prefix);

    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    if (!pane)
        return;

    // Read rather than asked for. The drive was asked on a worker when the
    // cursor landed here; this is on the thread that draws, and a menu opening
    // must never wait on storage.
    const QVariantList offered = pane->property("driveActions").toList();

    int order = 300;
    for (const QVariant& entry : offered) {
        const QVariantMap fields = entry.toMap();
        const QString id = fields.value(QStringLiteral("id")).toString();
        const QString title = fields.value(QStringLiteral("title")).toString();
        if (id.isEmpty() || title.isEmpty())
            continue;

        MenuAction action;
        action.id = prefix + id;
        // Something done to the file in front of you, which leaves you where you
        // were. See MenuAction::Section and docs/adr/0003-menu-sections.md -- a
        // drive's action is not a different question from a plugin's.
        action.section = MenuAction::Section::Operations;
        // The drive's own words. Nothing here knows what the action does, and a
        // title invented at this layer would be a guess shown as a label.
        action.title = title;
        action.sortOrder = order;
        action.separatorBefore = order == 300;
        order += 1;

        const bool enabled = fields.value(QStringLiteral("enabled"), true).toBool();
        action.enabled = [enabled] { return enabled; };
        action.trigger = [this, id] {
            if (QObject* active = currentTabProperty("activePane").value<QObject*>())
                QMetaObject::invokeMethod(active, "invokeDriveAction", Q_ARG(QString, id));
        };
        m_actions->addAction(std::move(action));
    }
}

void AppController::refreshBookmarkActions()
{
    if (!m_actions || !m_bookmarks)
        return;

    // Rebuilt wholesale rather than kept in sync row by row: the list is a
    // handful of entries and correctness beats cleverness here. The two entries
    // that act on where you are are rebuilt too, because what they act on -- a
    // folder or a set -- depends on which tab is in front of you, and an entry
    // that says "folder" while about to bookmark a set is worse than no entry.
    static const QString prefix = QStringLiteral("mole.bookmarks.");
    m_actions->removeActionsStartingWith(prefix);

    const bool aboutASet = !currentSetId().isEmpty();
    {
        MenuAction action;
        action.id = QStringLiteral("mole.bookmarks.add");
        action.section = MenuAction::Section::Bookmarks;
        action.title = aboutASet ? QStringLiteral("Add this set") : QStringLiteral("Add current folder");
        action.shortcut = QStringLiteral("Ctrl+D");
        action.sortOrder = 10;
        // The predicates read the tab again rather than trusting the title: the
        // title is a label rebuilt on a tab change, and these decide what happens.
        action.enabled = [this] {
            const QString set = currentSetId();
            if (!set.isEmpty())
                return !m_bookmarks->containsSet(set);
            const QString here = currentLocation();
            return !here.isEmpty() && !m_bookmarks->contains(here);
        };
        action.trigger = [this] {
            const QString set = currentSetId();
            if (!set.isEmpty()) {
                m_bookmarks->addSet(set);
                return;
            }
            m_bookmarks->add(currentLocation());
        };
        m_actions->addAction(std::move(action));
    }
    {
        MenuAction action;
        action.id = QStringLiteral("mole.bookmarks.remove");
        action.section = MenuAction::Section::Bookmarks;
        action.title
            = aboutASet ? QStringLiteral("Remove this set") : QStringLiteral("Remove current folder");
        action.sortOrder = 20;
        action.enabled = [this] {
            const QString set = currentSetId();
            return set.isEmpty() ? m_bookmarks->contains(currentLocation()) : m_bookmarks->containsSet(set);
        };
        action.trigger = [this] {
            const QString set = currentSetId();
            if (!set.isEmpty()) {
                m_bookmarks->removeSet(set);
                return;
            }
            m_bookmarks->removeUri(currentLocation());
        };
        m_actions->addAction(std::move(action));
    }

    static const QString goPrefix = QStringLiteral("mole.bookmarks.go.");

    int order = 100;
    for (int row = 0; row < m_bookmarks->rowCount(); ++row) {
        const QModelIndex at = m_bookmarks->index(row, 0);
        const QString kind = at.data(BookmarkModel::KindRole).toString();
        const QString target = at.data(BookmarkModel::TargetRole).toString();

        MenuAction action;
        // A folder keeps the id it has always had. A set's id is not a uri, so it
        // gets its own form rather than being concatenated into the same space:
        // one id per bookmark, whatever it points at.
        action.id
            = kind == QLatin1String("set") ? goPrefix + QStringLiteral("set.") + target : goPrefix + target;
        action.section = MenuAction::Section::Bookmarks;
        // The set's current name, read from the store by the model.
        action.title = at.data(BookmarkModel::NameRole).toString();
        action.sortOrder = order;
        action.separatorBefore = order == 100;
        action.trigger = [this, kind, target] { openPlace(kind, target); };
        m_actions->addAction(std::move(action));
        order += 10;
    }
}

void AppController::setTheme(const QString& name)
{
    m_colour->setTheme(name);
    // What the palette settled on rather than what was asked for, so picking a
    // theme also repairs a preferences file that named one Mole does not have.
    if (m_preferences)
        m_preferences->setValue(QStringLiteral("ui.theme"), m_colour->theme());
}

QVariantList AppController::buildMenu()
{
    // What a drive offers depends on the row the cursor is on, so the entries
    // are rebuilt as the menu is opened rather than kept in step with the
    // cursor. It is the same answer ActionRegistry gives about its predicates:
    // a menu is opened rarely and read once.
    refreshDriveActions();
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

void AppController::startDrag(const QStringList& uris)
{
    if (!m_dragSource)
        return;

    QList<VfsUri> rows;
    rows.reserve(uris.size());
    for (const QString& uri : uris)
        rows.append(VfsUri::fromString(uri));

    // What the listing in front of the user says these rows are. The drag needs
    // it to tell a staged copy from a stale one, and taking it from the model
    // rather than asking the drive is what keeps a stat() off this thread -- a
    // drag of a row on a share that had stopped answering used to freeze the
    // window at the moment somebody picked the file up. See MOLE-360.
    QHash<QString, FileEntry> known;
    QObject* pane = currentTabProperty("activePane").value<QObject*>();
    if (auto* files
        = pane ? qobject_cast<FileListModel*>(pane->property("files").value<QObject*>()) : nullptr) {
        for (const QString& uri : uris) {
            const int row = files->rowOfUri(uri);
            if (row >= 0)
                known.insert(uri, files->entryAt(row));
        }
    }

    m_dragSource->start(rows, known);
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

    auto* task = new ScanTask(fs, root, label.isEmpty() ? uri : label, m_index.get());
    // What the dialog opens on. Setting nothing meant every call walked the
    // whole tree and wrote rows that said nothing about the files.
    ScanOptions options;
    options.incremental = true;
    options.archives = true;
    applyScanOptions(*task, options, m_services, fs, root);
    connect(task, &Task::finished, this, [this, task] {
        if (task->state() == Task::State::Succeeded)
            m_events->postIndexUpdated(-1, task->filesIndexed());
    });
    m_taskManager->submit(task);
}

} // namespace mole
