#pragma once

#include "host/FeatureRegistry.h"
#include "sdk/PluginApi.h"
#include "ui/Palette.h"
#include "ui/models/BookmarkModel.h"
#include "ui/models/CommandPaletteModel.h"
#include "ui/models/DriveListModel.h"
#include "ui/models/TabsModel.h"
#include "ui/models/TaskListModel.h"
#include "ui/models/TerminalController.h"

#include "core/automation/Scheduler.h"
#include "core/data/JsonFileStore.h"

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

class QTimer;
class QAbstractItemModel;
class QSortFilterProxyModel;

#include <functional>
#include <memory>
#include <vector>

namespace mole {

class VfsManager;
class TaskManager;
class IndexDatabase;
class IndexSummary;
class EventBus;
class PreviewRegistry;
class MetadataRegistry;
class ThumbnailRegistry;
class PluginManager;
class FileLauncher;
class DragSource;
class ActionRegistry;
class SessionStore;
class ScheduleStore;
class Scheduler;
class AlertStore;
class AnalysisStore;
class FileSetStore;
class Preferences;
class SecretStore;
class RemoteRegistry;
class UpdateCheck;

/// Owns the application's services and exposes them to QML as a single root
/// object. Construction order here is the application's startup sequence.
class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(mole::TabsModel* tabs READ tabs CONSTANT)
    /// What the window is painted in. `CONSTANT` because the palette itself never
    /// changes; the tokens inside it notify, which is what lets a theme repaint a
    /// running window. See Palette.h and ADR-0072.
    Q_PROPERTY(mole::Palette* colour READ colour CONSTANT)
    Q_PROPERTY(mole::DriveListModel* drives READ drives CONSTANT)
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
    //   monospaceSize      code and data, level with prose rather than under it:
    //                      a log or a source file is the thing left open longest,
    //                      and it may never read larger than prose
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
    /// True when this window is being photographed rather than used.
    ///
    /// A still picture cannot show something that breathes; it can only catch one
    /// arbitrary phase of it. A drive's dot pulses for ever while the drive is
    /// waiting or working, so the user guide's picture of the sidebar came out at a
    /// different opacity every time it was taken -- and the dot is in *every* one of
    /// the guide's pictures, so that put all of them at risk rather than a nameable
    /// few. Held still, the dot is drawn at the opacity it rests at, which
    /// is the honest still rendering of that state.
    ///
    /// Set from `MOLE_STILL_PICTURES`, the same way `MOLE_DRIVES` fixes the drive
    /// list and for the same reason. Nothing sets it in an ordinary run.
    /// See MOLE-255 and ADR-0063.
    Q_PROPERTY(bool stillPictures READ stillPictures CONSTANT)
    Q_PROPERTY(bool credentialsAvailable READ credentialsAvailable CONSTANT)
    Q_PROPERTY(bool credentialsExist READ credentialsExist NOTIFY credentialsChanged)
    Q_PROPERTY(bool credentialsUnlocked READ credentialsUnlocked NOTIFY credentialsChanged)
    Q_PROPERTY(bool credentialsNeeded READ credentialsNeeded NOTIFY credentialsChanged)
    /// True while a passphrase is being turned into a key. scrypt at this cost
    /// takes a noticeable fraction of a second *by design*, and it used to run
    /// in the dialog's button handler -- a window that stops, with nothing
    /// saying it is working. See ADR-0090.
    Q_PROPERTY(bool credentialsBusy READ credentialsBusy NOTIFY credentialsChanged)
    /// Properties, not plain callable methods. A method call in a QML binding
    /// is evaluated once and never again -- there is no signal to tell the
    /// binding it went stale -- so a drive saved through this dialog would
    /// never appear in its own list.
    /// The kinds of drive this build can serve. Its NOTIFY is
    /// driveKindsChanged() and nothing emits it, because the answer depends only
    /// on the registered factories and those are fixed once the plugins have
    /// loaded.
    ///
    /// **It used to be NOTIFY drivesChanged, and that signal was emitted from
    /// five mutators** as a general "something about drives moved" -- a
    /// connection, a disconnection, a save, a removal, an unlock -- none of which
    /// can change this list. Whatever needed that news was DriveListModel, which
    /// observes VfsManager::mountsChanged and RemoteRegistry::drivesChanged for
    /// itself; the sidebar's eject path never emitted it and works, which is what
    /// showed it was carrying nothing. See MOLE-395.
    Q_PROPERTY(QVariantList driveKinds READ driveKinds NOTIFY driveKindsChanged)
    /// A view over the drive list, narrowed to the ones somebody configured.
    /// A model rather than a list of maps, and CONSTANT rather than notifying:
    /// the pointer never changes and the model announces its own rows, so a
    /// binding cannot go stale the way a rebuilt list of maps could.
    Q_PROPERTY(QAbstractItemModel* configuredDrives READ configuredDrives CONSTANT)
    /// What the sidebar draws: every drive that has not been taken out of the
    /// list. See MOLE-311 and DriveListModel::ShownRole.
    Q_PROPERTY(QAbstractItemModel* sidebarDrives READ sidebarDrives CONSTANT)

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

