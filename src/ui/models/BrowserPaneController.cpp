#include "ui/models/BrowserPaneController.h"

#include "ui/models/FileListModel.h"

#include "core/alerts/AlertStore.h"
#include "core/analysis/AnalysisStore.h"
#include "core/events/EventBus.h"
#include "core/rename/RenameTask.h"
#include "core/tasks/InvokeFileActionTask.h"
#include "core/tasks/ListDirectoryTask.h"
#include "core/tasks/MakeDirectoryTask.h"
#include "core/tasks/ProbeDriveTask.h"
#include "core/tasks/QueryFileActionsTask.h"
#include "core/tasks/QueryFolderActionsTask.h"
#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/text/SizeWords.h"
#include "core/vcs/ReadRepositoryTask.h"
#include "core/vcs/ReadStatusTask.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QUrl>

namespace mole {

BrowserPaneController::BrowserPaneController(PluginServices services, QObject* parent)
    : QObject(parent)
    , m_services(services)
    , m_files(new FileListModel(this))
    , m_repository(new RepositoryInfo(this))
    , m_statusFloor(new QTimer(this))
    , m_gitWatcher(new QFileSystemWatcher(this))
{
    // **The cursor follows the row, not the row number.** FileListModel already
    // tracks the *selection* by uri so a re-sort or a refresh does not move it
    // onto other files; the cursor was a plain int, re-anchored only by
    // setCurrentIndex() and load(), and nothing listened to the model's reset.
    // So a filter typed with the cursor on row 7 of forty left the cursor on
    // "row 7" of two rows: currentName() empty, F3 and "copy path" greyed out,
    // and Enter in the filter field -- which FilePane.qml calls "open that one",
    // the headline browsing gesture -- calling openRow(7) with nothing there.
    // See MOLE-394.
    connect(m_files, &QAbstractItemModel::modelAboutToBeReset, this,
        [this] { m_cursorWas = m_files->uriAt(m_currentIndex); });
    connect(m_files, &QAbstractItemModel::modelReset, this, [this] { reanchorCursor(); });

    m_statusFloor->setSingleShot(true);
    m_statusFloor->setInterval(kStatusFloorMs);
    connect(m_statusFloor, &QTimer::timeout, this, [this] {
        // The root is read now rather than captured when the timer started: by the
        // time a burst has settled the user may be in a different checkout, and this
        // must walk the one they are looking at.
        if (m_repository->isPresent())
            readStatus(m_repository->root());
    });

    // A commit, a checkout or a pull run from the terminal panel or from another
    // window changes the repository directory and announces nothing. Watched
    // directly rather than through VfsCapability::Watch, which is a capability bit
    // with no backend behind it and no API on IFileSystem to call -- and this
    // feature is local drives only, which is exactly where a filesystem watcher
    // works. See TODO.md.
    connect(m_gitWatcher, &QFileSystemWatcher::directoryChanged, this,
        [this](const QString&) { invalidateStatus(); });
    connect(
        m_gitWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&) { invalidateStatus(); });

    // Straight from the mount table rather than through the EventBus: mounts are
    // added and removed on this thread, and a pane whose drive has just arrived
    // should be listing it in the same turn the sidebar starts showing it.
    if (m_services.vfs) {
        connect(
            m_services.vfs, &VfsManager::mountsChanged, this, &BrowserPaneController::retryIfADriveArrived);
    }

    // Every write Mole performs already says so here, including its own copies and
    // deletes, so there is one place to listen rather than a hook per operation.
    if (m_services.events) {
        connect(
            m_services.events, &EventBus::directoryChanged, this, &BrowserPaneController::noteWrittenInto);
        connect(m_services.events, &EventBus::entryCreated, this, &BrowserPaneController::noteWrittenInto);
        connect(m_services.events, &EventBus::entryRemoved, this, &BrowserPaneController::noteWrittenInto);
        connect(
            m_services.events, &EventBus::entryRenamed, this, [this](const VfsUri& from, const VfsUri& to) {
                noteWrittenInto(from);
                noteWrittenInto(to);
            });
    }
}

void BrowserPaneController::retryIfADriveArrived()
{
    if (!m_waitingForADrive.isValid() || m_waitingForADrive != m_current)
        return;
    // Some other drive's mount changed. Asked rather than attempted, so a pane
    // waiting on a drive nobody is connecting does not re-list on every mount.
    if (!m_services.vfs || !m_services.vfs->resolve(m_waitingForADrive))
        return;
    load(m_current, false);
}

void BrowserPaneController::noteWrittenInto(const VfsUri& path)
{
    if (!m_repository->isPresent())
        return;

    // Anywhere inside the work tree, not only the folder on screen: copying into
    // `src/` while looking at the checkout root changes what the band says, and the
    // roll-up on the folder row with it.
    const QString written = path.toLocalPath();
    const QString& root = m_repository->root();
    if (written.isEmpty() || !(written == root || written.startsWith(root + QLatin1Char('/'))))
        return;

    invalidateStatus();
}

void BrowserPaneController::invalidateStatus()
{
    if (!m_repository->isPresent())
        return;

    // Forgotten at once rather than when the walk runs, so a second pane navigating
    // into this checkout in the meantime cannot pick the stale answer out of the
    // cache and believe it.
    RepositoryStatusCache::shared().forget(m_repository->root());

    // Started, not restarted: a burst collapses into one walk, and a copy that runs
    // for minutes still refreshes as it goes. See kStatusFloorMs.
    if (!m_statusFloor->isActive())
        m_statusFloor->start();
}

