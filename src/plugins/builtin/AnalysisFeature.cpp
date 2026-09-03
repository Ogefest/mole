#include "plugins/builtin/AnalysisFeature.h"

#include "plugins/builtin/AnalysisJob.h"
#include "ui/models/FileListModel.h"

#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"
#include "core/events/EventBus.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QCryptographicHash>
#include <QLocale>

namespace mole {
namespace {

    /// How many runs to keep per directory. Enough to see a trend, not enough
    /// to become a slow leak.
    constexpr int kHistoryKept = 50;

    QVariantMap bucketToVariant(const BucketStat& bucket, qint64 peak)
    {
        return { { QStringLiteral("label"), bucket.label }, { QStringLiteral("count"), bucket.count },
            { QStringLiteral("bytes"), bucket.bytes },
            { QStringLiteral("sizeText"), FileListModel::formatSize(bucket.bytes) },
            { QStringLiteral("peakShare"),
                peak > 0 ? static_cast<double>(bucket.count) / static_cast<double>(peak) : 0.0 } };
    }

} // namespace

AnalysisTarget::AnalysisTarget(
    PluginServices services, AnalysisStore* store, QString rootUri, QString label, QObject* parent)
    : QObject(parent)
    , m_services(services)
    , m_store(store)
    , m_rootUri(std::move(rootUri))
    , m_label(std::move(label))
    , m_extensions(new BreakdownModel(this))
{
    if (m_label.isEmpty()) {
        const VfsUri uri = VfsUri::fromString(m_rootUri);
        m_label = uri.fileName().isEmpty() ? uri.toString() : uri.fileName();
    }
}

AnalysisTarget::~AnalysisTarget()
{
    if (m_task)
        m_task->requestCancel();
}

void AnalysisTarget::loadLatest()
{
    if (!m_store)
        return;
    AnalysisReport stored = m_store->latest(m_rootUri);
    if (stored.isValid())
        setReport(std::move(stored));
    emit historyChanged();
}

void AnalysisTarget::refresh()
{
    if (!m_services.isValid() || m_busy)
        return;

    const VfsUri root = VfsUri::fromString(m_rootUri);
    FileSystemPtr fs = m_services.vfs->resolve(root);
    if (!fs) {
        m_statusText = QStringLiteral("No drive is mounted for this folder");
        emit stateChanged();
        return;
    }

    auto* task = new AnalyseDirectoryTask(std::move(fs), root, m_label);
    m_task = task;

    connect(task, &Task::statusTextChanged, this, [this, task] {
        if (m_task != task)
            return;
        m_statusText = task->statusText();
        emit stateChanged();
    });

    connect(task, &AnalyseDirectoryTask::reportReady, this, [this, task](const AnalysisReport& report) {
        if (m_task != task)
            return;
        // Filed before it is shown: a report the user can see but not compare
        // against later would defeat the point of keeping history.
        // The report is shown either way -- it is on screen and correct -- and
        // a filing that did not happen is a history that will not have it. The
        // store has said why in the log; the tab's own history list is what
        // shows the gap.
        if (m_store && m_store->save(report))
            m_store->prune(m_rootUri, kHistoryKept);
        setReport(report);
        emit historyChanged();
    });

    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_task.clear();
        m_busy = false;
        if (task->state() == Task::State::Failed)
            m_statusText = task->error().message;
        emit stateChanged();
    });

    m_busy = true;
    m_statusText = QStringLiteral("Walking the folder...");
    emit stateChanged();
    m_services.tasks->submit(task);
}

namespace {

    /// One rule per folder, derived from the folder itself: scheduling the same
    /// directory twice must change the interval, not create a second job that
    /// walks the same tree.
    QString scheduleIdFor(const QString& rootUri)
    {
        return QStringLiteral("analysis-%1")
            .arg(QString::fromLatin1(
                QCryptographicHash::hash(rootUri.toUtf8(), QCryptographicHash::Sha1).toHex().left(16)));
    }

} // namespace

QString AnalysisTarget::scheduleText() const
{
    const qint64 seconds = scheduleSeconds();
    return seconds > 0 ? ScheduleRule::describeInterval(seconds) : QString();
}

qint64 AnalysisTarget::scheduleSeconds() const
{
    if (!m_services.scheduler || !m_services.scheduler->store())
        return 0;
    const ScheduleRule rule = m_services.scheduler->store()->rule(scheduleIdFor(m_rootUri));
    return rule.isValid() && rule.enabled ? rule.intervalSeconds : 0;
}