    /// Opens the page the manifest named for the version last announced, and
    /// nothing else. False when no version has been announced.
    ///
    /// **The URL never passes through QML and is never assembled here.** It is the
    /// manifest's own field, opened verbatim -- which is the whole reason that
    /// field exists, and the reason the window is handed a version and not a link.
    Q_INVOKABLE bool openReleasePage();

    /// The final "hand this to whatever opens links" step. Replaceable so a test
    /// can assert what would have been opened without a browser appearing -- the
    /// same seam FileLauncher has, for the same reason.
    using LinkHook = std::function<bool(const QUrl&)>;
    void setLinkHook(LinkHook hook);

    /// Asks whether a newer Mole exists, at most once per run.
    ///
    /// Separate from initialise() and called by main.cpp once there is a window,
    /// for two reasons. A notice must never appear over a half-drawn window, and
    /// the answer can arrive at any moment. And every test that builds an
    /// AppController would otherwise make a request: nothing in `make test` may
    /// depend on GitHub being up. See UpdateCheck.h for what it does and does not
    /// say.
    void startUpdateCheck();

    UpdateCheck* updateCheck() const { return m_updateCheck; }

    TabsModel* tabs() const { return m_tabs; }
    Palette* colour() const { return m_colour; }
    /// Repaints the window and remembers the choice under `ui.theme`. The only
    /// thing that writes that key: a theme picked from the menu and a theme read
    /// at startup have to agree, and two writers is how they stop agreeing.
    void setTheme(const QString& name);
    DriveListModel* drives() const { return m_drives; }
    TaskListModel* tasks() const { return m_taskModel; }
    TerminalController* terminal() const { return m_terminal; }
    FeatureRegistry* features() const { return m_features; }
    CommandPaletteModel* commands() const { return m_commands; }
    BookmarkModel* bookmarks() const { return m_bookmarks; }
    PreviewRegistry* previews() const { return m_previews; }
    MetadataRegistry* metadata() const { return m_metadata; }
    ThumbnailRegistry* thumbnails() const { return m_thumbnails; }
    Scheduler* scheduler() const { return m_scheduler; }
    ScheduleStore* schedules() const { return m_schedules; }
    AlertStore* alerts() const { return m_alerts; }
    AnalysisStore* reports() const { return m_reports.get(); }
    FileSetStore* sets() const { return m_sets; }
    RemoteRegistry* remotes() const { return m_remotes; }

    bool stillPictures() const { return m_stillPictures; }
    bool credentialsAvailable() const;
    bool credentialsExist() const;
    bool credentialsUnlocked() const;
    /// True when a configured drive cannot connect until the store is opened.
    bool credentialsNeeded() const;

    /// Creates the store with this passphrase, or opens an existing one.
    /// Starts opening the store, or creating one. The answer arrives as
    /// `credentialsAttempted`, because the derivation happens on a task.
    Q_INVOKABLE void unlockCredentials(const QString& passphrase);
    /// Why the last passphrase attempt failed, for the unlock dialog.
    ///
    /// **This is the passphrase dialog's message and nothing else's.** Saving a
    /// drive used to write its failure here too, so a refused endpoint was shown
    /// by the box asking for a passphrase. See MOLE-395.
    Q_INVOKABLE QString credentialsError() const { return m_credentialsError; }
    bool credentialsBusy() const { return m_credentialsBusy; }

    // ---- configured drives ----------------------------------------------

