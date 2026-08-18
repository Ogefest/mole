#include "ui/models/BrowserPaneController.h"

#include "ui/models/FileListModel.h"

#include "core/alerts/AlertStore.h"
#include "core/analysis/AnalysisStore.h"
#include "core/events/EventBus.h"
#include "core/tasks/ListDirectoryTask.h"
#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
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

void BrowserPaneController::setCurrentIndex(int index)
{
    const int rows = m_files->rowCount();
    const int clamped = rows == 0 ? -1 : qBound(0, index, rows - 1);
    if (m_currentIndex == clamped)
        return;
    m_currentIndex = clamped;
    emit currentIndexChanged();
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
        // Already here, so there is no listing coming to put the cursor on it.
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
            { QStringLiteral("isDir"), entry.isDir },
            // A folder's own size says nothing about what is inside it, and a
            // dialog that showed "4 kB" next to a tree of ten thousand files
            // would be worse than showing nothing.
            { QStringLiteral("detail"), entry.isDir ? QString() : FileListModel::formatSize(entry.size) } });
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

    const VfsUri target = m_current.child(name.trimmed());
    Result<void> created = fs->makeDirectory(target);
    if (!created.ok()) {
        emit operationFailed(created.error().message);
        return;
    }

    // Announce rather than refresh directly: a second pane on the same folder
    // has to see it too.
    m_services.events->postEntryCreated(target);
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

    const VfsUri target = source.parent().child(trimmed);
    Result<void> renamed = fs->rename(source, target);
    if (!renamed.ok()) {
        emit operationFailed(renamed.error().message);
        return;
    }

    m_services.events->postEntryRenamed(source, target);
}

void BrowserPaneController::deleteTargets()
{
    const QList<VfsUri> doomed = targets();
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
    qint64 bytes = 0;
    for (const VfsUri& row : rows) {
        if (existing.contains(row.fileName()))
            collisions.append(row.fileName());

        // A dropped url is a local path by construction, which is what makes
        // this cheap enough to answer mid-gesture. A folder claims nothing: its
        // own size says nothing about what is inside it, and walking it to find
        // out is exactly what must not happen while the pointer is moving.
        const QFileInfo info(row.toLocalPath());
        if (info.isFile())
            bytes += info.size();
    }

    plan.insert(QStringLiteral("collisions"), collisions);
    plan.insert(QStringLiteral("sizeText"), QLocale().formattedDataSize(bytes));
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

    FileSystemPtr fs = m_services.vfs->resolve(uri);
    if (!fs) {
        setErrorText(QStringLiteral("No drive is mounted for %1").arg(uri.toString()));
        m_files->clear();
        return;
    }

    m_current = uri;
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

    setErrorText({});
    setLoading(true);

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
            m_files->setFilterText(QString());
            m_files->setEntries(entries);

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