QVariantList AnalysisTarget::schedulePresets() const
{
    QVariantList out;
    const auto presets = ScheduleRule::presets();
    for (const auto& preset : presets) {
        out.append(QVariantMap { { QStringLiteral("label"), preset.first },
            { QStringLiteral("seconds"), QVariant::fromValue(preset.second) } });
    }
    return out;
}

void AnalysisTarget::setSchedule(qint64 seconds)
{
    if (!m_services.scheduler || !m_services.scheduler->store())
        return;

    ScheduleStore* store = m_services.scheduler->store();
    const QString id = scheduleIdFor(m_rootUri);

    if (seconds <= 0) {
        store->remove(id);
        emit scheduleChanged();
        return;
    }

    ScheduleRule rule = store->rule(id);
    if (!rule.isValid()) {
        rule.id = id;
        rule.jobKind = AnalysisJob::kind();
        rule.parameters.insert(AnalysisJob::rootUriParameter(), m_rootUri);
        // A folder just analysed by hand does not need running again this
        // minute, so the clock starts from the report that already exists.
        rule.lastRunAt = m_report.isValid() ? m_report.createdAt : QDateTime();
    }
    rule.label = m_label.isEmpty() ? m_rootUri : m_label;
    rule.intervalSeconds = seconds;
    rule.enabled = true;
    store->put(rule);
    emit scheduleChanged();
}

void AnalysisTarget::showReport(const QString& id)
{
    if (!m_store)
        return;
    AnalysisReport stored = m_store->load(m_rootUri, id);
    if (stored.isValid())
        setReport(std::move(stored));
}

void AnalysisTarget::setReport(AnalysisReport report)
{
    m_report = std::move(report);
    m_extensions->setExtensions(m_report.extensions);

    // Default to comparing with the run before this one: that is the question
    // being asked almost every time.
    if (m_store) {
        const QList<ReportSummary> past = m_store->history(m_rootUri);
        m_comparisonId.clear();
        for (const ReportSummary& summary : past) {
            if (summary.id != m_report.id) {
                m_comparisonId = summary.id;
                break;
            }
        }
    }

    recomputeDiff();
    emit reportChanged();
}

void AnalysisTarget::setComparisonId(const QString& id)
{
    if (m_comparisonId == id)
        return;
    m_comparisonId = id;
    recomputeDiff();
}

void AnalysisTarget::recomputeDiff()
{
    m_diff = ReportDiff();
    if (m_store && !m_comparisonId.isEmpty() && m_report.isValid()) {
        const AnalysisReport before = m_store->load(m_rootUri, m_comparisonId);
        m_diff = ReportDiff::between(before, m_report);
    }
    emit diffChanged();
}

QVariantMap AnalysisTarget::headline() const
{
    return {
        { QStringLiteral("files"), m_report.fileCount },
        { QStringLiteral("folders"), m_report.folderCount },
        { QStringLiteral("bytes"), m_report.totalBytes },
        { QStringLiteral("sizeText"), FileListModel::formatSize(m_report.totalBytes) },
        { QStringLiteral("averageText"), FileListModel::formatSize(m_report.averageFileBytes()) },
        { QStringLiteral("kinds"), static_cast<qint64>(m_report.extensions.size()) },
        { QStringLiteral("depth"), m_report.maxDepth },
        { QStringLiteral("unreadable"), m_report.unreadableFolders },
        { QStringLiteral("partial"), m_report.partial },
        { QStringLiteral("takenAt"),
            m_report.createdAt.isValid() ? QLocale().toString(m_report.createdAt, QLocale::ShortFormat)
                                         : QString() },
    };
}

QVariantList AnalysisTarget::topFolders() const
{
    qint64 peak = 0;
    for (const FolderStat& folder : m_report.topFolders)
        peak = std::max(peak, folder.bytes);

    QVariantList out;
    for (const FolderStat& folder : m_report.topFolders) {
        out.append(
            QVariantMap { { QStringLiteral("name"), folder.name }, { QStringLiteral("uri"), folder.uri },
                { QStringLiteral("count"), folder.count }, { QStringLiteral("bytes"), folder.bytes },
                { QStringLiteral("sizeText"), FileListModel::formatSize(folder.bytes) },
                { QStringLiteral("peakShare"),
                    peak > 0 ? static_cast<double>(folder.bytes) / static_cast<double>(peak) : 0.0 } });
    }
    return out;
}