void BrowserPaneController::watchRepositoryDirectory(const QString& gitDir)
{
    const QStringList watched = m_gitWatcher->directories() + m_gitWatcher->files();
    if (!watched.isEmpty())
        m_gitWatcher->removePaths(watched);
    if (gitDir.isEmpty())
        return;

    // The repository directory itself, where a commit replaces `index` and a
    // checkout rewrites `HEAD`; and the loose branch tips, where a commit or a pull
    // moves the one HEAD is on without necessarily touching anything above.
    for (const QString& path : { gitDir, gitDir + QStringLiteral("/refs/heads") }) {
        if (QFileInfo::exists(path))
            m_gitWatcher->addPath(path);
    }
}

BrowserPaneController::~BrowserPaneController()
{
    if (m_pending)
        m_pending->requestCancel();
    if (m_repositoryPending)
        m_repositoryPending->requestCancel();
    if (m_statusPending)
        m_statusPending->requestCancel();
}

QString BrowserPaneController::displayPath() const
{
    if (!m_current.isValid())
        return {};
    if (m_current.scheme() == QLatin1String("file"))
        return m_current.path();
    return m_current.toString();
}

QString BrowserPaneController::locationName() const
{
    if (!m_current.isValid())
        return {};
    const QString name = m_current.fileName();
    return name.isEmpty() ? m_current.scheme() : name;
}

void BrowserPaneController::readRepository()
{
    // Local drives only, and this is the whole of that rule: a uri that is not a
    // real filesystem path has no local path, so an archive, an SFTP volume and a
    // bucket all leave the pane with nothing to say.
    const QString localPath = m_current.toLocalPath();
    if (!Repository::isSupported() || localPath.isEmpty() || !m_services.tasks) {
        m_repository->clear();
        return;
    }

    if (m_repositoryPending)
        m_repositoryPending->requestCancel();

    auto* task = new ReadRepositoryTask(localPath);
    m_repositoryPending = task;

    connect(task, &ReadRepositoryTask::repositoryRead, this,
        [this, task](const QString& path, const QString& root, const RepositoryHead& head) {
            // An answer about a folder the user has already left must not land in
            // the band. Two guards rather than one, because a read of the folder
            // now in view can also be superseded by a newer read of the same one.
            if (m_repositoryPending != task || path != m_current.toLocalPath())
                return;
            m_repository->setHead(root, head);
            // Aimed before the walk is asked for, so a commit that lands while the
            // first walk is still running is not the one change that gets missed.
            watchRepositoryDirectory(task->gitDir());
            // The branch is on the band by now; what has changed in the tree costs a
            // walk, so it is asked for second and arrives second.
            readStatus(root);
        });

    connect(task, &Task::finished, this, [this, task] {
        if (m_repositoryPending == task)
            m_repositoryPending.clear();
    });

    m_services.tasks->submit(task);
}

void BrowserPaneController::readStatus(const QString& root)
{
    // A walk of a work tree nobody is looking at any more is work for nobody.
    // Abandoned rather than left to finish, because on a large checkout that is the
    // difference between a window that answers and one that is busy for seconds
    // after the user has moved on.
    //
    // Only when the work tree changed, though. Moving from `src/` to `tests/` is
    // still waiting on the same walk, and cancelling it there would mean starting
    // it again from the top.
    if (m_statusPending && m_statusWalkRoot != root) {
        m_statusPending->requestCancel();
        m_statusPending.clear();
        m_statusWalkRoot.clear();
    }

    if (root.isEmpty() || !m_services.tasks) {
        m_repository->clearStatus();
        annotateListing(m_files->allEntries());
        // Nothing to watch outside a checkout, and an inotify handle held on the last
        // one is a handle held on a directory the user may be deleting.
        watchRepositoryDirectory(QString());
        m_statusFloor->stop();
        return;
    }

    // Somebody has walked this work tree already -- the other pane, or this one
    // before the user moved between two folders in the same checkout. One walk per
    // checkout is the rule, and this is where it is kept: no task is submitted at
    // all, so there is nothing to cancel and nothing on the strip.
    const RepositoryStatus known = RepositoryStatusCache::shared().forRoot(root);
    if (known.complete) {
        m_repository->setStatus(root, known);
        annotateListing(m_files->allEntries());
        return;
    }

    // Already walking this very work tree, for the folder the user was in a moment
    // ago. Its answer covers this folder too -- that is what a work tree walk is --
    // so there is nothing to start.
    if (m_statusPending)
        return;

    auto* task = new ReadStatusTask(m_current.toLocalPath());
    m_statusPending = task;
    m_statusWalkRoot = root;

    connect(task, &ReadStatusTask::statusRead, this,
        [this, task](const QString& walked, const RepositoryStatus& status) {
            if (m_statusPending != task)
                return;
            // setStatus() drops an answer about a work tree that is no longer the
            // one in view, so a walk that outlived its pane's navigation is spent
            // rather than wrong.
            m_repository->setStatus(walked, status);
            // The listing landed before the walk did, so the rows are marked here
            // rather than when they arrived. setAnnotations() is one dataChanged
            // over the rows: nothing is inserted, removed or reordered, so the
            // cursor and the ticks stay where the user left them.
            annotateListing(m_files->allEntries());
        });

    connect(task, &Task::finished, this, [this, task] {
        if (m_statusPending == task) {
            m_statusPending.clear();
            m_statusWalkRoot.clear();
        }
    });

    m_services.tasks->submit(task);
}

