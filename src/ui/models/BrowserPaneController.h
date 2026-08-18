#pragma once

#include "sdk/PluginServices.h"
#include "ui/models/FileListModel.h"
#include "ui/models/RepositoryInfo.h"

#include "core/vfs/VfsUri.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

namespace mole {

class ListDirectoryTask;
class ReadRepositoryTask;
class ReadStatusTask;

/// One navigable pane: current location, history, and the listing shown in it.
///
/// A split view is two of these side by side; a single view is one. Nothing
/// here knows which, so adding a three-pane layout later is a QML change.
class BrowserPaneController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(mole::FileListModel* files READ files CONSTANT)
    /// What git says about the folder in view. Never null: it reports that there
    /// is nothing to say rather than being absent, so a binding never has to
    /// check.
    Q_PROPERTY(mole::RepositoryInfo* repository READ repository CONSTANT)
    Q_PROPERTY(QString currentUri READ currentUri NOTIFY locationChanged)
    Q_PROPERTY(QString displayPath READ displayPath NOTIFY locationChanged)
    Q_PROPERTY(QString locationName READ locationName NOTIFY locationChanged)
    /// The path broken into clickable pieces, each with the uri it leads to.
    /// Pressing Backspace three times to get up three levels is work the
    /// interface can do for the user.
    Q_PROPERTY(QVariantList pathSegments READ pathSegments NOTIFY locationChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY historyChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY historyChanged)
    Q_PROPERTY(bool canGoUp READ canGoUp NOTIFY locationChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentIndexChanged)
    Q_PROPERTY(bool writable READ isWritable NOTIFY locationChanged)

public:
    BrowserPaneController(PluginServices services, QObject* parent = nullptr);
    ~BrowserPaneController() override;

    FileListModel* files() const { return m_files; }
    RepositoryInfo* repository() const { return m_repository; }
    QString currentUri() const { return m_current.toString(); }
    QString displayPath() const;
    QString locationName() const;
    QVariantList pathSegments() const;
    bool isLoading() const { return m_loading; }
    QString errorText() const { return m_errorText; }
    bool canGoBack() const { return m_historyIndex > 0; }
    bool canGoForward() const { return m_historyIndex + 1 < m_history.size(); }
    bool canGoUp() const { return m_current.isValid() && !m_current.isRoot(); }

    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int index);
    QString currentName() const;
    /// False for read-only drives such as a mounted archive.
    bool isWritable() const;

    Q_INVOKABLE void navigateTo(const QString& uri);
    /// Navigates to the folder holding `fileUri` and puts the cursor on the file
    /// once the listing arrives. The end of most searches is "show me where this
    /// is", and arriving in the right folder with the cursor somewhere else is only
    /// half an answer.
    Q_INVOKABLE void revealFile(const QString& fileUri);
    /// Navigates to the folder that *held* `fileUri` and leaves the cursor on
    /// nothing.
    ///
    /// What "go to" means for a file that is not there -- a deletion git knows
    /// about and the disk does not. The folder is the true half of the answer and
    /// the cursor is the false half: dropping it on whatever happens to sort first
    /// would be pointing at a different file and looking exactly like success. See
    /// docs/adr/0042-a-deletion-is-reachable-from-the-band.md.
    Q_INVOKABLE void revealMissingFile(const QString& fileUri);
    /// Moves the cursor by `delta` rows, clamped to the listing.
    Q_INVOKABLE void moveCursor(int delta);
    Q_INVOKABLE void cursorToStart();
    Q_INVOKABLE void cursorToEnd();
    /// Ticks the row under the cursor and steps down, like Insert in a
    /// commander-style manager.
    Q_INVOKABLE void toggleSelectionAndAdvance();

    // ---- operations on this pane ----------------------------------------
    //
    // Each queues a Task and announces the result on the event bus, so other
    // panes showing the same directory refresh themselves.

    Q_INVOKABLE void createDirectory(const QString& name);
    Q_INVOKABLE void renameCurrent(const QString& newName);
    Q_INVOKABLE void deleteTargets();
    /// The selection, or the row under the cursor when nothing is ticked.
    QList<VfsUri> targets() const;
    /// What a drag that started on `row` carries: the ticked rows when `row` is
    /// one of them, and that row alone when it is not.
    ///
    /// The same selection targets() reads, so the two can never disagree about
    /// what is ticked; what differs is the fallback, and deliberately. F5 acts on
    /// the row under the *cursor*, a drag on the row under the *pointer* -- and
    /// dragging an unticked row must not quietly send a selection the user made
    /// somewhere else in the list.
    Q_INVOKABLE QStringList dragTargets(int row) const;
    Q_INVOKABLE int targetCount() const;
    Q_INVOKABLE QString targetSummary() const;
    /// Exactly what an operation would act on -- name, folder or not, and size --
    /// so a dialog can show it instead of a count. Same source as targets(), so
    /// what is listed is what would happen and not a second opinion about it.
    Q_INVOKABLE QVariantList targetDetails() const;
    // ---- what a drop means ----------------------------------------------
    //
    // A drop is a copy into the folder this pane is showing, and it is never a
    // move: Mole does not delete another application's file because something
    // was dragged out of it, whatever action the source offered.

    /// What a drop of `urls` would do here: `count`, `sizeText`, `targetPath`,
    /// `singleName`, `writable`, and the `collisions` -- names already taken in
    /// this folder.
    ///
    /// Answered from the listing this pane has already loaded and one stat per
    /// dropped file, so it costs nothing and can be shown while the pointer is
    /// still moving, which is the only moment it can change what somebody does.
    Q_INVOKABLE QVariantMap dropPlan(const QStringList& urls) const;
    /// Copies `urls` into the folder this pane is showing. `conflict` is "skip",
    /// "overwrite" or "stop", exactly as `runTransfer()` takes them; "stop" is
    /// the default and means nothing is overwritten without an answer.
    Q_INVOKABLE void dropHere(const QStringList& urls, const QString& conflict = QStringLiteral("stop"));

    /// Enters the row if it is a directory. Returns false for files, letting
    /// the caller decide what "open" means for them.
    Q_INVOKABLE bool activate(int row);
    Q_INVOKABLE void goUp();
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void refresh();