QVariantList AnalysisTarget::largestFiles() const
{
    QVariantList out;
    for (const FileStat& file : m_report.largestFiles) {
        out.append(QVariantMap { { QStringLiteral("name"), file.name }, { QStringLiteral("uri"), file.uri },
            { QStringLiteral("sizeText"), FileListModel::formatSize(file.bytes) },
            { QStringLiteral("modifiedText"),
                file.modified.isValid() ? QLocale().toString(file.modified, QLocale::ShortFormat)
                                        : QString() } });
    }
    return out;
}

QVariantList AnalysisTarget::sizeBuckets() const
{
    qint64 peak = 0;
    for (const BucketStat& bucket : m_report.sizeBuckets)
        peak = std::max(peak, bucket.count);

    QVariantList out;
    for (const BucketStat& bucket : m_report.sizeBuckets)
        out.append(bucketToVariant(bucket, peak));
    return out;
}

QVariantList AnalysisTarget::ageBuckets() const
{
    qint64 peak = 0;
    for (const BucketStat& bucket : m_report.ageBuckets)
        peak = std::max(peak, bucket.count);

    QVariantList out;
    for (const BucketStat& bucket : m_report.ageBuckets)
        out.append(bucketToVariant(bucket, peak));
    return out;
}

QVariantList AnalysisTarget::history() const
{
    QVariantList out;
    if (!m_store)
        return out;

    for (const ReportSummary& summary : m_store->history(m_rootUri)) {
        out.append(QVariantMap { { QStringLiteral("id"), summary.id },
            { QStringLiteral("takenAt"), QLocale().toString(summary.createdAt, QLocale::ShortFormat) },
            { QStringLiteral("files"), summary.fileCount },
            { QStringLiteral("sizeText"), FileListModel::formatSize(summary.totalBytes) },
            { QStringLiteral("current"), summary.id == m_report.id } });
    }
    return out;
}

QVariantMap AnalysisTarget::diffHeadline() const
{
    if (!m_diff.valid)
        return {};

    const auto signedSize = [](qint64 bytes) {
        const QString text = FileListModel::formatSize(std::abs(bytes));
        return bytes > 0 ? QStringLiteral("+%1").arg(text)
            : bytes < 0  ? QStringLiteral("-%1").arg(text)
                         : QStringLiteral("no change");
    };

    return {
        { QStringLiteral("since"), QLocale().toString(m_diff.beforeAt, QLocale::ShortFormat) },
        { QStringLiteral("filesDelta"), m_diff.fileCountDelta() },
        { QStringLiteral("foldersDelta"), m_diff.folderCountDelta() },
        { QStringLiteral("bytesDelta"), m_diff.totalBytesDelta() },
        { QStringLiteral("bytesDeltaText"), signedSize(m_diff.totalBytesDelta()) },
        { QStringLiteral("grew"), m_diff.totalBytesDelta() > 0 },
    };
}

QVariantList AnalysisTarget::diffRows() const
{
    QVariantList out;
    if (!m_diff.valid)
        return out;

    qint64 peak = 0;
    for (const ExtensionDelta& delta : m_diff.extensions)
        peak = std::max(peak, std::abs(delta.bytesDelta()));

    for (const ExtensionDelta& delta : m_diff.extensions) {
        if (!delta.changed())
            continue; // the unchanged tail is noise in a diff view

        out.append(QVariantMap {
            { QStringLiteral("extension"),
                delta.extension.isEmpty() ? QStringLiteral("(no extension)") : delta.extension },
            { QStringLiteral("countDelta"), delta.countDelta() },
            { QStringLiteral("bytesDelta"), delta.bytesDelta() },
            { QStringLiteral("bytesDeltaText"), FileListModel::formatSize(std::abs(delta.bytesDelta())) },
            { QStringLiteral("grew"), delta.bytesDelta() > 0 }, { QStringLiteral("isNew"), delta.isNew() },
            { QStringLiteral("isGone"), delta.isGone() },
            { QStringLiteral("peakShare"),
                peak > 0 ? static_cast<double>(std::abs(delta.bytesDelta())) / static_cast<double>(peak)
                         : 0.0 } });
    }
    return out;
}

// ------------------------------------------------------------------- tab

AnalysisTabController::AnalysisTabController(PluginServices services, AnalysisStore* store, QObject* parent)
    : FeatureController(QStringLiteral("Analysis"), parent)
    , m_services(services)
    , m_store(store)
{
}

