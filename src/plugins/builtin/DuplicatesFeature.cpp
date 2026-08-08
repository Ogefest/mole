#include "plugins/builtin/DuplicatesFeature.h"

#include "core/duplicates/Strategies.h"
#include "core/events/EventBus.h"
#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/VfsManager.h"

#include <QLocale>

namespace mole {
namespace {

    std::unique_ptr<IDuplicateStrategy> strategyById(const QString& id)
    {
        auto all = IDuplicateStrategy::all();
        for (auto& strategy : all) {
            if (strategy->id() == id)
                return std::move(strategy);
        }
        return std::make_unique<SameContentStrategy>();
    }

} // namespace

DuplicatesController::DuplicatesController(PluginServices services, QObject* parent)
    : FeatureController(QStringLiteral("Duplicates"), parent)
    , m_services(services)
{
}

DuplicatesController::~DuplicatesController()
{
    if (m_task)
        m_task->requestCancel();
}

void DuplicatesController::setTargets(const QStringList& uris)
{
    // Folders are what a duplicate scan searches. Handed a selection of files,
    // the folders they live in are the sensible reading -- looking for
    // duplicates "of these three files" is a different question.
    QStringList roots;
    for (const QString& uri : uris) {
        const VfsUri parsed = VfsUri::fromString(uri);
        if (!parsed.isValid())
            continue;
        const QString root = parsed.toString();
        if (!roots.contains(root))
            roots.append(root);
    }

    m_roots = roots;
    setSubtitle(m_roots.size() == 1 ? VfsUri::fromString(m_roots.first()).fileName()
                                    : QStringLiteral("%1 folders").arg(m_roots.size()));
    emit rootsChanged();
    emit stateChanged();
}

QVariantList DuplicatesController::strategies() const
{
    QVariantList out;
    const auto all = IDuplicateStrategy::all();
    for (const auto& strategy : all) {
        out.append(QVariantMap { { QStringLiteral("id"), strategy->id() },
            { QStringLiteral("label"), strategy->label() },
            { QStringLiteral("description"), strategy->description() },
            { QStringLiteral("stages"), strategy->stageNames() } });
    }
    return out;
}

QString DuplicatesController::strategyDescription() const
{
    return strategyById(m_strategyId)->description();
}

void DuplicatesController::setStrategyId(const QString& id)
{
    if (m_strategyId == id)
        return;
    m_strategyId = id;
    emit optionsChanged();
    emit stateChanged();
}

void DuplicatesController::setMinimumSize(qint64 bytes)
{
    if (m_minimumSize == bytes)
        return;
    m_minimumSize = std::max<qint64>(0, bytes);
    emit optionsChanged();
    emit stateChanged();
}

QVariantList DuplicatesController::groups() const
{
    QVariantList out;
    const QLocale locale;

    for (const DuplicateGroup& group : m_groups) {
        QVariantList files;
        for (const FileEntry& entry : group.files) {
            const QString uri = entry.uri.toString();
            files.append(QVariantMap { { QStringLiteral("uri"), uri }, { QStringLiteral("name"), entry.name },
                { QStringLiteral("location"), entry.uri.parent().toString() },
                { QStringLiteral("sizeText"), locale.formattedDataSize(entry.size) },
                { QStringLiteral("modifiedText"),
                    entry.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm")) },
                { QStringLiteral("selected"), m_selected.contains(uri) } });
        }

        out.append(QVariantMap { { QStringLiteral("count"), group.files.size() },
            { QStringLiteral("sizeText"), locale.formattedDataSize(group.files.first().size) },
            { QStringLiteral("reclaimableText"), locale.formattedDataSize(group.reclaimable) },
            { QStringLiteral("files"), files } });
    }
    return out;
}

QString DuplicatesController::summary() const
{
    if (isScanning())
        return QStringLiteral("scanning…");
    if (!m_hasRun)
        return {};
    if (m_groups.isEmpty())
        return QStringLiteral("no duplicates found");

    qint64 reclaimable = 0;
    for (const DuplicateGroup& group : m_groups)
        reclaimable += group.reclaimable;

    return QStringLiteral("%1 groups · %2 could be freed")
        .arg(m_groups.size())
        .arg(QLocale().formattedDataSize(reclaimable));
}

QStringList DuplicatesController::selectedUris() const
{
    QStringList out = m_selected.values();
    out.sort();
    return out;
}

QString DuplicatesController::selectedSizeText() const
{
    qint64 bytes = 0;
    for (const DuplicateGroup& group : m_groups) {
        for (const FileEntry& entry : group.files) {
            if (m_selected.contains(entry.uri.toString()))
                bytes += entry.size;
        }
    }
    return QLocale().formattedDataSize(bytes);
}

void DuplicatesController::scan()
{
    if (!m_services.isValid() || m_roots.isEmpty() || m_task)
        return;

    QList<VfsUri> roots;
    for (const QString& uri : std::as_const(m_roots))
        roots.append(VfsUri::fromString(uri));

    m_groups.clear();
    m_selected.clear();
    emit resultsChanged();
    emit selectionChanged();

    auto* task = new FindDuplicatesTask(m_services.vfs, roots, strategyById(m_strategyId));
    task->setMinimumSize(m_minimumSize);
    m_task = task;
    setBusy(true);

    connect(task, &FindDuplicatesTask::groupsReady, this,
        [this](const QList<DuplicateGroup>& groups) { m_groups = groups; });
    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_task.clear();
        setBusy(false);
        m_hasRun = true;
        emit resultsChanged();
        emit selectionChanged();
    });

