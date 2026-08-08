#include "plugins/builtin/SearchFeatures.h"

#include "ui/models/FileListModel.h"

#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/IndexSearchTask.h"
#include "core/index/ScanTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

namespace mole {

// ---------------------------------------------------------------- live search

LiveSearchController::LiveSearchController(PluginServices services, QString rootUri, QObject* parent)
    : FeatureController(QStringLiteral("Quick search"), parent)
    , m_services(services)
    , m_results(new FileListModel(this))
    , m_rootUri(std::move(rootUri))
{
    m_results->setShowHidden(true);
    setSubtitle(m_rootUri);
}

LiveSearchController::~LiveSearchController()
{
    if (m_task)
        m_task->requestCancel();
}

void LiveSearchController::setRootUri(const QString& uri)
{
    if (m_rootUri == uri)
        return;
    m_rootUri = uri;
    setSubtitle(uri);
    emit rootUriChanged();
    emit stateChanged();
}

void LiveSearchController::setQueryText(const QString& text)
{
    if (m_queryText == text)
        return;
    m_queryText = text;
    setTitle(text.isEmpty() ? QStringLiteral("Quick search") : QStringLiteral("\"%1\"").arg(text));
    emit queryTextChanged();
    emit stateChanged();
}

void LiveSearchController::setExtension(const QString& extension)
{
    if (m_extension == extension)
        return;
    m_extension = extension;
    emit criteriaChanged();
}

void LiveSearchController::setCaseSensitive(bool sensitive)
{
    if (m_caseSensitive == sensitive)
        return;
    m_caseSensitive = sensitive;
    emit criteriaChanged();
}

void LiveSearchController::start()
{
    if (!m_services.isValid())
        return;

    // Starting a new search abandons the old one rather than racing it.
    stop();

    const VfsUri root = VfsUri::fromString(m_rootUri);
    FileSystemPtr fs = m_services.vfs->resolve(root);
    if (!fs) {
        setStatusText(QStringLiteral("No drive is mounted for %1").arg(m_rootUri));
        return;
    }

    m_results->clear();
    m_truncated = false;

    LiveSearchTask::Criteria criteria;
    criteria.text = m_queryText;
    criteria.extension = m_extension;
    criteria.caseSensitive = m_caseSensitive;

    auto* task = new LiveSearchTask(std::move(fs), root, criteria);
    m_task = task;

    connect(task, &LiveSearchTask::hitsFound, this, [this, task](const FileEntryList& batch) {
        if (m_task == task)
            m_results->appendEntries(batch);
    });

    connect(task, &Task::statusTextChanged, this, [this, task] {
        if (m_task == task)
            setStatusText(task->statusText());
    });

    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_truncated = task->truncated();
        setRunning(false);
        setStatusText(task->state() == Task::State::Failed ? task->error().message : task->statusText());
        m_task.clear();
    });

    setRunning(true);
    m_services.tasks->submit(task);
}

void LiveSearchController::stop()
{
    if (m_task) {
        m_task->requestCancel();
        m_task.clear();
    }
    setRunning(false);
}

QVariantMap LiveSearchController::saveState() const
{
    // The criteria, not the results: re-running a walk on startup would be a
    // surprise, and stale hits would be worse.
    return {
        { QStringLiteral("root"), m_rootUri },
        { QStringLiteral("query"), m_queryText },
        { QStringLiteral("extension"), m_extension },
        { QStringLiteral("caseSensitive"), m_caseSensitive },
    };
}

void LiveSearchController::restoreState(const QVariantMap& state)
{
    const QString root = state.value(QStringLiteral("root")).toString();
    if (!root.isEmpty())
        setRootUri(root);
    setQueryText(state.value(QStringLiteral("query")).toString());
    setExtension(state.value(QStringLiteral("extension")).toString());
    setCaseSensitive(state.value(QStringLiteral("caseSensitive"), false).toBool());
}

void LiveSearchController::setRunning(bool running)
{
    if (m_running == running)
        return;
    m_running = running;
    setBusy(running);
    emit runningChanged();
}

void LiveSearchController::setStatusText(const QString& text)
{
    if (m_statusText == text)
        return;
    m_statusText = text;
    emit statusChanged();
}

// --------------------------------------------------------------- index search

IndexSearchController::IndexSearchController(PluginServices services, QObject* parent)
    : FeatureController(QStringLiteral("Indexed search"), parent)
    , m_services(services)
    , m_results(new FileListModel(this))
{
    m_results->setShowHidden(true);

    // A finished scan should make its results searchable immediately, without
    // the user knowing they have to reload a dropdown.
    if (m_services.events) {
        connect(m_services.events, &EventBus::indexUpdated, this, [this] { refreshVolumes(); });
    }

    refreshVolumes();
}

