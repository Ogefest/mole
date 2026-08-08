#pragma once

#include "host/FeatureRegistry.h"
#include "sdk/PluginApi.h"
#include "ui/models/BookmarkModel.h"
#include "ui/models/CommandPaletteModel.h"
#include "ui/models/MountListModel.h"
#include "ui/models/TabsModel.h"
#include "ui/models/TaskListModel.h"
#include "ui/models/TerminalController.h"

#include "core/automation/Scheduler.h"

#include <QObject>
#include <QStringList>
#include <QVariantList>

class QTimer;

#include <memory>
#include <vector>

namespace mole {

class VfsManager;
class TaskManager;
class IndexDatabase;
class EventBus;
class PreviewRegistry;
class PluginManager;
class FileLauncher;
class ActionRegistry;
class SessionStore;
class ScheduleStore;
class Scheduler;
class AlertStore;
class AnalysisStore;
class FileSetStore;
class SecretStore;
class RemoteRegistry;

/// Owns the application's services and exposes them to QML as a single root
/// object. Construction order here is the application's startup sequence.
class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(mole::TabsModel* tabs READ tabs CONSTANT)
    Q_PROPERTY(mole::MountListModel* mounts READ mounts CONSTANT)
    Q_PROPERTY(mole::TaskListModel* tasks READ tasks CONSTANT)
    Q_PROPERTY(mole::FeatureRegistry* features READ features CONSTANT)
    Q_PROPERTY(mole::BookmarkModel* bookmarks READ bookmarks CONSTANT)
    /// Everything that can be done right now, as one filterable list -- the menu,
    /// the bookmarks and the drives. A view over those, never a copy of them.
    Q_PROPERTY(mole::CommandPaletteModel* commands READ commands CONSTANT)
    Q_PROPERTY(QStringList pluginSummary READ pluginSummary CONSTANT)
    Q_PROPERTY(QStringList pluginErrors READ pluginErrors CONSTANT)
    Q_PROPERTY(QString defaultLocation READ defaultLocation CONSTANT)
    /// The family every code and data view uses. Picked once, here, so a
    /// preview, a table and a log all line up rather than each guessing.
    Q_PROPERTY(QString monospaceFont READ monospaceFont CONSTANT)