    m_services.tasks->submit(task);
    emit resultsChanged();
}

void DuplicatesController::cancel()
{
    if (m_task)
        m_task->requestCancel();
}

void DuplicatesController::toggle(const QString& uri)
{
    if (m_selected.contains(uri))
        m_selected.remove(uri);
    else
        m_selected.insert(uri);
    emit selectionChanged();
    emit resultsChanged();
}

void DuplicatesController::clearSelection()
{
    m_selected.clear();
    emit selectionChanged();
    emit resultsChanged();
}

void DuplicatesController::selectAllBut(const std::function<int(const QList<FileEntry>&)>& chooseKeeper)
{
    m_selected.clear();
    for (const DuplicateGroup& group : m_groups) {
        const int keeper = chooseKeeper(group.files);
        for (int i = 0; i < group.files.size(); ++i) {
            if (i != keeper)
                m_selected.insert(group.files.at(i).uri.toString());
        }
    }
    emit selectionChanged();
    emit resultsChanged();
}

void DuplicatesController::keepNewest()
{
    selectAllBut([](const QList<FileEntry>& files) {
        int best = 0;
        for (int i = 1; i < files.size(); ++i) {
            if (files.at(i).modified > files.at(best).modified)
                best = i;
        }
        return best;
    });
}

void DuplicatesController::keepOldest()
{
    selectAllBut([](const QList<FileEntry>& files) {
        int best = 0;
        for (int i = 1; i < files.size(); ++i) {
            if (files.at(i).modified < files.at(best).modified)
                best = i;
        }
        return best;
    });
}

void DuplicatesController::keepShortestPath()
{
    // The copy nearest the top of the tree is usually the original; the ones
    // buried in "old", "backup" and "copy of copy" are usually not.
    selectAllBut([](const QList<FileEntry>& files) {
        int best = 0;
        for (int i = 1; i < files.size(); ++i) {
            if (files.at(i).uri.path().size() < files.at(best).uri.path().size())
                best = i;
        }
        return best;
    });
}

void DuplicatesController::deleteSelected()
{
    if (!m_services.isValid() || m_selected.isEmpty() || m_task)
        return;

    // Grouped by drive, because a delete task belongs to one backend.
    QHash<QString, QList<VfsUri>> byDrive;
    for (const QString& uri : std::as_const(m_selected)) {
        const VfsUri parsed = VfsUri::fromString(uri);
        if (parsed.isValid())
            byDrive[parsed.scheme() + QLatin1Char('/') + parsed.authority()].append(parsed);
    }

    for (auto it = byDrive.constBegin(); it != byDrive.constEnd(); ++it) {
        FileSystemPtr fs = m_services.vfs->resolve(it.value().first());
        if (!fs)
            continue;

        auto* task = new DeleteTask(fs, it.value());
        connect(task, &Task::finished, this, [this] {
            // The results now describe files that may be gone, so they are
            // cleared rather than left to look actionable.
            m_groups.clear();
            m_selected.clear();
            m_hasRun = false;
            emit resultsChanged();
            emit selectionChanged();
        });
        m_services.tasks->submit(task);
    }
}

QVariantMap DuplicatesController::saveState() const
{
    return { { QStringLiteral("roots"), m_roots }, { QStringLiteral("strategy"), m_strategyId },
        { QStringLiteral("minimumSize"), m_minimumSize } };
}

void DuplicatesController::restoreState(const QVariantMap& state)
{
    setStrategyId(state.value(QStringLiteral("strategy"), m_strategyId).toString());
    setMinimumSize(state.value(QStringLiteral("minimumSize"), m_minimumSize).toLongLong());
    // Results are not restored: they describe a filesystem as it was, and
    // presenting stale duplicates as current is how someone deletes the wrong
    // copy of something.
    setTargets(state.value(QStringLiteral("roots")).toStringList());
}

DuplicatesFeature::DuplicatesFeature(PluginServices services)
    : m_services(services)
{
}

QUrl DuplicatesFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/DuplicatesView.qml"));
}

FeatureController* DuplicatesFeature::createController(QObject* parent)
{
    return new DuplicatesController(m_services, parent);
}

} // namespace mole