void BrowserPaneController::annotateListing(const FileEntryList& entries)
{
    QHash<QString, int> annotations;

    // One directory read for the reports and one in-memory pass for the
    // alerts. A store lookup per row would make a listing of five thousand
    // entries pay thousands of file opens for a pair of small tags.
    const QSet<QString> reported
        = m_services.reports ? m_services.reports->storedFolderNames() : QSet<QString> {};

    QHash<QString, int> alerted;
    if (m_services.alerts) {
        const QList<AlertRule> rules = m_services.alerts->rules();
        for (const AlertRule& rule : rules) {
            int flags = alerted.value(rule.targetUri, 0) | FileListModel::AlertPresent;
            if (rule.state == AlertState::Triggered)
                flags |= FileListModel::AlertTriggered;
            alerted.insert(rule.targetUri, flags);
        }
    }

    // Git status, when a walk has answered for the work tree in view. Looked up by
    // local path rather than by uri: the walk answers in the paths a kernel
    // understands, and a row's uri can be spelled more than one way.
    const RepositoryStatus& status = m_repository->status();
    const bool marking = m_repository->isPresent() && m_repository->isStatusKnown();

    for (const FileEntry& entry : entries) {
        const QString uri = entry.uri.toString();
        int flags = alerted.value(uri, FileListModel::NoAnnotation);

        if (marking) {
            const QString localPath = entry.uri.toLocalPath();
            if (!localPath.isEmpty()) {
                if (const int state = status.stateFor(localPath); state != RepositoryUnchanged)
                    flags |= state << FileListModel::GitStateShift;
            }
        }

        // What the drive said about this folder, looked up by name in a set the
        // one query filled. No work per row beyond the lookup itself.
        if (!m_offeredHere.isEmpty() && m_offeredHere.contains(entry.name))
            flags |= FileListModel::DriveActionPresent;

        // Only folders can have a report; testing files would hash five
        // thousand names to answer no five thousand times.
        if (entry.isDir && !reported.isEmpty() && reported.contains(AnalysisStore::folderNameFor(uri))) {
            flags |= FileListModel::ReportPresent;
        }

        if (flags != FileListModel::NoAnnotation)
            annotations.insert(uri, flags);
    }

    m_files->setAnnotations(std::move(annotations));
}

QVariantList BrowserPaneController::pathSegments() const
{
    QVariantList out;
    if (!m_current.isValid())
        return out;

    // The first crumb is the drive, not an empty string: "/" and "sftp://host"
    // are both places you can click on, and neither has a name of its own.
    const bool local = m_current.scheme() == QLatin1String("file");
    VfsUri walker = m_current;
    QList<QPair<QString, QString>> reversed;

    while (walker.isValid() && !walker.isRoot()) {
        reversed.append({ walker.fileName(), walker.toString() });
        walker = walker.parent();
    }
    reversed.append({ local ? QStringLiteral("/") : m_current.scheme(), walker.toString() });

    for (int i = reversed.size() - 1; i >= 0; --i) {
        out.append(QVariantMap { { QStringLiteral("label"), reversed.at(i).first },
            { QStringLiteral("uri"), reversed.at(i).second },
            // The last crumb is where we already are; nothing to click.
            { QStringLiteral("current"), i == 0 } });
    }
    return out;
}

void BrowserPaneController::navigateTo(const QString& uri)
{
    const VfsUri target = VfsUri::fromString(uri);
    if (!target.isValid()) {
        setErrorText(QStringLiteral("Not a valid location: %1").arg(uri));
        return;
    }
    load(target, true);
}

void BrowserPaneController::reanchorCursor()
{
    if (m_installingListing)
        return;

    // Where the cursor was, if that row is still visible; the first row when it
    // is not, because a list with rows in it and no cursor is a list nothing can
    // be done to. No cursor at all only when there are no rows.
    const int rows = m_files->rowCount();
    if (rows == 0) {
        if (m_currentIndex == -1)
            return;
        m_currentIndex = -1;
        emit currentIndexChanged();
        refreshDriveActions();
        return;
    }

    const int row = m_cursorWas.isEmpty() ? -1 : m_files->rowOfUri(m_cursorWas);
    const int landed = row >= 0 ? row : qBound(0, m_currentIndex, rows - 1);
    if (m_currentIndex == landed)
        return;
    m_currentIndex = landed;
    emit currentIndexChanged();
    refreshDriveActions();
}

void BrowserPaneController::setCurrentIndex(int index)
{
    const int rows = m_files->rowCount();
    const int clamped = rows == 0 ? -1 : qBound(0, index, rows - 1);
    if (m_currentIndex == clamped)
        return;
    m_currentIndex = clamped;
    emit currentIndexChanged();
    refreshDriveActions();
}

QString BrowserPaneController::currentName() const
{
    return m_files->nameAt(m_currentIndex);
}

bool BrowserPaneController::isWritable() const
{
    if (!m_current.isValid() || !m_services.vfs)
        return false;
    FileSystemPtr fs = m_services.vfs->resolve(m_current);
    return fs && fs->capabilities().testFlag(VfsCapability::Write);
}

void BrowserPaneController::revealFile(const QString& fileUri)
{
    const VfsUri file = VfsUri::fromString(fileUri);
    if (!file.isValid())
        return;

    m_pendingReveal = fileUri;
    m_pendingRevealMissing = false;
    const VfsUri folder = file.parent();
    if (folder == m_current) {
        // Already here -- but "here" is not the same as "listed". A pane opened a
        // moment ago in order to show this file has a listing in flight, and that
        // listing honours the request when it lands; answering from the rows there
        // are now would answer from none. See MOLE-205.
        if (m_pending)
            return;
        const int row = m_files->rowOfUri(fileUri);
        m_pendingReveal.clear();
        if (row >= 0) {
            m_currentIndex = row;
            emit currentIndexChanged();
        }
        return;
    }
    navigateTo(folder.toString());
}

void BrowserPaneController::revealMissingFile(const QString& fileUri)
{
    const VfsUri file = VfsUri::fromString(fileUri);
    if (!file.isValid())
        return;

    const VfsUri folder = file.parent();
    if (folder == m_current) {
        // Already the folder in view, so no listing is coming. The cursor comes off
        // whatever it was on, because the answer to "where is this file" is this
        // folder and not any row in it.
        if (m_currentIndex != -1) {
            m_currentIndex = -1;
            emit currentIndexChanged();
        }
        return;
    }

    // Asked for by name so that the listing which lands does not fall back to the
    // first row, and remembered so a second navigation to this folder later is
    // unaffected.
    m_pendingReveal = fileUri;
    m_pendingRevealMissing = true;
    navigateTo(folder.toString());
}