    /// The kinds of drive that can be added, from every registered factory.
    QVariantList driveKinds() const;
    /// The form for one kind, as field descriptions.
    Q_INVOKABLE QVariantList driveFields(const QString& factoryScheme, const QString& variant) const;
    /// The drives already configured, as rows of the one drive model rather
    /// than a second list carrying its own copy of what state each is in. Two
    /// implementations of one piece of state is how the two drift apart.
    QAbstractItemModel* configuredDrives() const;
    QAbstractItemModel* sidebarDrives() const;
    /// What was typed to make this drive: the factory, the variant, the root
    /// and the settings, with no secrets and no state. Asked for by id when a
    /// row is opened, rather than carried on every row all the time.
    Q_INVOKABLE QVariantMap driveConfiguration(const QString& id) const;
    /// Adds or updates one. `values` holds every field; the secret ones are
    /// separated here rather than by the caller, so a view cannot get it wrong.
    Q_INVOKABLE bool saveDrive(const QString& id, const QString& name, const QString& factoryScheme,
        const QString& variant, const QString& root, const QVariantMap& values);
    Q_INVOKABLE bool removeDrive(const QString& id);
    /// Connects one now. Returns an empty string on success, else the reason.
    ///
    /// "Connected" here means the backend was built, not that the far end
    /// answered — building one performs no I/O. checkDrive() is what asks.
    Q_INVOKABLE QString connectDrive(const QString& id);
    Q_INVOKABLE void disconnectDrive(const QString& id);

    /// Asks the shell for the passphrase. The key on a locked row and the
    /// palette's Unlock command both come through here, so there is one dialog
    /// rather than two ways of doing the same thing.
    Q_INVOKABLE void requestCredentials() { emit credentialsRequested(); }
    /// Forgets where the store was being opened on the way to. Called when
    /// somebody backs out of the passphrase dialog: the navigation they asked
    /// for is not owed to them a session later.
    Q_INVOKABLE void abandonPendingNavigation() { m_pendingNavigation.clear(); }

    /// Asks whether a saved drive can actually be reached, in the background,
    /// and reports through driveChecked(). Runs automatically after saveDrive(),
    /// so a configuration is verified where it was entered rather than several
    /// steps later when something finally tries to read from it.
    Q_INVOKABLE void checkDrive(const QString& id);

    /// Asks a drive what it is still holding that no listing shows, and -- when
    /// `discard` is set -- throws it away. Reports through driveSwept().
    ///
    /// An upload interrupted by the machine losing power is the case: S3 keeps
    /// the parts and charges for them until somebody completes or abandons the
    /// upload, and they are not objects, so nothing that lists a bucket will
    /// mention them. Two steps rather than one, because what is found is
    /// somebody's and one of them may be a copy running on another machine right
    /// now -- the first call reports, the second acts.
    ///
    /// A day old by default, which is the same rule stated in
    /// IFileSystem::leftovers(): younger than that and it cannot be told apart
    /// from an upload in flight.
    Q_INVOKABLE void sweepDrive(const QString& id, bool discard = false, int olderThanHours = 24);

    // ---- putting a location on the clipboard -------------------------------

    /// A location as text worth pasting: the native path for something on local
    /// disk, and the uri for anything else.
    ///
    /// A remote drive has no native path, and handing out the path part alone
    /// would produce something that looks local and is not -- "/reports/2026"
    /// pasted into a terminal means a directory that does not exist rather than a
    /// folder in a bucket. The uri says where it is.
    /// "1 item", "3 items" -- a count with the right word after it.
    ///
    /// **Nine places wrote `count + " items"`** with no singular branch, so a
    /// folder holding one file read "1 items" in the pane's status line, and the
    /// same in the sets, analysis, reports, live-search, indexes and duplicates
    /// views. Six other sites in the same files took the trouble, which is the
    /// shape of a thing that wants one function. Here rather than in QML because
    /// the same question is asked from C++ too. See MOLE-398.
    Q_INVOKABLE QString countOf(int count, const QString& singular, const QString& plural) const;

    Q_INVOKABLE QString pathTextFor(const QString& uri) const;

    /// The other direction: what somebody typed, as a uri.
    ///
    /// A bare path means the local disk and anything with a scheme is passed
    /// through, which is what three QML files each used to work out for
    /// themselves with `"file://" + value`. That expression is right on Linux
    /// and wrong everywhere else -- typing C:\Users\me into the path bar
    /// yields file://C:\Users\me, whose authority is "C:" and whose path is a
    /// run of backslashes -- and it has to agree with pathTextFor(), which is
    /// hard to arrange in three copies written in a language that does not have
    /// the type.
    ///
    /// Empty in, empty out. Text with a scheme that does not parse comes back as
    /// it was typed, so the caller reports what the user asked for rather than
    /// an empty string.
    Q_INVOKABLE QString uriForPathText(const QString& text) const;