IndexSearchController::~IndexSearchController()
{
    if (m_task)
        m_task->requestCancel();
}

void IndexSearchController::setQueryText(const QString& text)
{
    if (m_queryText == text)
        return;
    m_queryText = text;
    emit queryTextChanged();
    emit stateChanged();
}

void IndexSearchController::setVolumeIndex(int index)
{
    const int clamped = qBound(0, index, static_cast<int>(m_volumeLabels.size()) - 1);
    if (m_volumeIndex == clamped)
        return;
    m_volumeIndex = clamped;
    emit volumeIndexChanged();
}

void IndexSearchController::refreshVolumes()
{
    m_volumeLabels = { QStringLiteral("All volumes") };
    m_volumeIds = { -1 };

    if (m_services.index) {
        Result<QList<IndexVolume>> volumes = m_services.index->volumes();
        if (volumes.ok()) {
            for (const IndexVolume& volume : volumes.value()) {
                m_volumeLabels.append(
                    QStringLiteral("%1 (%2 entries)").arg(volume.label).arg(volume.fileCount));
                m_volumeIds.append(volume.id);
            }
        }
    }

    if (m_volumeIndex >= m_volumeLabels.size())
        setVolumeIndex(0);
    emit volumesChanged();
}

void IndexSearchController::search()
{
    if (!m_services.isValid())
        return;

    if (m_task) {
        m_task->requestCancel();
        m_task.clear();
    }

    IndexSearchQuery query;
    query.text = m_queryText;
    query.volumeId
        = m_volumeIndex >= 0 && m_volumeIndex < m_volumeIds.size() ? m_volumeIds.at(m_volumeIndex) : -1;

    auto* task = new IndexSearchTask(m_services.index, query);
    m_task = task;

    connect(task, &IndexSearchTask::resultsReady, this, [this, task](const FileEntryList& entries) {
        if (m_task == task)
            m_results->setEntries(entries);
    });

    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        setRunning(false);
        setStatusText(task->state() == Task::State::Failed ? task->error().message : task->statusText());
        m_task.clear();
    });

    setRunning(true);
    setTitle(
        m_queryText.isEmpty() ? QStringLiteral("Indexed search") : QStringLiteral("\"%1\"").arg(m_queryText));
    m_services.tasks->submit(task);
}

void IndexSearchController::scanDirectory(const QString& uri, const QString& label)
{
    if (!m_services.isValid())
        return;

    const VfsUri root = VfsUri::fromString(uri);
    FileSystemPtr fs = m_services.vfs->resolve(root);
    if (!fs) {
        setStatusText(QStringLiteral("No drive is mounted for %1").arg(uri));
        return;
    }

    auto* task = new ScanTask(std::move(fs), root, label.isEmpty() ? uri : label, m_services.index);

    // Announcing on the bus rather than calling back directly means every open
    // indexed-search tab refreshes, not just the one that started the scan.
    connect(task, &Task::finished, this, [this, task] {
        if (task->state() == Task::State::Succeeded && m_services.events)
            m_services.events->postIndexUpdated(-1, task->filesIndexed());
        setStatusText(task->statusText());
    });

    setStatusText(QStringLiteral("Scanning %1...").arg(uri));
    m_services.tasks->submit(task);
}

QVariantMap IndexSearchController::saveState() const
{
    return {
        { QStringLiteral("query"), m_queryText },
        { QStringLiteral("volumeIndex"), m_volumeIndex },
    };
}

void IndexSearchController::restoreState(const QVariantMap& state)
{
    setQueryText(state.value(QStringLiteral("query")).toString());
    // The volume list is rebuilt from the index, so the remembered position
    // may no longer exist; setVolumeIndex clamps.
    setVolumeIndex(state.value(QStringLiteral("volumeIndex"), 0).toInt());
}

void IndexSearchController::setStatusText(const QString& text)
{
    if (m_statusText == text)
        return;
    m_statusText = text;
    emit statusChanged();
}

void IndexSearchController::setRunning(bool running)
{
    if (m_running == running)
        return;
    m_running = running;
    setBusy(running);
    emit runningChanged();
}

// -------------------------------------------------------------------features

LiveSearchFeature::LiveSearchFeature(PluginServices services, QString defaultRoot)
    : m_services(services)
    , m_defaultRoot(std::move(defaultRoot))
{
}

QUrl LiveSearchFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/LiveSearchView.qml"));
}

FeatureController* LiveSearchFeature::createController(QObject* parent)
{
    return new LiveSearchController(m_services, m_defaultRoot, parent);
}

IndexSearchFeature::IndexSearchFeature(PluginServices services)
    : m_services(services)
{
}

QUrl IndexSearchFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/IndexSearchView.qml"));
}

FeatureController* IndexSearchFeature::createController(QObject* parent)
{
    return new IndexSearchController(m_services, parent);
}

} // namespace mole