void BrowserPaneController::moveCursor(int delta)
{
    const int rows = m_files->rowCount();
    if (rows == 0)
        return;
    setCurrentIndex(m_currentIndex < 0 ? 0 : m_currentIndex + delta);
}

void BrowserPaneController::cursorToStart()
{
    setCurrentIndex(0);
}

void BrowserPaneController::cursorToEnd()
{
    setCurrentIndex(m_files->rowCount() - 1);
}

void BrowserPaneController::toggleSelectionAndAdvance()
{
    if (m_currentIndex < 0)
        return;
    m_files->toggleSelected(m_currentIndex);
    moveCursor(1);
}

QList<VfsUri> BrowserPaneController::targets() const
{
    return m_files->targets(m_currentIndex);
}

QStringList BrowserPaneController::dragTargets(int row) const
{
    QStringList uris;
    if (row < 0)
        return uris;

    // Dragging one of the ticked rows takes all of them; dragging any other row
    // takes that row alone. Passing no fallback is what says "the ticked rows and
    // nothing else" -- targets() would offer the cursor row instead, which is the
    // right answer for a key and the wrong one for a pointer.
    if (m_files->isSelected(row)) {
        const QList<VfsUri> ticked = m_files->targets(-1);
        uris.reserve(static_cast<int>(ticked.size()));
        for (const VfsUri& uri : ticked)
            uris.append(uri.toString());
        return uris;
    }

    const QString uri = m_files->uriAt(row);
    if (!uri.isEmpty())
        uris.append(uri);
    return uris;
}

int BrowserPaneController::targetCount() const
{
    return static_cast<int>(targets().size());
}

QString BrowserPaneController::targetSummary() const
{
    const QList<VfsUri> selected = targets();
    if (selected.isEmpty())
        return {};
    if (selected.size() == 1)
        return selected.first().fileName();
    return QStringLiteral("%1 items").arg(selected.size());
}

QVariantList BrowserPaneController::targetDetails() const
{
    QVariantList out;
    for (const FileEntry& entry : m_files->targetEntries(m_currentIndex)) {
        out.append(QVariantMap { { QStringLiteral("name"), entry.name },
            // The uri as well as the name, because the dialog that shows these
            // rows is the thing that must then delete exactly *these* rows --
            // not whatever the cursor and the selection say a second later. See
            // deleteTargets() and MOLE-339.
            { QStringLiteral("uri"), entry.uri.toString() }, { QStringLiteral("isDir"), entry.isDir },
            // A folder's own size says nothing about what is inside it, and a
            // dialog that showed "4 kB" next to a tree of ten thousand files
            // would be worse than showing nothing.
            { QStringLiteral("detail"), entry.isDir ? QString() : sizeInWords(entry.size) } });
    }
    return out;
}

void BrowserPaneController::createDirectory(const QString& name)
{
    if (name.trimmed().isEmpty() || !m_current.isValid())
        return;

    FileSystemPtr fs = m_services.vfs->resolve(m_current);
    if (!fs) {
        emit operationFailed(QStringLiteral("No drive is mounted here"));
        return;
    }

    // On a task, because makeDirectory() goes to storage. On a share that has
    // stopped answering it blocks for as long as the mount takes to give up, and
    // this used to make the call from the thread that draws -- so pressing F7
    // stopped the whole window, dialog and all. deleteTargets() two functions
    // down has always done it this way. See ARCHITECTURE.md's first rule and
    // MOLE-360.
    const VfsUri target = m_current.child(name.trimmed());
    auto* task = new MakeDirectoryTask(std::move(fs), target);
    connect(task, &MakeDirectoryTask::created, this, [this](const VfsUri& made) {
        // Announced rather than refreshed directly: a second pane on the same
        // folder has to see it too.
        m_services.events->postEntryCreated(made);
    });
    connect(task, &MakeDirectoryTask::refused, this,
        [this](const QString& reason) { emit operationFailed(reason); });
    m_services.tasks->submit(task);
}

void BrowserPaneController::renameCurrent(const QString& newName)
{
    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty() || m_currentIndex < 0)
        return;

    const VfsUri source = VfsUri::fromString(m_files->uriAt(m_currentIndex));
    if (!source.isValid() || trimmed == source.fileName())
        return;

    FileSystemPtr fs = m_services.vfs->resolve(source);
    if (!fs) {
        emit operationFailed(QStringLiteral("No drive is mounted here"));
        return;
    }

    // Through the batch renamer's own task, which handles one entry as readily as
    // two hundred -- and off this thread, for the reason createDirectory() gives.
    // F2 on a stalled mount used to stop the window with the rename box still
    // open on it. See MOLE-360.
    const VfsUri target = source.parent().child(trimmed);
    RenamePlan::Entry entry;
    entry.source = source;
    entry.originalName = source.fileName();
    entry.newName = trimmed;

    auto* task = new RenameTask(m_services.vfs, { entry });
    connect(task, &Task::finished, this, [this, task, source, target] {
        if (!task->failures().isEmpty()) {
            emit operationFailed(task->failures().join(QLatin1String("; ")));
            return;
        }
        m_services.events->postEntryRenamed(source, target);
    });
    m_services.tasks->submit(task);
}