    /// Copies the folder currently open in the active pane. Returns what was
    /// copied, or an empty string when there was nothing to copy.
    Q_INVOKABLE QString copyCurrentFolderPath();
    /// Copies the file under the cursor. Empty when a directory is there instead:
    /// the folder actions already cover that, and silently copying something else
    /// than what was asked for is worse than doing nothing.
    Q_INVOKABLE QString copySelectedFilePath();
    /// Copies the root of the drive the active pane is looking at.
    Q_INVOKABLE QString copyDriveRootPath();
    const PluginServices& services() const { return m_services; }

    QString monospaceFont() const;
    int headingSize() const { return 17; }
    int textSize() const { return 14; }
    int secondaryTextSize() const { return 13; }
    int smallTextSize() const { return 11; }
    int monospaceSize() const { return 14; }
    int listRowHeight() const { return qRound(textSize() * 2.2); }
    int minimumTarget() const { return 28; }

    QStringList pluginSummary() const;
    QStringList pluginErrors() const;

    /// Writes what this run started with to the session log: the build, the
    /// plugins, the drives and the indexes.
    ///
    /// A log could not answer the three questions anybody asks of a report before
    /// reading a line of it -- which build is this, what did it load, what drives
    /// were there -- and every one of those facts was already assembled at
    /// startup, two of them already formatted by `runDiagnostics()`, which prints
    /// to the console and nowhere else. So a report from a packaged build could
    /// not say whether a plugin had failed to load, which is the case that
    /// function was written for.
    ///
    /// **Called from initialise(), not from main.cpp**, and deliberately before
    /// `Scheduler::start()`. That call runs `checkDue()` synchronously, so a due
    /// index rule submits its scan there and then -- and the scan holds the
    /// index's one mutex for as long as the walk lasts, which would make the
    /// `volumes()` read below wait for it. See MOLE-264. Before the scheduler is
    /// the one moment when every fact is known and nothing is competing for the
    /// index.
    ///
    /// Nothing here may be a credential or a path outside the profile: a log is a
    /// file people send to other people. A drive's display name is a name the
    /// user chose and is fine; its uri is not, so only the scheme is written.
    void recordStartup() const;
    /// The session log's "Indexes:" line. Separate from recordStartup() because
    /// it can only be written once the snapshot has read the index, which is a
    /// task round trip after startup. See ADR-0066.
    void recordIndexes() const;

    /// Opens a browser tab already pointing at `uri`.
    /// Opens a tab and, when the new tab has somewhere to start from, points
    /// it at wherever the user currently is. A search opened from a folder
    /// should search that folder, not a default nobody asked for.
    Q_INVOKABLE int openFeatureTab(const QString& featureId);

    /// Shows the one tab of `featureId`, opening it only if it is not there.
    ///
    /// For the standing tools ADR-0032 names -- the alerts list, the saved
    /// reports, the schedule, the sets. Opening a second tab of one leaves two
    /// controllers over the same store, each with its own idea of what is
    /// current, and the user with two tabs that look the same.
    ///
    /// Asked for by name rather than decided from a predicate on IFeature: that
    /// would be a reshaping of the extension point, and reusing on the existing
    /// opensFromNothing() would be wrong -- a duplicate scan and a bulk rename
    /// also answer false, and reusing a Duplicates tab halfway through a scan
    /// would throw the scan away. See MOLE-206.
    Q_INVOKABLE int openStandingTab(const QString& featureId);

    /// Opens the search with its scope set to every indexed volume.
    ///
    /// There used to be a second search tab behind Ctrl+Shift+I. The question it
    /// asked is a field on the one form now, and this is the key still landing
    /// on it -- a preset, not a second tool. Returns the row, or -1.
    Q_INVOKABLE int openSearchEverywhere();

    /// The three standing tabs, by name rather than by id.
    ///
    /// **QML spelled `mole.browser`, `mole.commander` and `mole.livesearch` as
    /// string literals**, in two files, beside a fourth key that called
    /// openSearchEverywhere() -- and `openFeatureTab()` with an id nothing knows
    /// returns -1 in silence, so a rename broke two keys with no diagnostic
    /// anywhere. The id belongs in the layer that knows what a feature is. See
    /// MOLE-396 and ADR-0032, which is why the shell may know these three at all.
    Q_INVOKABLE int openNewBrowser();
    Q_INVOKABLE int openNewCommander();
    Q_INVOKABLE int openSearchHere();