void AnalysisTabController::addTarget(const QString& rootUri, const QString& label)
{
    if (rootUri.isEmpty())
        return;

    for (int i = 0; i < m_targets.size(); ++i) {
        if (m_targets.at(i)->rootUri() == rootUri) {
            setCurrentIndex(i);
            return;
        }
    }

    auto* target = new AnalysisTarget(m_services, m_store, rootUri, label, this);
    connect(target, &AnalysisTarget::stateChanged, this, &AnalysisTabController::refreshTitle);
    connect(target, &AnalysisTarget::reportChanged, this, &AnalysisTabController::refreshTitle);

    m_targets.append(target);
    // A stored report appears immediately; walking again is the user's call.
    target->loadLatest();

    emit targetsChanged();
    setCurrentIndex(static_cast<int>(m_targets.size()) - 1);
    refreshTitle();
}

void AnalysisTabController::setTargets(const QStringList& rootUris)
{
    qDeleteAll(m_targets);
    m_targets.clear();
    m_currentIndex = -1;

    for (const QString& uri : rootUris)
        addTarget(uri);

    emit targetsChanged();

    // Opening a report is opening a report. Walking a large tree again takes
    // minutes, and doing it because someone wanted to look at yesterday's
    // numbers is work nobody asked for.
    //
    // A target with nothing saved is walked, because an empty tab would be
    // useless -- but only that one.
    for (AnalysisTarget* target : std::as_const(m_targets)) {
        if (!target->hasReport())
            target->refresh();
    }
}

void AnalysisTabController::analyse(const QStringList& rootUris)
{
    setTargets(rootUris);
    // "Analyse folder" is a request for current numbers, so everything is
    // walked whether or not there is something saved to fall back on.
    refreshAll();
}

void AnalysisTabController::refreshAll()
{
    // Every selected folder is walked, not just the visible one: the user
    // asked about all of them and will switch between the results.
    for (AnalysisTarget* target : std::as_const(m_targets))
        target->refresh();
}

QVariantList AnalysisTabController::targetList() const
{
    QVariantList out;
    for (const AnalysisTarget* target : m_targets) {
        out.append(QVariantMap { { QStringLiteral("label"), target->label() },
            { QStringLiteral("rootUri"), target->rootUri() }, { QStringLiteral("busy"), target->isBusy() } });
    }
    return out;
}

void AnalysisTabController::setCurrentIndex(int index)
{
    const int clamped = m_targets.isEmpty() ? -1 : qBound(0, index, static_cast<int>(m_targets.size()) - 1);
    if (m_currentIndex == clamped)
        return;
    m_currentIndex = clamped;
    emit currentChanged();
    refreshTitle();
}

AnalysisTarget* AnalysisTabController::current() const
{
    if (m_currentIndex < 0 || m_currentIndex >= m_targets.size())
        return nullptr;
    return m_targets.at(m_currentIndex);
}

void AnalysisTabController::refreshTitle()
{
    if (m_targets.isEmpty()) {
        setTitle(QStringLiteral("Analysis"));
        setSubtitle(QString());
        return;
    }

    const AnalysisTarget* target = current();
    setTitle(m_targets.size() == 1 ? m_targets.first()->label()
                                   : QStringLiteral("Analysis (%1)").arg(m_targets.size()));

    bool anyBusy = false;
    for (const AnalysisTarget* candidate : m_targets)
        anyBusy = anyBusy || candidate->isBusy();
    setBusy(anyBusy);

    setSubtitle(target ? (target->isBusy() ? target->statusText() : target->rootUri()) : QString());
}

QVariantMap AnalysisTabController::saveState() const
{
    QStringList uris;
    for (const AnalysisTarget* target : m_targets)
        uris.append(target->rootUri());

    return { { QStringLiteral("targets"), uris }, { QStringLiteral("current"), m_currentIndex } };
}

void AnalysisTabController::restoreState(const QVariantMap& state)
{
    const QStringList uris = state.value(QStringLiteral("targets")).toStringList();
    for (const QString& uri : uris)
        addTarget(uri);
    // Restoring shows the stored reports; it does not re-walk anything, which
    // could be minutes of disk on startup.
    setCurrentIndex(state.value(QStringLiteral("current"), 0).toInt());
}

// --------------------------------------------------------------- feature

AnalysisFeature::AnalysisFeature(PluginServices services)
    : m_services(services)
    // Borrowed, not owned: an automatic run, an alert reading the latest
    // report and a tab comparing two of them must all see one history.
    , m_store(services.reports)
{
}

AnalysisFeature::~AnalysisFeature() = default;

QUrl AnalysisFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/AnalysisView.qml"));
}

FeatureController* AnalysisFeature::createController(QObject* parent)
{
    return new AnalysisTabController(m_services, m_store, parent);
}

} // namespace mole