void BrowserPaneController::deleteTargets(const QStringList& uris)
{
    // The rows the question was asked about, when the caller kept them -- and
    // the cursor's own targets only when it did not.
    //
    // The confirmation froze what it *showed* and then called this with no
    // argument, so the targets were worked out again at accept time. A modal
    // does not stop the event loop: a directoryChanged from the other pane, a
    // finishing task or a second tab on the same folder reloads the model under
    // the dialog, and if the cursor lands somewhere else the rows deleted are
    // not the rows named. The dialog's own comment said the two were "the same
    // rows by construction"; only the display was. See MOLE-339.
    QList<VfsUri> doomed;
    if (uris.isEmpty()) {
        doomed = targets();
    } else {
        for (const QString& uri : uris) {
            const VfsUri parsed = VfsUri::fromString(uri);
            if (parsed.isValid())
                doomed.append(parsed);
        }
    }
    if (doomed.isEmpty())
        return;

    FileSystemPtr fs = m_services.vfs->resolve(m_current);
    if (!fs) {
        emit operationFailed(QStringLiteral("No drive is mounted here"));
        return;
    }

    const VfsUri parent = m_current;
    auto* task = new DeleteTask(std::move(fs), doomed);
    connect(task, &Task::finished, this, [this, task, parent] {
        if (!task->failures().isEmpty())
            emit operationFailed(task->failures().join(QLatin1String("; ")));
        m_services.events->postDirectoryChanged(parent);
    });

    m_files->clearSelection();
    m_services.tasks->submit(task);
}

QList<VfsUri> BrowserPaneController::droppedRows(const QStringList& urls, int* alreadyHereOut) const
{
    QList<VfsUri> rows;
    int alreadyHere = 0;

    for (const QString& text : urls) {
        // Only files. A browser offers http urls and a scrap of HTML beside
        // them; no backend here could fetch either, and fetching from the web is
        // not something this project does.
        const QUrl url(text);
        if (!url.isLocalFile())
            continue;

        const VfsUri uri = VfsUri::fromLocalPath(url.toLocalFile());
        if (!uri.isValid())
            continue;

        // Already in this folder, which is what a drag onto the folder it came
        // from looks like from here. Taking it would mean asking the user about
        // collisions with itself.
        if (uri.parent() == m_current) {
            ++alreadyHere;
            continue;
        }

        rows.append(uri);
    }

    if (alreadyHereOut)
        *alreadyHereOut = alreadyHere;
    return rows;
}

QVariantMap BrowserPaneController::dropPlan(const QStringList& urls) const
{
    QVariantMap plan;
    const QList<VfsUri> rows = droppedRows(urls);

    plan.insert(QStringLiteral("count"), static_cast<int>(rows.size()));
    plan.insert(QStringLiteral("targetPath"), displayPath());
    plan.insert(QStringLiteral("writable"), isWritable());
    // A single item could be given a different name on arrival; a batch could
    // not, because there would be nothing sensible to call the rest.
    plan.insert(QStringLiteral("singleName"), rows.size() == 1 ? rows.first().fileName() : QString());

    QSet<QString> existing;
    for (int row = 0; row < m_files->rowCount(); ++row)
        existing.insert(m_files->nameAt(row));

    QStringList collisions;
    for (const VfsUri& row : rows) {
        if (existing.contains(row.fileName()))
            collisions.append(row.fileName());
    }
    plan.insert(QStringLiteral("collisions"), collisions);

    // How much, but only while asking is cheap.
    //
    // A dropped url is a local path by construction, and a stat on a local path
    // is nothing -- except that "local" includes a kernel-mounted NFS or SMB
    // path, where it is a round trip, and this runs on the thread that draws
    // when a drag enters the pane. Two hundred files dragged off a share was two
    // hundred round trips before the banner could say anything. Past a few dozen
    // the total is dropped and the banner says the count alone, which is the
    // part that matters anyway. A folder claims nothing either way: its own size
    // says nothing about what is inside it, and walking it to find out is
    // exactly what must not happen mid-gesture. See MOLE-360.
    constexpr int kSizesWorthAsking = 32;
    if (rows.size() <= kSizesWorthAsking) {
        qint64 bytes = 0;
        for (const VfsUri& row : rows) {
            const QFileInfo info(row.toLocalPath());
            if (info.isFile())
                bytes += info.size();
        }
        plan.insert(QStringLiteral("sizeText"), sizeInWords(bytes));
    } else {
        plan.insert(QStringLiteral("sizeText"), QString());
    }
    return plan;
}

void BrowserPaneController::dropHere(const QStringList& urls, const QString& conflict)
{
    if (!m_current.isValid())
        return;

    // Refused while the answer is still cheap. A destination that cannot be
    // written to has to say so rather than accept the drop and fail afterwards.
    if (!isWritable()) {
        emit operationFailed(QStringLiteral("%1 is read-only").arg(displayPath()));
        return;
    }

    int alreadyHere = 0;
    const QList<VfsUri> rows = droppedRows(urls, &alreadyHere);
    if (rows.isEmpty()) {
        // Nothing to do and nothing to say: the files are already where they
        // were dropped.
        if (alreadyHere > 0)
            return;
        // A drop that silently does nothing reads as a bug and gets reported as
        // one, so the commonest way to get here -- dragging a picture out of a
        // web page, which is a link rather than a file -- is named.
        emit operationFailed(QStringLiteral("Nothing here is a file. A link or a picture dragged out "
                                            "of a web page is an address, not a file to copy."));
        return;
    }

    FileSystemPtr target = m_services.vfs->resolve(m_current);
    if (!target) {
        emit operationFailed(QStringLiteral("No drive is mounted here"));
        return;
    }

    // The source is usually under no mount at all -- an ordinary download folder
    // is neither Home nor a system volume, and resolve() answers from the mount
    // table. The local backend is stateless, so constructing one is the right
    // answer rather than refusing a perfectly ordinary file.
    FileSystemPtr source = m_services.vfs->resolve(rows.first());
    if (!source)
        source = std::make_shared<LocalFileSystem>();

    TransferTask::Request request;
    request.sourceFileSystem = std::move(source);
    request.targetFileSystem = std::move(target);
    request.sources = rows;
    request.targetDirectory = m_current;
    // Never Move, whatever the source offered. Deleting somebody else's file
    // because of a gesture that looks exactly like the one that copies is the
    // one outcome a drop may not have -- see ADR-0040.
    request.mode = TransferTask::Mode::Copy;
    request.onConflict = conflict == QLatin1String("overwrite") ? TransferTask::Conflict::Overwrite
        : conflict == QLatin1String("skip")                     ? TransferTask::Conflict::Skip
                                                                : TransferTask::Conflict::Fail;

    const VfsUri destination = m_current;
    auto* task = new TransferTask(request);
    connect(task, &Task::finished, this, [this, task, destination] {
        if (!task->failures().isEmpty())
            emit operationFailed(task->failures().join(QLatin1String("; ")));
        // Announced rather than refreshed directly: every pane showing this
        // folder has to see what arrived, not only the one that was dropped on.
        m_services.events->postDirectoryChanged(destination);
    });

    m_services.tasks->submit(task);
}