    /// One of the window's keys, handed back by a view that had to steal it.
    ///
    /// A read-only `TextArea` is still an editor as far as Qt's shortcut
    /// override is concerned, so it swallows every key in the standard editing
    /// bindings -- `Ctrl+W` is DeleteStartOfWord, and clicking into a preview
    /// stopped Ctrl+W from closing the tab. `ViewerKeys.qml` hands those back,
    /// and it did it with **a second copy of the window's key table written as a
    /// QML switch**: one that already disagreed with Main.qml about nine keys.
    /// ADR-0002 accepted the duplication as the lesser evil; this is the same
    /// arrangement with the table in the layer that owns the actions, so the two
    /// lists can be compared and a key added to the window is one line.
    ///
    /// Returns true when the key was acted on, in which case the caller marks
    /// the event accepted so the text control never sees it. Only keys a viewer
    /// has no use for: Ctrl+C, Ctrl+A and Ctrl+Home are left alone.
    Q_INVOKABLE bool relayWindowKey(int key, int modifiers);

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
    /// What an operation acts on: the ticked entries, or the row under the cursor
    /// when nothing is ticked -- the rule F5, F8 and analysing already follow. Empty
    /// only when there is no row at all, which is when "the folder in view" applies.
    Q_INVOKABLE QStringList currentTargetsOrCursor() const;
    /// A sentence naming what would be packed, for the dialog to show before
    /// anything happens.
    Q_INVOKABLE QString compressionSubject() const;
    /// Exactly what would be packed, one entry per line: `name` and `isDir`, in the
    /// order it would be written. A count is a summary; this is the answer.
    Q_INVOKABLE QVariantList compressionTargets() const;
    /// False for formats that cannot carry a password at all, so the interface can
    /// say so rather than offering a box that would be quietly ignored.
    Q_INVOKABLE bool formatSupportsPassword(const QString& format) const;

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
    ///
    /// A configured drive with nothing mounted behind it is connected on the way,
    /// because opening a drive is what asking for it means. When it needs the
    /// credential store and the store is shut, the passphrase is asked for and
    /// the navigation follows once it is open — rather than showing the drive as
    /// a folder with nothing in it, which is what it used to do.
    ///
    /// Returns whether the navigation actually happened, so a caller that hands
    /// the keyboard to wherever it just went does not hand it to a place nobody
    /// arrived at. Taking the keyboard out of the dialog that just went up leaves
    /// that dialog holding no focus, and then nothing inside it can take the
    /// keyboard either.
    Q_INVOKABLE bool goTo(const QString& uri);

    /// Goes to whatever a saved place points at, `kind` saying how to read
    /// `target` -- the two the bookmark model hands out. See ADR-0061.
    ///
    /// Taken before goTo(), which would try to parse a set's id as a uri and
    /// fail with a message about a drive. A drive row and a folder bookmark are
    /// kind "folder" and go straight through, so there is one path rather than
    /// one per caller. False, having said so, for a bookmark whose set has been
    /// deleted. See MOLE-208.
    Q_INVOKABLE bool openPlace(const QString& kind, const QString& target);
    /// Opens the folder holding this file with the cursor on it -- what "show me
    /// where this is" means at the end of a search.
    Q_INVOKABLE void revealFile(const QString& fileUri);
    /// Mounts an archive as a drive and opens it. Returns the new mount's root
    /// uri, or an empty string on failure.
    Q_INVOKABLE QString openArchive(const QString& archiveUri);
    /// True when this file can be mounted as a drive.
    Q_INVOKABLE bool isMountableArchive(const QString& uri) const;
    /// Totals up the ticked folders, or every folder in the listing when none
    /// are ticked, and writes the answers into the rows as they arrive.
    Q_INVOKABLE void measureFolderSizes();
    /// True when this build can write archives at all, so the interface can offer
    /// the operation or leave it out rather than offering it and then failing.
    Q_INVOKABLE bool canCompress() const;
    /// What formats to offer, in the order to offer them.
    Q_INVOKABLE QStringList compressionFormats() const;
    /// A name for the archive worked out from what is selected, so the dialog opens
    /// with something sensible in it.
    Q_INVOKABLE QString suggestedArchiveName(const QString& format) const;
    /// The name already in the box, wearing the suffix a newly chosen format wants.
    /// Changing the kind must not discard a name somebody typed -- which it did,
    /// until a bug report said so.
    Q_INVOKABLE QString archiveNameForFormat(const QString& currentName, const QString& format) const;
    /// True for a format with no container: a bare `.xz` holds one file and no
    /// folders, so the dialog can say so instead of failing on Ok.
    Q_INVOKABLE bool formatTakesOneFileOnly(const QString& format) const;
    /// Packs the selection, or the folder in view when nothing is selected, into a
    /// new archive beside it. With `removeSources`, the originals go once the archive
    /// is written -- and only then, and only if every one of them could be read.
    Q_INVOKABLE void compressSelection(const QString& archiveName, const QString& format,
        const QString& passphrase = {}, bool removeSources = false);
    /// Starts a scan with no dialog in the way, for whoever already knows what
    /// they are asking for.
    ///
    /// Incremental and archive-listing by default, which is what the *Index a
    /// folder* dialog opens on: this used to set nothing at all, so every call
    /// walked the whole tree and recorded nothing about the files. Metadata is
    /// off, because one read per file is bounded per file and unbounded in
    /// aggregate -- it is asked for, in the dialog. See ADR-0056.
    Q_INVOKABLE void queueScan(const QString& uri, const QString& label);

