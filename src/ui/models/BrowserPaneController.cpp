#include "ui/models/BrowserPaneController.h"

#include "ui/models/FileListModel.h"

#include "core/alerts/AlertStore.h"
#include "core/analysis/AnalysisStore.h"
#include "core/events/EventBus.h"
#include "core/tasks/ListDirectoryTask.h"
#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/VfsManager.h"

namespace mole {

BrowserPaneController::BrowserPaneController(PluginServices services, QObject* parent)
    : QObject(parent)
    , m_services(services)
    , m_files(new FileListModel(this))
{
}

BrowserPaneController::~BrowserPaneController()
{
    if (m_pending)
        m_pending->requestCancel();
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

    for (const FileEntry& entry : entries) {
        const QString uri = entry.uri.toString();
        int flags = alerted.value(uri, FileListModel::NoAnnotation);

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

            const QString wanted = rememberedCursor(directory);
            const int row = wanted.isEmpty() ? -1 : m_files->rowOfUri(wanted);
            m_currentIndex = row >= 0 ? row : (m_files->rowCount() > 0 ? 0 : -1);
            emit currentIndexChanged();
        });

    connect(task, &Task::finished, this, [this, task] {
        if (m_pending != task)
            return;
        m_pending.clear();
        setLoading(false);
        if (task->state() == Task::State::Failed)
            setErrorText(task->error().message);
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