bool BrowserPaneController::activate(int row)
{
    if (!m_files->isDirAt(row)) {
        emit fileActivated(m_files->uriAt(row));
        return false;
    }
    navigateTo(m_files->uriAt(row));
    return true;
}

void BrowserPaneController::goUp()
{
    if (canGoUp())
        load(m_current.parent(), true);
}

void BrowserPaneController::goBack()
{
    if (!canGoBack())
        return;
    --m_historyIndex;
    load(VfsUri::fromString(m_history.at(m_historyIndex)), false);
    emit historyChanged();
}

void BrowserPaneController::goForward()
{
    if (!canGoForward())
        return;
    ++m_historyIndex;
    load(VfsUri::fromString(m_history.at(m_historyIndex)), false);
    emit historyChanged();
}

void BrowserPaneController::refresh()
{
    if (m_current.isValid())
        load(m_current, false);
}

void BrowserPaneController::rememberCursor(const VfsUri& from, const VfsUri& to)
{
    const auto note = [this](const QString& folder, const QString& entry) {
        if (folder.isEmpty() || entry.isEmpty())
            return;
        if (!m_cursorMemory.contains(folder))
            m_cursorMemoryOrder.append(folder);
        m_cursorMemory.insert(folder, entry);
        while (m_cursorMemoryOrder.size() > kCursorMemoryLimit)
            m_cursorMemory.remove(m_cursorMemoryOrder.takeFirst());
    };

    if (!from.isValid())
        return;

    // Where the cursor stood in the folder being left.
    if (m_currentIndex >= 0 && m_currentIndex < m_files->rowCount())
        note(from.toString(), m_files->uriAt(m_currentIndex));

    // And, when stepping up, the folder itself is what the cursor should be on
    // in the parent -- which is what makes walking a tree with the keyboard
    // feel like walking rather than restarting at every level.
    if (to.isValid() && from.parent().toString() == to.toString())
        note(to.toString(), from.toString());
}

QString BrowserPaneController::rememberedCursor(const VfsUri& folder) const
{
    return folder.isValid() ? m_cursorMemory.value(folder.toString()) : QString();
}