    // ---- application menu -------------------------------------------------

    /// Sections and entries for the menu, rebuilt on every call so tick boxes
    /// and greyed-out entries reflect the tab that is open right now.
    Q_INVOKABLE QVariantList buildMenu();
    Q_INVOKABLE bool triggerAction(const QString& id);
    ActionRegistry* actions() const { return m_actions; }
    /// The small things the application remembers about how somebody likes to
    /// work, including whether it looks for a newer version of itself.
    Preferences* preferences() const { return m_preferences; }

    /// Hands a file to the desktop's default application for its type. Files
    /// on a remote or archive drive are extracted to a scratch copy first.
    Q_INVOKABLE void openExternally(const QString& uri);

    /// Hands `uris` to whatever the pointer is over, as a copy. Called when a
    /// press in a listing has become a drag; what it turns into is
    /// `DragSource`'s business and what it may not do is delete anything.
    Q_INVOKABLE void startDrag(const QStringList& uris);

    QString defaultLocation() const;
    FileLauncher* launcher() const { return m_launcher; }
    /// Exposed so the shell can install the step that builds the real `QDrag`,
    /// and so a test can install a recorder instead -- see ADR-0040.
    DragSource* dragSource() const { return m_dragSource; }

    // ---- window geometry --------------------------------------------------

    /// Geometry to restore, as `{ width, height, x, y, windowState }`, where the
    /// state is "normal", "maximized" or "fullscreen". Empty when there is
    /// nothing usable to restore.
    Q_INVOKABLE QVariantMap savedWindowGeometry() const;
    /// Called by the shell whenever the window is moved, resized, maximised or
    /// taken full-screen. `visibility` is the window's own `QWindow::Visibility`
    /// -- passed through rather than reduced to a flag by the caller, because
    /// reducing it is exactly how full-screen came to be recorded as "not
    /// maximised, so this must be the size the user chose".
    Q_INVOKABLE void rememberWindowGeometry(int x, int y, int width, int height, int visibility);

    // ---- the keys that act on the pane in front of you --------------------
    //
    // Routed through the active tab's active pane rather than through whatever
    // holds the keyboard, because that is what they mean. currentFile() and
    // currentLocation() already resolve their target the same way and have
    // always worked from the menu, wherever the focus was; these are the same
    // operations reached by a key.
    //
    // See docs/adr/0019-the-keys-that-belong-to-the-window.md for which keys
    // are here and which stay in the pane.

    /// Previews the file under the pane's cursor, or opens it when it is a
    /// folder -- a key that does nothing is indistinguishable from one that is
    /// broken, and there is nothing to preview about a directory.
    Q_INVOKABLE void previewCurrent();
    Q_INVOKABLE void goUpInCurrentPane();
    Q_INVOKABLE void goBackInCurrentPane();
    Q_INVOKABLE void goForwardInCurrentPane();

    /// True when this rectangle still overlaps a screen that exists. A window
    /// restored onto an unplugged monitor is invisible and unrecoverable.
    static bool geometryIsOnScreen(int x, int y, int width, int height);
    /// Qt's `QWindow::Visibility` reduced to the three states worth restoring.
    static WindowState windowStateOf(int visibility);

