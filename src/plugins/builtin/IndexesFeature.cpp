#include "plugins/builtin/IndexesFeature.h"

#include "plugins/builtin/IndexScanJob.h"
#include "plugins/builtin/TimeWords.h"
#include "sdk/ScanReaders.h"

#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"
#include "core/events/EventBus.h"
#include "core/index/ScanTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QLocale>

#include <algorithm>

namespace mole {
namespace {

    /// What kind of scan built a volume, in the words the search form uses for
    /// the same fact.
    ///
    /// A volume from before the options were recorded answers *not known*, which
    /// is a third answer and not a polite way of saying no: telling somebody the
    /// index they built with metadata last week has none would be worse than
    /// admitting it cannot be told. See ADR-0057.
    QString kindInWords(const std::optional<ScanOptions>& asked)
    {
        if (!asked)
            return QStringLiteral("not known — rescan to find out");

        QStringList parts;
        parts.append(asked->metadata ? QStringLiteral("with what the files say about themselves")
                                     : QStringLiteral("names only"));
        if (asked->archives)
            parts.append(QStringLiteral("archives listed"));
        return parts.join(QStringLiteral(", "));
    }

    /// The bit of a uri worth reading. A column of identical prefixes tells
    /// nobody which tree is which.
    QString shortLabel(const QString& rootUri)
    {
        const VfsUri uri = VfsUri::fromString(rootUri);
        const QString name = uri.fileName();
        return name.isEmpty() ? rootUri : name;
    }

} // namespace

IndexesController::IndexesController(PluginServices services, QObject* parent)
    : FeatureController(QStringLiteral("Indexes"), parent)
    , m_services(services)
{
    // A scan that finishes anywhere -- a search tab, the folder menu, the
    // nightly rule -- shows up here without the tab being reopened.
    if (m_services.events) {
        connect(m_services.events, &EventBus::indexUpdated, this, [this](qint64, qint64) { refresh(); });
    }
    // A scan that is running is otherwise invisible except as one line in the
    // task strip -- and a scheduled scan starting while somebody is working is a
    // mystery slowdown. Followed here so the row it belongs to says so.
    if (m_services.tasks) {
        connect(m_services.tasks, &TaskManager::taskAppended, this, &IndexesController::watch);
        const QList<Task*> already = m_services.tasks->tasks();
        for (Task* task : already)
            watch(task);
    }
    rebuild();
}

void IndexesController::watch(Task* task)
{
    // By type rather than by title: a scan is a ScanTask, and matching on words
    // would break the first time one of them was reworded.
    if (!qobject_cast<ScanTask*>(task))
        return;

    connect(task, &Task::statusTextChanged, this, &IndexesController::volumesChanged);
    connect(task, &Task::stateChanged, this, &IndexesController::volumesChanged);
    // The counts and the date move when it lands, so the whole list is re-read.
    connect(task, &Task::finished, this, [this] { rebuild(); });
}

Task* IndexesController::scanOf(const QString& rootUri) const
{
    if (!m_services.tasks)
        return nullptr;

    const VfsUri root = VfsUri::fromString(rootUri);
    const QList<Task*> tasks = m_services.tasks->tasks();
    for (Task* task : tasks) {
        if (!qobject_cast<ScanTask*>(task) || task->isFinished())
            continue;
        if (task->touching().contains(root))
            return task;
    }
    return nullptr;
}

std::optional<IndexVolume> IndexesController::volumeWithId(qint64 volumeId) const
{
    for (const IndexVolume& volume : m_volumes) {
        if (volume.id == volumeId)
            return volume;
    }
    return std::nullopt;
}

ScanOptions IndexesController::optionsFor(const IndexVolume& volume, bool full) const
{
    // What it records, so a rescan repeats the scan that built it rather than a
    // poorer one. A volume from before the options were recorded gets what the
    // index dialog opens on, which is the honest guess and is said in the tab.
    ScanOptions options = volume.scan.value_or(ScanOptions { true, false, true });
    // Nothing kept and everything walked, which is what "full" means here and in
    // the dialog.
    options.incremental = !full;
    return options;
}

QVariantList IndexesController::schedulePresets() const
{
    QVariantList out;
    const auto presets = ScheduleRule::presets();
    for (const auto& preset : presets) {
        out.append(QVariantMap { { QStringLiteral("label"), preset.first },
            { QStringLiteral("seconds"), QVariant::fromValue(preset.second) } });
    }
    return out;
}

bool IndexesController::rescan(qint64 volumeId, bool full)
{
    const std::optional<IndexVolume> volume = volumeWithId(volumeId);
    if (!volume || !m_services.isValid())
        return false;
    // One scan per volume at a time. Two at once would have the second one's
    // generation swap drop the first one's rows.
    if (scanOf(volume->rootUri))
        return false;

    const VfsUri root = VfsUri::fromString(volume->rootUri);
    FileSystemPtr fs = m_services.vfs->resolve(root);
    if (!fs)
        return false; // an unplugged drive, which is not this tab's to explain

    auto* task = new ScanTask(fs, root, volume->label, m_services.index);
    applyScanOptions(*task, optionsFor(*volume, full), m_services, fs, root);
    // Watched before it is submitted, so the row shows the first status line
    // rather than starting to move only once something else has happened.
    watch(task);
    connect(task, &Task::finished, this, [this, task] {
        if (task->state() == Task::State::Succeeded && m_services.events)
            m_services.events->postIndexUpdated(-1, task->filesIndexed());
    });
    m_services.tasks->submit(task);
    emit volumesChanged();
    return true;
}

bool IndexesController::setSchedule(qint64 volumeId, qint64 seconds)
{
    const std::optional<IndexVolume> volume = volumeWithId(volumeId);
    if (!volume || !m_services.scheduler || !m_services.scheduler->store())
        return false;

    // Incremental whatever this volume's own last scan was: a nightly full walk
    // of the tree this exists for is hours a night for nothing.
    ScanOptions nightly = optionsFor(*volume, false);
    IndexScanJob::schedule(*m_services.scheduler->store(), volume->rootUri, seconds, nightly,
        QStringLiteral("Re-index %1").arg(volume->label));
    emit volumesChanged();
    return true;
}

bool IndexesController::forget(qint64 volumeId)
{
    if (!m_services.index)
        return false;
    const std::optional<IndexVolume> volume = volumeWithId(volumeId);
    if (!volume)
        return false;
    // A scan writing into a volume that is being deleted would put its rows back.
    if (Task* running = scanOf(volume->rootUri))
        running->requestCancel();

    if (!m_services.index->removeVolume(volumeId).ok())
        return false;
    // The rule outliving the index it refreshes would rebuild it tonight, which
    // is not what "forget this index" means.
    if (m_services.scheduler && m_services.scheduler->store()) {
        IndexScanJob::schedule(*m_services.scheduler->store(), volume->rootUri, 0, ScanOptions {}, QString());
    }
    rebuild();
    if (m_services.events)
        m_services.events->postIndexUpdated(-1, 0);
    return true;
}

bool IndexesController::stopScan(qint64 volumeId)
{
    const std::optional<IndexVolume> volume = volumeWithId(volumeId);
    if (!volume)
        return false;
    Task* running = scanOf(volume->rootUri);
    if (!running)
        return false;
    running->requestCancel();
    return true;
}

void IndexesController::rebuild()
{
    m_volumes.clear();
    if (m_services.index) {
        if (Result<QList<IndexVolume>> listed = m_services.index->volumes(); listed.ok())
            m_volumes = listed.value();
    }

    // Stalest first, because the row worth looking at is the one that has not
    // been scanned for longest. A volume that has never finished a scan has no
    // date at all and is staler than any of them.
    std::sort(m_volumes.begin(), m_volumes.end(), [](const IndexVolume& a, const IndexVolume& b) {
        if (a.lastScan.isValid() != b.lastScan.isValid())
            return !a.lastScan.isValid();
        if (a.lastScan != b.lastScan)
            return a.lastScan < b.lastScan;
        return a.label.localeAwareCompare(b.label) < 0;
    });

    setSubtitle(m_volumes.isEmpty()
            ? QStringLiteral("nothing indexed yet")
            : QStringLiteral("%1 indexes, %2 entries").arg(volumeCount()).arg(totalEntriesText()));

    emit volumesChanged();
}

void IndexesController::refresh()
{
    rebuild();
}

std::optional<ScheduleRule> IndexesController::ruleFor(const QString& rootUri) const
{
    if (!m_services.scheduler || !m_services.scheduler->store())
        return std::nullopt;

    const QList<ScheduleRule> rules = m_services.scheduler->store()->rules();
    for (const ScheduleRule& rule : rules) {
        if (rule.jobKind == IndexScanJob::kind()
            && rule.parameters.value(IndexScanJob::rootUriParameter()).toString() == rootUri) {
            return rule;
        }
    }
    return std::nullopt;
}

QVariantList IndexesController::volumes() const
{
    QVariantList out;
    const QLocale locale;
    const QDateTime now = QDateTime::currentDateTime();

    for (const IndexVolume& volume : m_volumes) {
        if (!m_filter.isEmpty() && !volume.rootUri.contains(m_filter, Qt::CaseInsensitive)
            && !volume.label.contains(m_filter, Qt::CaseInsensitive)) {
            continue;
        }

        const std::optional<ScheduleRule> rule = ruleFor(volume.rootUri);
        const Task* running = scanOf(volume.rootUri);
        // A rule that is paused is not keeping anything fresh, and saying it is
        // scheduled would be the kind of confident wrong answer this tab exists
        // to remove.
        const bool kept = rule && rule->enabled;

        out.append(QVariantMap {
            { QStringLiteral("id"), QVariant::fromValue(volume.id) },
            { QStringLiteral("rootUri"), volume.rootUri },
            { QStringLiteral("label"), volume.label.isEmpty() ? shortLabel(volume.rootUri) : volume.label },
            { QStringLiteral("scannedText"),
                volume.lastScan.isValid() ? QStringLiteral("scanned %1").arg(ageInWords(volume.lastScan))
                                          : QStringLiteral("never finished a scan") },
            { QStringLiteral("scannedAt"),
                volume.lastScan.isValid() ? volume.lastScan.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                          : QString() },
            { QStringLiteral("entryCount"), QVariant::fromValue(volume.fileCount) },
            { QStringLiteral("entryCountText"), locale.toString(volume.fileCount) },
            { QStringLiteral("kindText"), kindInWords(volume.scan) },
            { QStringLiteral("kindKnown"), volume.scan.has_value() },
            { QStringLiteral("hasMetadata"), volume.scan && volume.scan->metadata },
            { QStringLiteral("hasArchives"), volume.scan && volume.scan->archives },
            { QStringLiteral("running"), running != nullptr },
            // What it has covered so far, which is the scan's own line: a count
            // of entries, what it kept, and what it could not read.
            { QStringLiteral("progressText"), running ? running->statusText() : QString() },
            { QStringLiteral("scheduled"), kept },
            { QStringLiteral("scheduleSeconds"),
                QVariant::fromValue(kept ? rule->intervalSeconds : qint64(0)) },
            { QStringLiteral("scheduleText"),
                kept ? ScheduleRule::describeInterval(rule->intervalSeconds)
                     : (rule ? QStringLiteral("paused") : QStringLiteral("not on a clock")) },
            { QStringLiteral("nextDueText"),
                !kept ? QString()
                      : (!rule->lastRunAt.isValid()
                                ? QStringLiteral("as soon as possible")
                                : QStringLiteral("in %1").arg(locale.toString(rule->dueAt(),
                                      rule->dueAt().date() == now.date() ? QStringLiteral("HH:mm")
                                                                         : QStringLiteral("ddd HH:mm")))) },
        });
    }
    return out;
}

int IndexesController::volumeCount() const
{
    return static_cast<int>(m_volumes.size());
}

QString IndexesController::totalEntriesText() const
{
    qint64 entries = 0;
    for (const IndexVolume& volume : m_volumes)
        entries += volume.fileCount;
    return QLocale().toString(entries);
}

int IndexesController::scheduledCount() const
{
    int kept = 0;
    for (const IndexVolume& volume : m_volumes) {
        const std::optional<ScheduleRule> rule = ruleFor(volume.rootUri);
        if (rule && rule->enabled)
            ++kept;
    }
    return kept;
}

void IndexesController::setFilter(const QString& filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    emit filterChanged();
    emit volumesChanged();
}

QVariantMap IndexesController::saveState() const
{
    return { { QStringLiteral("filter"), m_filter } };
}

void IndexesController::restoreState(const QVariantMap& state)
{
    setFilter(state.value(QStringLiteral("filter")).toString());
}

IndexesFeature::IndexesFeature(PluginServices services)
    : m_services(services)
{
}

QUrl IndexesFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/IndexesView.qml"));
}

FeatureController* IndexesFeature::createController(QObject* parent)
{
    return new IndexesController(m_services, parent);
}

} // namespace mole
