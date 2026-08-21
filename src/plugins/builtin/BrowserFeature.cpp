#include "plugins/builtin/BrowserFeature.h"

#include "ui/models/FileListModel.h"

#include "core/alerts/AlertStore.h"
#include "core/analysis/AnalysisStore.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/IndexSummary.h"
#include "core/tasks/QueryAccessTask.h"
#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/VfsManager.h"

#include <QLocale>
#include <QSet>

namespace mole {

BrowserController::BrowserController(
    PluginServices services, QString startUri, ViewMode initialMode, QObject* parent)
    : FeatureController(QStringLiteral("Browser"), parent)
    , m_services(services)
    , m_left(new BrowserPaneController(services, this))
    , m_right(new BrowserPaneController(services, this))
    , m_viewMode(initialMode)
{
    for (BrowserPaneController* pane : { m_left, m_right }) {
        connect(pane, &BrowserPaneController::locationChanged, this, &BrowserController::refreshLabels);
        connect(pane, &BrowserPaneController::locationChanged, this, &FeatureController::stateChanged);
        connect(pane, &BrowserPaneController::fileActivated, this, &BrowserController::fileActivated);
        connect(pane, &BrowserPaneController::operationFailed, this, &BrowserController::operationFailed);

        // Whether a transfer is possible depends on the mode, on what is
        // selected, and on whether the far side can be written to. All three
        // have to reach the binding, or Copy stays greyed out while being
        // perfectly possible.
        connect(pane, &BrowserPaneController::locationChanged, this,
            &BrowserController::transferAvailabilityChanged);
        connect(pane, &BrowserPaneController::currentIndexChanged, this,
            &BrowserController::transferAvailabilityChanged);
        connect(pane->files(), &FileListModel::selectionChanged, this,
            &BrowserController::transferAvailabilityChanged);

        connect(pane, &BrowserPaneController::locationChanged, this, &BrowserController::refreshFolderFacts);
    }

    // A report filed by a scheduled run, or an alert added from another tab,
    // has to show up here without the user navigating away and back.
    if (m_services.alerts) {
        connect(m_services.alerts, &AlertStore::rulesChanged, this, &BrowserController::refreshFolderFacts);
    }

    // The index tag arrives a moment after the listing, like the access tag
    // below it: the first folder shown is drawn before the snapshot has read
    // anything, so without this the tag would be missing until the next folder
    // change.
    if (m_services.indexSummary) {
        connect(
            m_services.indexSummary, &IndexSummary::changed, this, &BrowserController::refreshFolderFacts);
    }

    // React to the world changing underneath us instead of making the user
    // press refresh: anything that touches a directory we are showing gets
    // picked up here, including our own copies and deletes.
    if (m_services.events) {
        connect(m_services.events, &EventBus::directoryChanged, this, [this](const VfsUri& dir) {
            for (BrowserPaneController* pane : { m_left, m_right }) {
                if (pane->currentUri() == dir.toString())
                    pane->refresh();
            }
        });
    }

    if (!startUri.isEmpty()) {
        m_left->navigateTo(startUri);
        m_right->navigateTo(startUri);
    }
    refreshLabels();
    refreshFolderFacts();
}

QStringList BrowserController::openLocations() const
{
    QStringList open;
    if (m_viewMode == ViewMode::Dual) {
        if (m_left)
            open.append(m_left->currentUri());
        if (m_right)
            open.append(m_right->currentUri());
        return open;
    }
    if (BrowserPaneController* pane = activePane())
        open.append(pane->currentUri());
    return open;
}

BrowserPaneController* BrowserController::activePane() const
{
    return (m_activePaneIndex == 1 && splitEnabled()) ? m_right : m_left;
}

BrowserPaneController* BrowserController::otherPane() const
{
    if (!splitEnabled())
        return activePane();
    return activePane() == m_left ? m_right : m_left;
}

void BrowserController::setViewMode(ViewMode mode)
{
    if (m_viewMode == mode)
        return;
    m_viewMode = mode;

    if (!splitEnabled() && m_activePaneIndex != 0) {
        m_activePaneIndex = 0;
        emit activePaneChanged();
    }
    emit viewModeChanged();
    emit activePaneChanged();
    refreshLabels();
    emit stateChanged();
}

void BrowserController::setActivePaneIndex(int index)
{
    const int clamped = splitEnabled() ? qBound(0, index, 1) : 0;
    if (m_activePaneIndex == clamped)
        return;
    m_activePaneIndex = clamped;
    emit activePaneChanged();
    refreshLabels();
    emit stateChanged();
}

void BrowserController::toggleSplit()
{
    setViewMode(splitEnabled() ? ViewMode::Single : ViewMode::Dual);
}

void BrowserController::focusOtherPane()
{
    if (splitEnabled())
        setActivePaneIndex(m_activePaneIndex == 0 ? 1 : 0);
}

void BrowserController::navigateActive(const QString& uri)
{
    activePane()->navigateTo(uri);
}

void BrowserController::mirrorToOtherPane()
{
    if (!splitEnabled())
        return;
    otherPane()->navigateTo(activePane()->currentUri());
}

bool BrowserController::canTransfer() const
{
    if (!splitEnabled())
        return false;
    if (activePane()->targets().isEmpty())
        return false;
    return otherPane()->isWritable();
}

void BrowserController::refreshFolderFacts()
{
    const QString here = activePane() ? activePane()->currentUri() : QString();

    m_hasReport = false;
    m_reportAgeText.clear();
    m_alertCount = 0;
    m_triggeredAlertCount = 0;
    m_indexedFiles = 0;
    m_indexedText.clear();
    m_accessText.clear();
    m_accessDetail.clear();
    m_readOnlyHere = false;

    if (here.isEmpty()) {
        emit folderFactsChanged();
        return;
    }

    if (m_services.reports) {
        const QList<ReportSummary> history = m_services.reports->history(here);
        if (!history.isEmpty()) {
            m_hasReport = true;
            const qint64 days = history.first().createdAt.daysTo(QDateTime::currentDateTime());
            m_reportAgeText = days <= 0 ? QStringLiteral("today")
                : days == 1             ? QStringLiteral("yesterday")
                                        : QStringLiteral("%1 days ago").arg(days);
        }
    }

    if (m_services.alerts) {
        const QList<AlertRule> rules = m_services.alerts->rules();
        for (const AlertRule& rule : rules) {
            if (rule.targetUri != here)
                continue;
            ++m_alertCount;
            if (rule.state == AlertState::Triggered)
                ++m_triggeredAlertCount;
        }
    }

    // The index is asked about the volume this folder sits under, not about the
    // folder itself: scanning /data indexes /data/projects too, and claiming
    // otherwise would send the user to re-scan what is already there.
    //
    // Asked of the snapshot and not the database, because this runs on every
    // folder change and every alert edit, on the thread that draws the window.
    // Until the snapshot has an answer there is no tag at all -- the one thing
    // that must not happen is a folder that *is* indexed reading as one that is
    // not, which is what an empty answer would say. See ADR-0066.
    if (m_services.indexSummary && m_services.indexSummary->isKnown()) {
        const IndexVolume* best = nullptr;
        for (const IndexVolume& volume : m_services.indexSummary->volumes()) {
            if (here != volume.rootUri && !here.startsWith(volume.rootUri + QLatin1Char('/')))
                continue;
            // The closest enclosing root wins: a scan of /data/projects is a
            // better answer than a scan of /.
            if (!best || volume.rootUri.size() > best->rootUri.size())
                best = &volume;
        }
        if (best) {
            m_indexedFiles = best->fileCount;
            const qint64 days
                = best->lastScan.isValid() ? best->lastScan.daysTo(QDateTime::currentDateTime()) : -1;
            m_indexedText = days < 0 ? QStringLiteral("indexed")
                : days == 0          ? QStringLiteral("indexed today")
                : days == 1          ? QStringLiteral("indexed yesterday")
                                     : QStringLiteral("indexed %1 days ago").arg(days);
        }
    }

    // Access comes from storage, so it goes through a task like everything else
    // that does. The tag appears a moment after the listing, which is fine --
    // it is context, not something being waited on.
    if (m_services.tasks && m_services.vfs) {
        const VfsUri uri = VfsUri::fromString(here);
        if (FileSystemPtr fs = m_services.vfs->resolve(uri)) {
            auto* task = new QueryAccessTask(std::move(fs), uri);
            connect(task, &QueryAccessTask::accessReady, this,
                [this, here](const VfsUri& target, const AccessInfo& access) {
                    // The pane may have moved on while the answer was in flight.
                    if (!activePane() || activePane()->currentUri() != target.toString()
                        || here != target.toString()) {
                        return;
                    }
                    applyAccess(access);
                });
            m_services.tasks->submit(task);
        }
    }

    emit folderFactsChanged();
}

void BrowserController::applyAccess(const AccessInfo& access)
{
    const auto yes = [](AccessInfo::Answer answer) { return answer == AccessInfo::Answer::Yes; };
    const auto no = [](AccessInfo::Answer answer) { return answer == AccessInfo::Answer::No; };

    // The native form when the platform has one, because it says more than a
    // summary can. A summary otherwise -- and nothing at all when the drive had
    // nothing to say, rather than a reassuring blank.
    if (!access.nativeText.isEmpty()) {
        m_accessText = access.nativeText;
    } else if (yes(access.write)) {
        m_accessText = QStringLiteral("read+write");
    } else if (yes(access.read)) {
        m_accessText = QStringLiteral("read-only");
    } else if (no(access.read)) {
        m_accessText = QStringLiteral("no access");
    } else {
        m_accessText.clear();
    }

    m_readOnlyHere = no(access.write) || no(access.createInside);

    QStringList detail;
    if (!access.owner.isEmpty()) {
        detail.append(
            access.group.isEmpty() ? access.owner : QStringLiteral("%1:%2").arg(access.owner, access.group));
    }
    const auto note = [&detail](const char* text, AccessInfo::Answer answer) {
        if (answer != AccessInfo::Answer::Unknown) {
            detail.append(QStringLiteral("%1 %2").arg(
                answer == AccessInfo::Answer::Yes ? QStringLiteral("can") : QStringLiteral("cannot"),
                QLatin1String(text)));
        }
    };
    note("read", access.read);
    note("write", access.write);
    note("add files here", access.createInside);
    note("delete this", access.remove);
    m_accessDetail = detail.join(QStringLiteral(" · "));

    emit folderFactsChanged();
}

QString BrowserController::transferSummary() const
{
    if (!splitEnabled())
        return {};
    return QStringLiteral("%1 → %2").arg(activePane()->targetSummary(), otherPane()->displayPath());
}

void BrowserController::copyToOtherPane()
{
    startTransfer(false, {}, QStringLiteral("stop"));
}

void BrowserController::moveToOtherPane()
{
    startTransfer(true, {}, QStringLiteral("stop"));
}

void BrowserController::runTransfer(bool move, const QString& targetName, const QString& conflict)
{
    startTransfer(move, targetName, conflict);
}

QVariantMap BrowserController::transferPlan() const
{
    QVariantMap plan;
    if (!splitEnabled())
        return plan;

    const QList<VfsUri> sources = activePane()->targets();
    plan.insert(QStringLiteral("count"), static_cast<int>(sources.size()));
    plan.insert(QStringLiteral("targetPath"), otherPane()->displayPath());
    plan.insert(QStringLiteral("sourcePath"), activePane()->displayPath());

    // A single item can be renamed on arrival; a batch cannot, because there
    // would be nothing sensible to call the rest.
    plan.insert(QStringLiteral("singleName"), sources.size() == 1 ? sources.first().fileName() : QString());

    // What is already over there, from the listing the other pane has loaded.
    // No I/O, so the warning is on screen the instant the dialog opens -- which
    // is the only moment it is useful.
    QSet<QString> existing;
    FileListModel* destination = otherPane()->files();
    for (int row = 0; row < destination->rowCount(); ++row)
        existing.insert(destination->nameAt(row));

    QStringList collisions;
    qint64 bytes = 0;
    FileListModel* origin = activePane()->files();
    for (const VfsUri& source : sources) {
        const QString name = source.fileName();
        if (existing.contains(name))
            collisions.append(name);
        const int row = origin->rowOfUri(source.toString());
        if (row >= 0)
            bytes += origin->index(row, 0).data(FileListModel::SizeRole).toLongLong();
    }

    plan.insert(QStringLiteral("collisions"), collisions);
    plan.insert(QStringLiteral("sizeText"), QLocale().formattedDataSize(bytes));
    return plan;
}

void BrowserController::startTransfer(bool move, const QString& targetName, const QString& conflict)
{
    if (!splitEnabled()) {
        emit operationFailed(QStringLiteral("Switch to dual pane first"));
        return;
    }

    BrowserPaneController* source = activePane();
    BrowserPaneController* target = otherPane();

    const QList<VfsUri> sources = source->targets();
    if (sources.isEmpty())
        return;

    const VfsUri targetDir = VfsUri::fromString(target->currentUri());
    const VfsUri sourceDir = VfsUri::fromString(source->currentUri());
    if (targetDir == sourceDir) {
        emit operationFailed(QStringLiteral("Both panes show the same folder"));
        return;
    }

    TransferTask::Request request;
    request.sourceFileSystem = m_services.vfs->resolve(sources.first());
    request.targetFileSystem = m_services.vfs->resolve(targetDir);
    request.sources = sources;
    request.targetDirectory = targetDir;
    request.mode = move ? TransferTask::Mode::Move : TransferTask::Mode::Copy;
    request.onConflict = conflict == QLatin1String("overwrite") ? TransferTask::Conflict::Overwrite
        : conflict == QLatin1String("skip")                     ? TransferTask::Conflict::Skip
                                                                : TransferTask::Conflict::Fail;
    // Renaming on arrival only makes sense for a single item; the request
    // carries it as an override of the destination name.
    if (sources.size() == 1 && !targetName.trimmed().isEmpty()
        && targetName.trimmed() != sources.first().fileName()) {
        request.targetName = targetName.trimmed();
    }

    if (!request.sourceFileSystem || !request.targetFileSystem) {
        emit operationFailed(QStringLiteral("One of the panes has no drive mounted"));
        return;
    }
    if (!request.targetFileSystem->capabilities().testFlag(VfsCapability::Write)) {
        emit operationFailed(QStringLiteral("%1 is read-only").arg(target->displayPath()));
        return;
    }

    auto* task = new TransferTask(request);
    connect(task, &Task::finished, this, [this, task, sourceDir, targetDir, move] {
        if (!task->failures().isEmpty())
            emit operationFailed(task->failures().join(QLatin1String("; ")));

        // Both sides may have changed; the bus refreshes whoever is showing them.
        m_services.events->postDirectoryChanged(targetDir);
        if (move)
            m_services.events->postDirectoryChanged(sourceDir);
    });

    source->files()->clearSelection();
    m_services.tasks->submit(task);
}

QVariantMap BrowserController::saveState() const
{
    // Single versus dual is a way of looking at the same tab, not a different
    // tab, so it is remembered alongside where each pane was pointing.
    const QString mode = splitEnabled() ? QStringLiteral("dual")
        : gridEnabled()                 ? QStringLiteral("grid")
        : galleryEnabled()              ? QStringLiteral("gallery")
                                        : QStringLiteral("single");
    return {
        { QStringLiteral("viewMode"), mode },
        { QStringLiteral("left"), m_left->currentUri() },
        { QStringLiteral("right"), m_right->currentUri() },
        { QStringLiteral("activePane"), m_activePaneIndex },
    };
}

void BrowserController::restoreState(const QVariantMap& state)
{
    const QString mode = state.value(QStringLiteral("viewMode")).toString();
    // An unknown value from a newer build falls back to the plain listing
    // rather than refusing to restore the tab.
    setViewMode(mode == QLatin1String("dual")  ? ViewMode::Dual
            : mode == QLatin1String("grid")    ? ViewMode::Grid
            : mode == QLatin1String("gallery") ? ViewMode::Gallery
                                               : ViewMode::Single);

    // A remembered location may have been unmounted, renamed or deleted since.
    // navigateTo() reports that in the pane rather than refusing to restore.
    const QString left = state.value(QStringLiteral("left")).toString();
    if (!left.isEmpty())
        m_left->navigateTo(left);
    const QString right = state.value(QStringLiteral("right")).toString();
    if (!right.isEmpty())
        m_right->navigateTo(right);

    setActivePaneIndex(state.value(QStringLiteral("activePane"), 0).toInt());
}

void BrowserController::refreshLabels()
{
    const BrowserPaneController* pane = activePane();
    const QString name = pane->locationName();
    setTitle(name.isEmpty() ? QStringLiteral("Browser") : name);
    setSubtitle(splitEnabled()
            ? QStringLiteral("%1  |  %2").arg(m_left->displayPath(), m_right->displayPath())
            : pane->displayPath());
}

BrowserFeature::Config BrowserFeature::singlePaneConfig()
{
    Config config;
    config.id = QStringLiteral("mole.browser");
    config.title = QStringLiteral("Browser");
    config.description = QStringLiteral("Navigate files in a single pane.");
    config.iconText = QStringLiteral("\U0001F4C1");
    config.sortOrder = 10;
    config.initialMode = BrowserController::ViewMode::Single;
    return config;
}

BrowserFeature::Config BrowserFeature::dualPaneConfig()
{
    Config config;
    config.id = QStringLiteral("mole.commander");
    config.title = QStringLiteral("Dual pane");
    config.description
        = QStringLiteral("Two folders side by side: copy and move between them with F5 and F6.");
    config.iconText = QStringLiteral("◫");
    config.sortOrder = 15;
    config.initialMode = BrowserController::ViewMode::Dual;
    return config;
}

BrowserFeature::BrowserFeature(PluginServices services, QString defaultUri, Config config)
    : m_services(services)
    , m_defaultUri(std::move(defaultUri))
    , m_config(std::move(config))
{
}

QUrl BrowserFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/BrowserView.qml"));
}

FeatureController* BrowserFeature::createController(QObject* parent)
{
    return new BrowserController(m_services, m_defaultUri, m_config.initialMode, parent);
}

} // namespace mole