    /// Writes the open tabs out now, rather than waiting for the debounce.
    /// Called on shutdown; exposed so tests do not have to wait either.
    Q_INVOKABLE void saveSessionNow();

signals:
    void credentialsChanged();
    /// The answer to unlockCredentials(), once the derivation has finished.
    /// False leaves the reason in credentialsError().
    void credentialsAttempted(bool ok);
    /// Only when the set of registered factories changes, which is never after
    /// startup today. See the driveKinds property.
    void driveKindsChanged();
    /// A checkDrive() finished. `message` is ready to show as it stands: what was
    /// found, or why the drive could not be reached.
    void driveChecked(const QString& id, bool reachable, const QString& message);
    /// A sweepDrive() finished. `found` is how many leftovers there are, and
    /// `message` is ready to show as it stands.
    void driveSwept(const QString& id, int found, bool cleared, const QString& message);
    /// The shell should show the drives dialog. A signal rather than a direct
    /// call, because this layer has no business knowing what a dialog is.
    void drivesRequested();
    /// The shell should ask for the passphrase. Same reasoning: this layer does
    /// not know there is a dialog, only that somebody asked to do something that
    /// needs the store open.
    void credentialsRequested();
    void notification(int severity, const QString& title, const QString& detail);
    /// A newer Mole exists, and this is the only place the window is told.
    ///
    /// Separate from notification() rather than a ninth caller of it, because this
    /// notice has somewhere to go: the toast grows a button and stops counting
    /// down, since a link that disappears after five seconds is not a link. The
    /// page itself does not come through here -- see openReleasePage(). Shown once
    /// per version; UpdateCheck decides when, and being shown is what counts as
    /// having announced it. See MOLE-325.
    void versionAvailable(const QString& version);
    /// The shell asks for the little dialog: a name and a format are the user's to
    /// choose, and a controller has no business owning a window.
    void compressionRequested();
    /// A menu entry whose body is a QML dialog was picked; the shell decides
    /// which one to show. Keeps dialog markup out of C++.
    void dialogRequested(const QString& actionId);
    /// *Index this folder* was picked in a browser pane. The search tab is
    /// already open by the time this is emitted; the shell opens its *Index a
    /// folder* dialog on `uri`, so the four questions a scan raises are asked
    /// once, in one place, however somebody arrived at it.
    void indexFolderRequested(const QString& uri, const QString& label);

private slots:
    /// Rebuilds the entries a drive contributes for the row under the cursor.
    ///
    /// Nothing here knows what any of them do: the ids and the titles come from
    /// the drive, and picking one hands the id straight back to it.
    void refreshDriveActions();
    /// Follows the pane the keyboard is in, so what a drive offers reaches the
    /// menu -- and the command palette, and a shortcut -- without the menu having
    /// been opened first.
    ///
    /// A slot and a signal signature rather than a cast, like everything else the
    /// shell asks of a tab: a tab kind that has no panes simply does not answer.
    void watchActivePane();

private:
    /// Turns a store's "that did not reach the disk" into something the user
    /// sees, and its "I could not read this and kept it" into the same.
    ///
    /// One place for all of them: every store used to call save() as a
    /// statement, so a full disk or a read-only configuration directory left the
    /// model changed, the window showing the change, and the file as it was. See
    /// ADR-0089.
    void watchStore(JsonFileStore* store);
    /// The half of unlockCredentials() that runs once the derivation is done,
    /// back on the thread that owns everything it touches.
    void finishUnlock(bool ok);

    void mountDefaultDrives();
    void registerShellActions();

