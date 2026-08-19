#include "plugins/builtin/IndexesFeature.h"

#include "plugins/builtin/IndexScanJob.h"
#include "plugins/builtin/TimeWords.h"

#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"
#include "core/events/EventBus.h"

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
    rebuild();
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
            { QStringLiteral("scheduled"), kept },
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