void BrowserPaneController::load(const VfsUri& uri, bool recordHistory)
{
    if (!m_services.isValid()) {
        setErrorText(QStringLiteral("Application services are not available"));
        return;
    }

    rememberCursor(m_current, uri);

    // Abandon whatever the previous location was still fetching. Without this
    // a slow mount would keep filling the pane after the user moved on.
    if (m_pending)
        m_pending->requestCancel();
    // **And what was asked about that folder, here rather than when the next
    // listing lands.** Calling the marks query off was left to
    // refreshFolderMarks(), which runs when the new listing arrives -- so on a
    // pool small enough that a drive gets one thread at a time, the new listing
    // queued behind the query it was supposed to cancel and neither ever ran.
    // A four-core machine has a pool of two and a limit of one per drive, which
    // is every CI runner. Leaving a folder is the moment the answer stopped
    // being wanted, and it does not depend on anything else starting. See
    // MOLE-425 and ADR-0076.
    if (m_folderActionsPending)
        m_folderActionsPending->requestCancel();
    if (m_driveActionsPending)
        m_driveActionsPending->requestCancel();

    // Cleared before the mount is rebuilt below: rebuilding one announces itself,
    // and a pane that is still marked as waiting would answer that announcement by
    // starting this load a second time from inside itself.
    m_waitingForADrive = VfsUri();

    // Where the pane means to be, recorded before a drive is asked anything.
    //
    // **A location no drive answers for is still the pane's location.** It is what
    // the path bar shows, what Refresh retries, and what the session file keeps --
    // and a pane that forgot it instead had no way back: a tab restored before its
    // drive was connected went to the start folder, saved that over the remembered
    // one on the next debounce, and connecting the drive afterwards brought nothing
    // back. See MOLE-351.
    m_current = uri;
    // What the drive said about the folder being left says nothing about the one
    // arriving, and a re-listing of this one has to ask again rather than draw
    // what was true before it.
    m_offeredHere.clear();
    m_offeredHereFor = VfsUri();
    if (recordHistory) {
        // Navigating after going back truncates the forward history, the way
        // a browser does.
        while (m_history.size() > m_historyIndex + 1)
            m_history.removeLast();
        m_history.append(uri.toString());
        m_historyIndex = m_history.size() - 1;
        emit historyChanged();
    }
    emit locationChanged();
    // Before the listing rather than after it. The branch costs a discovery and a
    // handful of reference reads, and a folder with fifty thousand files in it
    // should not hold the one fact that is already known.
    readRepository();

    // Every way into a place comes through here -- a back step, a bookmark, a
    // breadcrumb, a restored tab, Refresh -- so this is where an archive whose
    // mount went away while nobody was inside it comes back. A no-op for anything
    // that still has a mount, and for every backend whose root cannot rebuild
    // itself. It sits here rather than in navigateTo() because goBack(),
    // goForward(), goUp() and refresh() call load() directly, and a back step is
    // one of the three things the rebuild exists for. See Mount::unlisted,
    // ADR-0083 and MOLE-310.
    if (m_services.vfs)
        m_services.vfs->remountFor(uri);

    FileSystemPtr fs = m_services.vfs->resolve(uri);
    if (!fs) {
        setErrorText(QStringLiteral("No drive is mounted for %1").arg(uri.toString()));
        m_files->clear();
        // Nobody else knows this pane wanted a drive. Said out loud so whoever can
        // arrange one does -- connecting a configured drive is AppController's job
        // and asking for a passphrase is the user's -- and remembered so the pane
        // retries by itself when a mount appears, whoever caused it.
        m_waitingForADrive = uri;
        if (m_services.events)
            m_services.events->postDriveNeeded(uri);
        return;
    }

    setErrorText({});
    setLoading(true);

    // Kept, because the listing takes ownership of its own reference and the
    // drive is wanted again below.
    FileSystemPtr drive = fs;
    auto* task = new ListDirectoryTask(std::move(fs), uri);
    m_pending = task;

    connect(task, &ListDirectoryTask::listed, this,
        [this, task](const VfsUri& directory, const FileEntryList& entries) {
            // A stale task that was cancelled mid-flight must not overwrite the
            // listing the user is actually looking at.
            if (m_pending != task || directory != m_current)
                return;
            // A filter belongs to the folder it was typed in; carrying it
            // into the next one looks like an empty directory.
            // A whole new listing places the cursor itself, a few lines below,
            // from what was remembered for this folder -- so the re-anchoring
            // that follows a filter or a re-sort must stay out of the way rather
            // than putting an interim cursor somewhere and asking the drive
            // about it.
            m_installingListing = true;
            m_files->setFilterText(QString());
            m_files->setEntries(entries);
            m_installingListing = false;
            m_cursorWas.clear();

            // Back on whatever the cursor was last on here, if it still
            // exists; the first row otherwise.
            annotateListing(entries);

            // A file someone asked to be shown wins over where the cursor was
            // last time in this folder: they said which one they meant.
            const QString wanted = m_pendingReveal.isEmpty() ? rememberedCursor(directory) : m_pendingReveal;
            // A reveal of something not expected to be here answers with the folder
            // and no row, rather than with the first row -- see revealMissingFile().
            const bool missing = m_pendingRevealMissing;
            m_pendingReveal.clear();
            m_pendingRevealMissing = false;
            const int row = wanted.isEmpty() ? -1 : m_files->rowOfUri(wanted);
            m_currentIndex = row >= 0 ? row : (missing || m_files->rowCount() == 0 ? -1 : 0);
            emit currentIndexChanged();
            // The cursor was placed without going through setCurrentIndex(), so
            // what the drive can do to the row it landed on is asked for here.
            refreshDriveActions();
            refreshFolderMarks();
        });

    connect(task, &Task::finished, this, [this, task] {
        if (m_pending != task)
            return;
        m_pending.clear();
        setLoading(false);
        if (task->state() == Task::State::Failed) {
            setErrorText(task->error().message);
            // Announced as well as shown. A listing that failed is the plainest
            // evidence there is that a drive is not answering, and the pane is
            // the only thing that sees it -- the sidebar has no idea a listing
            // was even attempted.
            if (m_services.events)
                m_services.events->postOperationFailed(task->directory(), task->error());
        }
    });

    m_services.tasks->submit(task);

    // What a drive can offer beyond a listing depends on what it was pointed at,
    // so it is discovered rather than compiled in -- and this is the moment to
    // discover it. Somebody is looking at a folder on the drive, which is the
    // first time anybody needs the answer, and the drive is already being called
    // for the listing. A drive nobody opens is never asked at all.
    //
    // Its own task, not a step inside the listing: a probe that never comes back
    // must not be a folder that never opens. Guarded here as well as inside the
    // drive so that navigating around one drive does not queue a task per step;
    // two panes opening at once may still both submit, and the drive answers the
    // first and nothing to the second. See ADR-0076.
    if (drive->offers().state == DriveOffers::State::Unasked) {
        auto* probe = new ProbeDriveTask(drive, uri);
        // What the drive turned out to offer is what decides whether the folder
        // is worth a query at all, and the answer arrives after the listing has
        // already been asked for.
        connect(probe, &Task::finished, this, [this] {
            refreshFolderMarks();
            // **And the row the cursor is already on.** refreshDriveActions()
            // declines while the probe has not answered -- there is nothing to
            // ask about yet -- and its comment says load() asks again when it
            // does. It does not: the listing has already loaded by the time a
            // probe comes back, so nothing re-asked and the menu for the row
            // somebody was sitting on stayed empty until they moved the cursor.
            //
            // A race, and which way it fell depended on the machine: on a
            // four-core runner the probe answers after the cursor is set, and
            // four suites failed there while passing everywhere else. See
            // MOLE-425.
            refreshDriveActions();
        });
        m_services.tasks->submit(probe);
    }
}

QVariantList BrowserPaneController::driveActions() const
{
    QVariantList out;
    for (const FileAction& action : m_driveActions) {
        out.append(QVariantMap { { QStringLiteral("id"), action.id },
            { QStringLiteral("title"), action.title }, { QStringLiteral("enabled"), action.enabled } });
    }
    return out;
}