    // ---- the type scale --------------------------------------------------
    //
    // Sizes are chosen here for the same reason the monospace family is: so that
    // a listing, a preview and a form line up instead of each picking its own
    // number. Four steps and one code size, each with a job:
    //
    //   headingSize        a title in a view
    //   textSize           primary content -- file names, prose, cell text
    //   secondaryTextSize  supporting content -- sizes, dates, column headers,
    //                      form labels
    //   smallTextSize      captions and badges, and the floor: nothing is
    //                      allowed to be smaller than this
    //   monospaceSize      code and data, which reads a shade smaller than prose
    //
    // Constant for now. When these become a preference the views do not change,
    // which is the point of them being here.
    Q_PROPERTY(int headingSize READ headingSize CONSTANT)
    Q_PROPERTY(int textSize READ textSize CONSTANT)
    Q_PROPERTY(int secondaryTextSize READ secondaryTextSize CONSTANT)
    Q_PROPERTY(int smallTextSize READ smallTextSize CONSTANT)
    Q_PROPERTY(int monospaceSize READ monospaceSize CONSTANT)
    /// Derived rather than stated, so raising the text size does not crop a row.
    Q_PROPERTY(int listRowHeight READ listRowHeight CONSTANT)
    /// The floor for anything that is only an icon -- a close cross, an add
    /// button. Twenty-four is the figure usually quoted as a minimum for a
    /// pointer; a desktop control nearer thirty stops feeling like a pinprick,
    /// and these are the two things people reach for most.
    Q_PROPERTY(int minimumTarget READ minimumTarget CONSTANT)
    Q_PROPERTY(mole::TerminalController* terminal READ terminal CONSTANT)
    /// The credential store's state, so the interface can ask for a passphrase
    /// once rather than failing drive by drive.
    Q_PROPERTY(bool credentialsAvailable READ credentialsAvailable CONSTANT)
    Q_PROPERTY(bool credentialsExist READ credentialsExist NOTIFY credentialsChanged)
    Q_PROPERTY(bool credentialsUnlocked READ credentialsUnlocked NOTIFY credentialsChanged)
    Q_PROPERTY(bool credentialsNeeded READ credentialsNeeded NOTIFY credentialsChanged)
    /// Properties, not plain callable methods. A method call in a QML binding
    /// is evaluated once and never again -- there is no signal to tell the
    /// binding it went stale -- so a drive saved through this dialog would
    /// never appear in its own list.
    Q_PROPERTY(QVariantList driveKinds READ driveKinds NOTIFY drivesChanged)
    Q_PROPERTY(QVariantList configuredDrives READ configuredDrives NOTIFY drivesChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    /// Builds services, registers `builtIns`, loads external plugins, mounts
    /// the local disk and opens the first tab. Returns false with `errorOut`
    /// set if the index cannot be opened.
    ///
    /// Built-in plugins are injected rather than constructed here so that this
    /// layer does not depend on the features it hosts -- the same reason a
    /// third-party plugin does not get to reach into the shell.
    bool initialise(std::vector<std::unique_ptr<IPlugin>> builtIns, QString* errorOut = nullptr);

    TabsModel* tabs() const { return m_tabs; }
    MountListModel* mounts() const { return m_mounts; }
    TaskListModel* tasks() const { return m_taskModel; }
    TerminalController* terminal() const { return m_terminal; }
    FeatureRegistry* features() const { return m_features; }
    CommandPaletteModel* commands() const { return m_commands; }
    BookmarkModel* bookmarks() const { return m_bookmarks; }
    PreviewRegistry* previews() const { return m_previews; }
    Scheduler* scheduler() const { return m_scheduler; }
    ScheduleStore* schedules() const { return m_schedules; }
    AlertStore* alerts() const { return m_alerts; }
    AnalysisStore* reports() const { return m_reports.get(); }
    FileSetStore* sets() const { return m_sets; }
    RemoteRegistry* remotes() const { return m_remotes; }

    bool credentialsAvailable() const;
    bool credentialsExist() const;
    bool credentialsUnlocked() const;
    /// True when a configured drive cannot connect until the store is opened.
    bool credentialsNeeded() const;

    /// Creates the store with this passphrase, or opens an existing one.
    Q_INVOKABLE bool unlockCredentials(const QString& passphrase);
    Q_INVOKABLE QString credentialsError() const { return m_credentialsError; }

    // ---- configured drives ----------------------------------------------

    /// The kinds of drive that can be added, from every registered factory.
    QVariantList driveKinds() const;
    /// The form for one kind, as field descriptions.
    Q_INVOKABLE QVariantList driveFields(const QString& factoryScheme, const QString& variant) const;
    /// The drives already configured.
    QVariantList configuredDrives() const;
    /// Adds or updates one. `values` holds every field; the secret ones are
    /// separated here rather than by the caller, so a view cannot get it wrong.
    Q_INVOKABLE bool saveDrive(const QString& id, const QString& name, const QString& factoryScheme,
        const QString& variant, const QString& root, const QVariantMap& values);
    Q_INVOKABLE bool removeDrive(const QString& id);
    /// Connects one now. Returns an empty string on success, else the reason.
    Q_INVOKABLE QString connectDrive(const QString& id);
    Q_INVOKABLE void disconnectDrive(const QString& id);
    const PluginServices& services() const { return m_services; }

    QString monospaceFont() const;
    int headingSize() const { return 17; }
    int textSize() const { return 14; }
    int secondaryTextSize() const { return 13; }
    int smallTextSize() const { return 11; }
    int monospaceSize() const { return 13; }
    int listRowHeight() const { return qRound(textSize() * 2.2); }
    int minimumTarget() const { return 28; }

    QStringList pluginSummary() const;
    QStringList pluginErrors() const;

    /// Opens a browser tab already pointing at `uri`.
    /// Opens a tab and, when the new tab has somewhere to start from, points
    /// it at wherever the user currently is. A search opened from a folder
    /// should search that folder, not a default nobody asked for.
    Q_INVOKABLE int openFeatureTab(const QString& featureId);

    /// Opens a preview tab for `uri`, reusing one that is already open rather
    /// than piling up a tab per file.
    Q_INVOKABLE void previewFile(const QString& uri);

    /// Opens the report a folder already has, loading the saved one rather than
    /// walking the tree again. Reuses a tab already showing it.
    Q_INVOKABLE void openReportFor(const QString& uri);

    /// What an operation started from the current tab should act on. A pane's
    /// selection, or whatever the tab declares -- a set names its members the
    /// same way, which is what lets every operation take a set without knowing
    /// what one is.
    Q_INVOKABLE QStringList currentTargets() const;

    /// Adds the current targets to a set, creating it when `setId` is empty.
    /// Returns how many were new.
    Q_INVOKABLE int addToSet(const QString& setId, const QString& newSetName = {});
    /// Sets to offer in a menu, newest activity first.
    Q_INVOKABLE QVariantList setChoices() const;

    /// Analyses the selected folders, or the current one when nothing is
    /// ticked. Always a new tab: comparing two analyses side by side is the
    /// point, and reusing one would throw the other away.
    Q_INVOKABLE void analyseSelection();

    Q_INVOKABLE void openLocation(const QString& uri);
    /// Navigates the current tab if it can navigate, otherwise opens a new one.
    Q_INVOKABLE void goTo(const QString& uri);
    /// Mounts an archive as a drive and opens it. Returns the new mount's root
    /// uri, or an empty string on failure.
    Q_INVOKABLE QString openArchive(const QString& archiveUri);
    /// True when this file can be mounted as a drive.
    Q_INVOKABLE bool isMountableArchive(const QString& uri) const;
    /// Totals up the ticked folders, or every folder in the listing when none
    /// are ticked, and writes the answers into the rows as they arrive.
    Q_INVOKABLE void measureFolderSizes();
    Q_INVOKABLE void queueScan(const QString& uri, const QString& label);

    // ---- application menu -------------------------------------------------

    /// Sections and entries for the menu, rebuilt on every call so tick boxes
    /// and greyed-out entries reflect the tab that is open right now.
    Q_INVOKABLE QVariantList buildMenu() const;
    Q_INVOKABLE bool triggerAction(const QString& id);
    ActionRegistry* actions() const { return m_actions; }

    /// Hands a file to the desktop's default application for its type. Files
    /// on a remote or archive drive are extracted to a scratch copy first.
    Q_INVOKABLE void openExternally(const QString& uri);

    QString defaultLocation() const;
    FileLauncher* launcher() const { return m_launcher; }

    // ---- window geometry --------------------------------------------------

    /// Geometry to restore, as `{ width, height, x, y, maximized }`. Empty
    /// when there is nothing usable to restore.
    Q_INVOKABLE QVariantMap savedWindowGeometry() const;
    /// Called by the shell whenever the window is moved, resized or maximised.
    Q_INVOKABLE void rememberWindowGeometry(int x, int y, int width, int height, bool maximized);

    /// True when this rectangle still overlaps a screen that exists. A window
    /// restored onto an unplugged monitor is invisible and unrecoverable.
    static bool geometryIsOnScreen(int x, int y, int width, int height);

    /// Writes the open tabs out now, rather than waiting for the debounce.
    /// Called on shutdown; exposed so tests do not have to wait either.
    Q_INVOKABLE void saveSessionNow();

signals:
    void credentialsChanged();
    void drivesChanged();
    /// The shell should show the drives dialog. A signal rather than a direct
    /// call, because this layer has no business knowing what a dialog is.
    void drivesRequested();
    void notification(int severity, const QString& title, const QString& detail);
    /// A menu entry whose body is a QML dialog was picked; the shell decides
    /// which one to show. Keeps dialog markup out of C++.
    void dialogRequested(const QString& actionId);

private:
    void mountDefaultDrives();
    void registerShellActions();
    /// Restores the previous tabs, or opens a default one. Returns false when
    /// there was nothing to restore.
    bool restoreSession();
    /// Rebuilds the one-entry-per-bookmark part of the menu.
    void refreshBookmarkActions();
    /// Where the current tab is pointing, or an empty string for a tab that
    /// has no location.
    QString currentLocation() const;
    /// The file under the cursor in the current tab, if it has one.
    QString currentFile() const;
    /// Ticked folders in the current tab; empty when none are.
    QStringList selectedFolders() const;
    /// Reads a property off the current tab's controller, or `fallback` when
    /// this kind of tab does not have it. Keeps the menu working for features
    /// the shell has never heard of.
    QVariant currentTabProperty(const char* name, const QVariant& fallback = {}) const;
    void setCurrentTabProperty(const char* name, const QVariant& value);

    VfsManager* m_vfs = nullptr;
    TaskManager* m_taskManager = nullptr;
    /// Not a QObject, and it must outlive every task that writes to it, so its
    /// lifetime is managed explicitly rather than by the QObject tree.
    std::unique_ptr<IndexDatabase> m_index;
    EventBus* m_events = nullptr;
    FeatureRegistry* m_features = nullptr;
    PreviewRegistry* m_previews = nullptr;
    PluginManager* m_plugins = nullptr;
    FileLauncher* m_launcher = nullptr;
    ActionRegistry* m_actions = nullptr;
    TerminalController* m_terminal = nullptr;
    ScheduleStore* m_schedules = nullptr;
    Scheduler* m_scheduler = nullptr;
    AlertStore* m_alerts = nullptr;
    std::unique_ptr<AnalysisStore> m_reports;
    FileSetStore* m_sets = nullptr;
    SecretStore* m_secrets = nullptr;
    RemoteRegistry* m_remotes = nullptr;
    QString m_credentialsError;
    BookmarkModel* m_bookmarks = nullptr;
    CommandPaletteModel* m_commands = nullptr;
    WindowGeometry m_window;
    std::unique_ptr<SessionStore> m_session;
    /// Navigation fires state changes constantly, so writes are coalesced.
    QTimer* m_sessionSaveTimer = nullptr;
    /// Restoring opens tabs, which would immediately mark the session dirty.
    bool m_restoring = false;
    TabsModel* m_tabs = nullptr;
    MountListModel* m_mounts = nullptr;
    TaskListModel* m_taskModel = nullptr;
    PluginServices m_services;
};

} // namespace mole
