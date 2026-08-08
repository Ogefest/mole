#include "plugins/builtin/ReportsFeature.h"

#include "core/events/EventBus.h"

#include <QLocale>

#include <algorithm>

namespace mole {
namespace {

    QString relativeTime(const QDateTime& when)
    {
        if (!when.isValid())
            return QStringLiteral("unknown");
        const qint64 seconds = when.secsTo(QDateTime::currentDateTime());
        if (seconds < 3600)
            return QStringLiteral("%1 min ago").arg(std::max<qint64>(1, seconds / 60));
        if (seconds < 86400)
            return QStringLiteral("%1 h ago").arg(seconds / 3600);
        const qint64 days = seconds / 86400;
        return days == 1 ? QStringLiteral("yesterday") : QStringLiteral("%1 days ago").arg(days);
    }

    /// The bit of a uri worth reading. A column of identical prefixes tells nobody
    /// which folder is which.
    QString shortLabel(const QString& rootUri)
    {
        const VfsUri uri = VfsUri::fromString(rootUri);
        const QString name = uri.fileName();
        return name.isEmpty() ? rootUri : name;
    }

} // namespace

ReportsController::ReportsController(PluginServices services, AnalysisStore* store, QObject* parent)
    : FeatureController(QStringLiteral("Reports"), parent)
    , m_services(services)
    , m_store(store)
{
    // A run filed by the scheduler, or by an analysis tab, appears here without
    // the user reopening the tab.
    if (m_services.events) {
        connect(m_services.events, &EventBus::directoryChanged, this, [this](const VfsUri&) { refresh(); });
    }
    rebuild();
}

void ReportsController::rebuild()
{
    m_folders.clear();
    if (!m_store) {
        emit foldersChanged();
        emit selectionChanged();
        return;
    }

    const QStringList roots = m_store->analysedRoots();
    for (const QString& root : roots) {
        const QList<ReportSummary> runs = m_store->history(root);
        if (runs.isEmpty())
            continue; // a folder whose reports were all deleted is not a folder
        m_folders.append(Folder { root, shortLabel(root), runs });
    }

    // Most recently touched first: a library sorted by name makes you hunt for
    // the one thing that changed.
    std::sort(m_folders.begin(), m_folders.end(),
        [](const Folder& a, const Folder& b) { return a.runs.first().createdAt > b.runs.first().createdAt; });

    if (!m_selectedRoot.isEmpty()) {
        const bool stillThere = std::any_of(m_folders.begin(), m_folders.end(),
            [this](const Folder& folder) { return folder.rootUri == m_selectedRoot; });
        if (!stillThere)
            m_selectedRoot.clear();
    }
    if (m_selectedRoot.isEmpty() && !m_folders.isEmpty())
        m_selectedRoot = m_folders.first().rootUri;

    setSubtitle(m_folders.isEmpty()
            ? QStringLiteral("nothing saved yet")
            : QStringLiteral("%1 folders, %2 runs").arg(folderCount()).arg(reportCount()));

    emit foldersChanged();
    emit selectionChanged();
}

void ReportsController::refresh()
{
    rebuild();
}

QVariantList ReportsController::folders() const
{
    QVariantList out;
    const QLocale locale;

    for (const Folder& folder : m_folders) {
        if (!m_filter.isEmpty() && !folder.rootUri.contains(m_filter, Qt::CaseInsensitive))
            continue;

        const ReportSummary& latest = folder.runs.first();
        // The oldest run bounds how far back a comparison can reach, which is
        // the question someone opening this list is usually asking.
        const ReportSummary& oldest = folder.runs.last();

        out.append(QVariantMap {
            { QStringLiteral("rootUri"), folder.rootUri },
            { QStringLiteral("label"), folder.label },
            { QStringLiteral("runCount"), folder.runs.size() },
            { QStringLiteral("latestText"), relativeTime(latest.createdAt) },
            { QStringLiteral("latestAt"), latest.createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) },
            { QStringLiteral("sizeText"), locale.formattedDataSize(latest.totalBytes) },
            { QStringLiteral("fileCountText"), locale.toString(latest.fileCount) },
            { QStringLiteral("spanText"),
                folder.runs.size() < 2 ? QStringLiteral("one run")
                                       : QStringLiteral("history back to %1")
                                             .arg(oldest.createdAt.toString(QStringLiteral("yyyy-MM-dd"))) },
            { QStringLiteral("selected"), folder.rootUri == m_selectedRoot },
        });
    }
    return out;
}