void BrowserPaneController::refreshDriveActions()
{
    if (m_driveActionsPending)
        m_driveActionsPending->requestCancel();

    const QString uriText = m_files->uriAt(m_currentIndex);
    const VfsUri target = VfsUri::fromString(uriText);
    if (!target.isValid() || !m_services.vfs || !m_services.tasks) {
        if (!m_driveActions.isEmpty()) {
            m_driveActions.clear();
            m_driveActionsFor = VfsUri();
            emit driveActionsChanged();
        }
        return;
    }
    if (target == m_driveActionsFor)
        return;

    FileSystemPtr fs = m_services.vfs->resolve(target);
    if (!fs)
        return;
    // A drive that offers nothing is not asked at all: no task, no query, no
    // work -- the same gate refreshFolderMarks() has always had, and ADR-0076
    // states for both. This one did not, and it runs on *every cursor step*: a
    // stat per row, over the network, for an answer that was always going to be
    // empty. Until the probe has answered there is nothing to ask about either;
    // load() asks again when it does.
    if (fs->offers().state == DriveOffers::State::Unasked || fs->offers().ids.isEmpty())
        return;

    // The row itself, which the listing in front of the user already holds, so
    // the task has nothing to stat.
    auto* task = m_currentIndex >= 0 && m_currentIndex < m_files->rowCount()
        ? new QueryFileActionsTask(std::move(fs), target, m_files->entryAt(m_currentIndex))
        : new QueryFileActionsTask(std::move(fs), target);
    m_driveActionsPending = task;
    connect(task, &Task::finished, this, [this, task] {
        if (m_driveActionsPending != task)
            return;
        m_driveActionsPending.clear();
        // The cursor may have moved on while the drive was thinking. An answer
        // about a row nobody is looking at any more is not an answer.
        if (task->target().toString() != m_files->uriAt(m_currentIndex))
            return;
        if (m_driveActions.isEmpty() && task->actions().isEmpty())
            return;
        m_driveActions = task->actions();
        m_driveActionsFor = task->target();
        emit driveActionsChanged();
    });
    m_services.tasks->submit(task);
}

void BrowserPaneController::refreshFolderMarks()
{
    // One query per folder, and this is where that is enforced: two things ask
    // for the marks -- the listing landing, and the probe answering afterwards
    // with what the drive turned out to offer -- and whichever is second must
    // not ask again.
    if (m_folderActionsPending) {
        if (m_folderActionsPending->directory() == m_current)
            return;
        // A different folder: what was asked about the last one is no longer
        // worth waiting for.
        m_folderActionsPending->requestCancel();
    }
    if (m_offeredHereFor == m_current)
        return;
    if (!m_current.isValid() || !m_services.vfs || !m_services.tasks)
        return;

    FileSystemPtr fs = m_services.vfs->resolve(m_current);
    // A drive that offers nothing is not asked at all: no task, no query, no
    // work. Until the probe has answered there is nothing to ask about either --
    // and when it answers, load() asks again. See ADR-0076.
    if (!fs || fs->offers().ids.isEmpty())
        return;

    auto* task = new QueryFolderActionsTask(std::move(fs), m_current);
    m_folderActionsPending = task;
    connect(task, &Task::finished, this, [this, task] {
        if (m_folderActionsPending != task)
            return;
        m_folderActionsPending.clear();
        // An answer for a folder nobody is looking at any more is discarded
        // rather than drawn.
        if (task->directory() != m_current)
            return;
        m_offeredHere = QSet<QString>(task->names().begin(), task->names().end());
        m_offeredHereFor = task->directory();
        annotateListing(m_files->allEntries());
    });
    m_services.tasks->submit(task);
}

void BrowserPaneController::invokeDriveAction(const QString& id)
{
    const VfsUri target = VfsUri::fromString(m_files->uriAt(m_currentIndex));
    if (!target.isValid() || !m_services.vfs || !m_services.tasks)
        return;

    // Only what the drive actually offered for this row, and only while it is
    // still on offer: a stale menu entry must not reach the drive.
    QString title;
    for (const FileAction& action : m_driveActions) {
        if (action.id == id && action.enabled && m_driveActionsFor == target)
            title = action.title;
    }
    if (title.isEmpty())
        return;

    FileSystemPtr fs = m_services.vfs->resolve(target);
    if (!fs)
        return;

    auto* task = new InvokeFileActionTask(std::move(fs), id, title, target);
    connect(task, &Task::finished, this, [this, task] {
        if (task->state() == Task::State::Cancelled)
            return;
        if (task->state() == Task::State::Failed) {
            // Which action, in the drive's own words. "It did not work" tells
            // somebody who picked one of three entries nothing they can act on.
            emit operationFailed(QStringLiteral("%1: %2").arg(task->actionTitle(), task->error().message));
            return;
        }

        const FileActionOutcome& outcome = task->outcome();
        if (outcome.kind == FileActionOutcome::Kind::Text) {
            const QString until = outcome.validUntil.isValid()
                ? outcome.validUntil.toLocalTime().toString(QStringLiteral("d MMMM yyyy, HH:mm"))
                : QString();
            emit driveActionText(task->actionTitle(), outcome.text, until);
            return;
        }

        QVariantList choices;
        for (const VfsUri& uri : outcome.uris) {
            // The drive's own token is the label: it is the only thing that tells
            // one state of a file from another, and this layer must not invent a
            // prettier one it cannot know is true.
            choices.append(QVariantMap { { QStringLiteral("uri"), uri.toString() },
                { QStringLiteral("label"), uri.hasVersion() ? uri.version() : uri.fileName() } });
        }
        emit driveActionUris(task->actionTitle(), choices);
    });
    m_services.tasks->submit(task);
}

void BrowserPaneController::openUri(const QString& uri)
{
    const VfsUri parsed = VfsUri::fromString(uri);
    if (!parsed.isValid())
        return;
    // The same route a row takes, so an earlier version of a file opens in
    // whatever a file of that kind opens in.
    emit fileActivated(uri);
}

void BrowserPaneController::setLoading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    emit loadingChanged();
}

void BrowserPaneController::setErrorText(const QString& text)
{
    if (m_errorText == text)
        return;
    m_errorText = text;
    emit errorTextChanged();
}

} // namespace mole