signals:
    void locationChanged();
    void currentIndexChanged();
    void operationFailed(const QString& message);
    void loadingChanged();
    void errorTextChanged();
    void historyChanged();
    /// A non-directory row was activated.
    void fileActivated(const QString& uri);

private:
    /// The rows of a dropped payload this pane could actually take: the `file://`
    /// ones that are not already sitting in this folder. `alreadyHereOut` counts
    /// the ones left out for being here already, which is a different answer from
    /// a payload with no files in it and has to be told apart from it.
    QList<VfsUri> droppedRows(const QStringList& urls, int* alreadyHereOut = nullptr) const;

    void load(const VfsUri& uri, bool recordHistory);
    /// Notes where the cursor is before leaving `from` for `to`, so coming back
    /// -- or stepping up out of a folder -- lands where the user was rather
    /// than at the top of the list.
    void rememberCursor(const VfsUri& from, const VfsUri& to);
    /// Marks rows that already have a report or an alert, so the listing shows
    /// it without the user opening anything.
    void annotateListing(const FileEntryList& entries);
    /// Asks git about the folder now in view, on a worker.
    ///
    /// Local drives only: libgit2 wants a real filesystem path, and pulling
    /// `.git` across SFTP to decorate a listing is the wrong trade -- the same
    /// reason the sidebar draws no capacity bar for a bucket. Anything that is not
    /// a `file://` uri leaves the pane with nothing to say, which is what makes
    /// the band absent there. See ADR-0041.
    void readRepository();
    /// Asks what has changed in the work tree at `root`, on a worker.
    ///
    /// Chained off the branch read rather than run beside it: the branch is a
    /// handful of reference reads and belongs on the band at once, and this stats
    /// every file git tracks. An answer already in RepositoryStatusCache is taken
    /// as it is, which is what makes moving between folders in one checkout free.
    void readStatus(const QString& root);
    /// Notices that something under `path` was written, and re-reads the work tree
    /// if that path is inside it.
    ///
    /// Every write Mole performs already announces itself on the EventBus, which is
    /// what this listens to -- a copy, a move, a delete, a rename, a new folder. The
    /// moment the task reports done, what git said about that tree is out of date,
    /// and a listing that calls a file unchanged after Mole itself wrote over it is
    /// the one failure mode this feature has.
    void noteWrittenInto(const VfsUri& path);
    /// Forgets the work tree's status and asks for it again, once the writing stops.
    void invalidateStatus();
    /// Points the watcher at the repository directory of the work tree in view, so a
    /// commit made outside Mole is noticed.
    void watchRepositoryDirectory(const QString& gitDir);
    /// The entry to select on arrival, or an empty string for "the first row".
    /// Set by revealFile(), consumed by the next listing that lands.
    QString m_pendingReveal;
    /// Whether the pending reveal is for something not expected to be there, so a
    /// listing that does not hold it leaves the cursor nowhere rather than falling
    /// back to the first row. Set by revealMissingFile().
    bool m_pendingRevealMissing = false;

    QString rememberedCursor(const VfsUri& folder) const;
    void setLoading(bool loading);

    /// Folder uri to the entry uri the cursor was on. Bounded: walking a large
    /// tree would otherwise accumulate an entry per folder visited, for a
    /// convenience nobody would miss beyond the last few hundred.
    static constexpr int kCursorMemoryLimit = 300;
    QHash<QString, QString> m_cursorMemory;
    QList<QString> m_cursorMemoryOrder;
    void setErrorText(const QString& text);

    /// How long the pane waits after a write before walking the tree again.
    ///
    /// Copying two hundred files into a checkout finishes two hundred tasks, and a
    /// walk per task would make this feature cost more than the copy it is
    /// describing. Four hundred milliseconds is longer than the gap between two
    /// files of an ordinary bulk copy, so a burst collapses into one walk, and short
    /// enough that a single F5 looks immediate. The timer is started rather than
    /// restarted on each event, so a copy that runs for minutes still refreshes as
    /// it goes instead of showing nothing until it finishes.
    static constexpr int kStatusFloorMs = 400;

    PluginServices m_services;
    FileListModel* m_files = nullptr;
    RepositoryInfo* m_repository = nullptr;
    VfsUri m_current;
    QStringList m_history;
    int m_historyIndex = -1;
    int m_currentIndex = -1;
    bool m_loading = false;
    QString m_errorText;
    QPointer<ListDirectoryTask> m_pending;
    /// The git read in flight, so an answer about a folder somebody has already
    /// left cannot land in the band.
    QPointer<ReadRepositoryTask> m_repositoryPending;
    /// The work tree walk in flight, for the same reason.
    QPointer<ReadStatusTask> m_statusPending;
    /// Which work tree that walk is of, so navigating inside one checkout can tell
    /// itself apart from navigating out of it. The task cannot answer this until it
    /// has run, and the decision has to be made before then.
    QString m_statusWalkRoot;
    /// Collects a burst of writes into one walk. See kStatusFloorMs.
    QTimer* m_statusFloor = nullptr;
    /// Notices a commit, a checkout or a pull made outside Mole: what those change
    /// is the repository directory, which no operation of ours announces.
    QFileSystemWatcher* m_gitWatcher = nullptr;
};

} // namespace mole