    /// Whether there is a drive behind `uri` ready to be read from.
    enum class DriveReadiness {
        Ready, ///< mounted, or there is no configured drive here at all
        Waiting, ///< it needs the credential store, and the store is shut
        Failed, ///< there is a drive, and connecting it did not work
    };
    /// Connects the configured drive behind `uri` when nothing is mounted there.
    /// Says which of the three happened rather than returning a bare bool: the
    /// caller has something different to do in each case, and "false" for both
    /// "ask for the passphrase" and "it is broken" is how an unconnected drive
    /// came to look like an empty folder.
    DriveReadiness prepareDriveFor(const QString& uri);
    /// Arranges a drive for somewhere in the window that is pointed at one and
    /// found nothing mounted -- a tab restored before its drive was connected.
    ///
    /// The same readiness step goTo() uses, and deliberately no navigation:
    /// whoever asked is waiting on the mount table, not on being taken anywhere,
    /// and a background tab must not be able to drag the foreground one to its
    /// own folder. See MOLE-351.
    void arrangeDriveFor(const VfsUri& target);
    /// The row of the browser tab a tab with no browsing of its own should send
    /// somebody to: the one it opened before, or a new one. Returns -1 when a
    /// browser tab cannot be opened at all.
    ///
    /// A search is the case this exists for. Twenty results examined has to
    /// leave one browser tab, not twenty, and the search itself has to be
    /// waiting where it was when the user comes back to it.
    int browserTabForCurrent();
    /// Navigates the current tab when it can navigate, and opens a browser tab
    /// otherwise. The rule goTo() has always followed, shared so that opening an
    /// archive follows it too -- see MOLE-393.
    void navigateHereOrOpenABrowser(const QString& uri);
    /// Restores the previous tabs, or opens a default one. Returns false when
    /// there was nothing to restore.
    bool restoreSession();
    /// Rebuilds the one-entry-per-bookmark part of the menu.
    void refreshBookmarkActions();
    /// The set the current tab is about, or empty when it is not about one.
    QString currentSetId() const;
    /// Where the current tab is pointing, or an empty string for a tab that
    /// has no location.
    QString currentLocation() const;
    /// The file under the cursor in the current tab, if it has one.
    QString currentFile() const;
    /// Copies one location and says which one, so the three copy actions are
    /// distinguishable from each other. Returns the copied text.
    QString copyPathAndSay(const QString& uri, const QString& title);
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
    /// What the interface knows about the index. Owned as a child.
    IndexSummary* m_indexSummary = nullptr;
    EventBus* m_events = nullptr;
    FeatureRegistry* m_features = nullptr;
    PreviewRegistry* m_previews = nullptr;
    MetadataRegistry* m_metadata = nullptr;
    ThumbnailRegistry* m_thumbnails = nullptr;
    PluginManager* m_plugins = nullptr;
    FileLauncher* m_launcher = nullptr;
    DragSource* m_dragSource = nullptr;
    ActionRegistry* m_actions = nullptr;
    TerminalController* m_terminal = nullptr;
    ScheduleStore* m_schedules = nullptr;
    Scheduler* m_scheduler = nullptr;
    AlertStore* m_alerts = nullptr;
    std::unique_ptr<AnalysisStore> m_reports;
    FileSetStore* m_sets = nullptr;
    Preferences* m_preferences = nullptr;
    UpdateCheck* m_updateCheck = nullptr;
    /// The landing page of the version last announced, and the only copy of it.
    QUrl m_releasePage;
    LinkHook m_linkHook;
    SecretStore* m_secrets = nullptr;
    RemoteRegistry* m_remotes = nullptr;
    QString m_credentialsError;
    bool m_credentialsBusy = false;
    /// Where goTo() was heading when it found the store shut, so unlocking can
    /// finish the journey rather than leaving the user to ask twice.
    QString m_pendingNavigation;
    /// Drives something in the window is waiting for and could not have, because
    /// their secrets are in a store that was still shut. Connected once it opens.
    QStringList m_drivesWanted;
    BookmarkModel* m_bookmarks = nullptr;
    CommandPaletteModel* m_commands = nullptr;
    WindowGeometry m_window;
    std::unique_ptr<SessionStore> m_session;
    /// Navigation fires state changes constantly, so writes are coalesced.
    QTimer* m_sessionSaveTimer = nullptr;
    /// Restoring opens tabs, which would immediately mark the session dirty.
    bool m_restoring = false;
    /// Whether anything has changed the session since it was read.
    ///
    /// The destructor saves only when it has. `mole --plugins` and
    /// `mole --diagnostics` build this controller, initialise it and return, and
    /// the save on the way out used to rewrite the user's session from a run that
    /// opened no window -- without the tabs of any plugin that had failed to load.
    /// See MOLE-350.
    bool m_sessionTouched = false;
    const bool m_stillPictures = !qEnvironmentVariableIsEmpty("MOLE_STILL_PICTURES");
    TabsModel* m_tabs = nullptr;
    Palette* m_colour = nullptr;
    /// Tells the drive list which locations the window has open, so a drive can
    /// say that somebody is on it. Called when a tab moves, opens or closes, and
    /// when the mount table changes -- never on a timer.
    void refreshOpenDrives();
    /// Mounts an archive as somewhere to walk around in rather than as a drive.
    /// See Mount::unlisted.
    QString mountForBrowsing(IFileSystemFactory& factory, const QString& localPath, QString* errorOut);
    /// Takes away the unlisted mounts nobody is inside any more.
    void dropUnusedBrowsingMounts(const QList<VfsUri>& open);
    /// The same, from the two signals that can change the answer.
    void pruneBrowsingMounts();
    /// Where every tab is standing, right now.
    QList<VfsUri> openLocationsNow() const;
    /// The unlisted mounts something has actually been inside, so one on its way
    /// to being occupied is not taken away before it gets there. See
    /// dropUnusedBrowsingMounts().

    DriveListModel* m_drives = nullptr;
    QSortFilterProxyModel* m_configuredDrives = nullptr;
    QSortFilterProxyModel* m_sidebarDrives = nullptr;
    TaskListModel* m_taskModel = nullptr;
    PluginServices m_services;
};

} // namespace mole