QVariantList ReportsController::runs() const
{
    QVariantList out;
    if (m_selectedRoot.isEmpty())
        return out;

    const QLocale locale;
    for (const Folder& folder : m_folders) {
        if (folder.rootUri != m_selectedRoot)
            continue;

        qint64 previousBytes = -1;
        // Walked oldest first so each run can be compared with the one before,
        // then reversed for display: "grew by 2 GB" is the useful column.
        QVariantList rows;
        for (int i = folder.runs.size() - 1; i >= 0; --i) {
            const ReportSummary& run = folder.runs.at(i);
            QString change;
            if (previousBytes >= 0) {
                const qint64 delta = run.totalBytes - previousBytes;
                if (delta == 0)
                    change = QStringLiteral("no change");
                else
                    change = QStringLiteral("%1%2").arg(delta > 0 ? QStringLiteral("+") : QStringLiteral("−"),
                        locale.formattedDataSize(std::llabs(delta)));
            }
            previousBytes = run.totalBytes;

            rows.append(QVariantMap {
                { QStringLiteral("id"), run.id },
                { QStringLiteral("rootUri"), run.rootUri },
                { QStringLiteral("takenAt"), run.createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) },
                { QStringLiteral("whenText"), relativeTime(run.createdAt) },
                { QStringLiteral("sizeText"), locale.formattedDataSize(run.totalBytes) },
                { QStringLiteral("fileCountText"), locale.toString(run.fileCount) },
                { QStringLiteral("changeText"), change },
                { QStringLiteral("grew"), change.startsWith(QStringLiteral("+")) },
            });
        }
        std::reverse(rows.begin(), rows.end());
        return rows;
    }
    return out;
}

void ReportsController::setSelectedRoot(const QString& root)
{
    if (m_selectedRoot == root)
        return;
    m_selectedRoot = root;
    emit selectionChanged();
    emit foldersChanged();
}

void ReportsController::setFilter(const QString& filter)
{
    if (m_filter == filter)
        return;
    m_filter = filter;
    emit filterChanged();
    emit foldersChanged();
}

int ReportsController::folderCount() const
{
    return static_cast<int>(m_folders.size());
}

int ReportsController::reportCount() const
{
    int total = 0;
    for (const Folder& folder : m_folders)
        total += static_cast<int>(folder.runs.size());
    return total;
}

QString ReportsController::totalSizeText() const
{
    qint64 bytes = 0;
    for (const Folder& folder : m_folders)
        bytes += folder.runs.first().totalBytes;
    return QLocale().formattedDataSize(bytes);
}

bool ReportsController::removeRun(const QString& rootUri, const QString& id)
{
    if (!m_store)
        return false;
    if (!m_store->remove(rootUri, id))
        return false;
    rebuild();
    return true;
}

bool ReportsController::forgetFolder(const QString& rootUri)
{
    if (!m_store)
        return false;
    // prune(keep = 0) removes every run rather than special-casing a loop here.
    const int removed = m_store->prune(rootUri, 0);
    rebuild();
    return removed > 0;
}

QVariantMap ReportsController::saveState() const
{
    return { { QStringLiteral("selectedRoot"), m_selectedRoot }, { QStringLiteral("filter"), m_filter } };
}

void ReportsController::restoreState(const QVariantMap& state)
{
    setFilter(state.value(QStringLiteral("filter")).toString());
    const QString root = state.value(QStringLiteral("selectedRoot")).toString();
    if (!root.isEmpty())
        setSelectedRoot(root);
}

ReportsFeature::ReportsFeature(PluginServices services, AnalysisStore* store)
    : m_services(services)
    , m_store(store)
{
}

QUrl ReportsFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/ReportsView.qml"));
}

FeatureController* ReportsFeature::createController(QObject* parent)
{
    return new ReportsController(m_services, m_store, parent);
}

} // namespace mole
