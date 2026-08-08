#include "plugins/builtin/SearchFeatures.h"

#include "ui/models/FileListModel.h"

#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/IndexSearchTask.h"
#include "core/index/ScanTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QLocale>
#include <QRegularExpression>

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

void LiveSearchController::setUseIndex(bool use)
{
    if (m_useIndex == use)
        return;
    m_useIndex = use;
    emit criteriaChanged();
}

qint64 LiveSearchController::parseSize(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return -1;

    // A number, optional space, optional unit. Nobody should have to count zeros
    // to say "bigger than ten megabytes".
    static const QRegularExpression pattern(QStringLiteral("^([0-9]+(?:[.,][0-9]+)?)\\s*([kmgt]?)(?:i?b)?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(trimmed);
    if (!match.hasMatch())
        return -1;

    QString number = match.captured(1);
    number.replace(QLatin1Char(','), QLatin1Char('.'));
    bool ok = false;
    const double value = number.toDouble(&ok);
    if (!ok || value < 0)
        return -1;

    // Powers of 1024, which is what a file manager showing "GiB" everywhere else
    // has to mean by "G".
    const QString unit = match.captured(2).toLower();
    double multiplier = 1;
    if (unit == QLatin1String("k"))
        multiplier = 1024.0;
    else if (unit == QLatin1String("m"))
        multiplier = 1024.0 * 1024;
    else if (unit == QLatin1String("g"))
        multiplier = 1024.0 * 1024 * 1024;
    else if (unit == QLatin1String("t"))
        multiplier = 1024.0 * 1024 * 1024 * 1024;

    return static_cast<qint64>(value * multiplier);
}

void LiveSearchController::setSizeRange(const QString& minText, const QString& maxText)
{
    const qint64 low = parseSize(minText);
    const qint64 high = parseSize(maxText);
    if (low == m_minSize && high == m_maxSize)
        return;
    m_minSize = low;
    m_maxSize = high;
    emit criteriaChanged();
}

std::optional<IndexVolume> LiveSearchController::coveringVolume() const
{
    if (!m_services.isValid() || !m_services.index || !m_services.index->isOpen())
        return std::nullopt;

    Result<QList<IndexVolume>> volumes = m_services.index->volumes();
    if (!volumes.ok())
        return std::nullopt;

    // The volume's root has to be a prefix of what is being searched: an index
    // that covers only part of the subtree covers none of it, because a list where
    // some rows are current and some are as old as the last scan is an answer
    // nobody can reason about. See ADR-0005.
    std::optional<IndexVolume> best;
    for (const IndexVolume& volume : volumes.value()) {
        if (volume.fileCount <= 0 || !volume.lastScan.isValid())
            continue;
        if (!m_rootUri.startsWith(volume.rootUri))
            continue;
        // The deepest match, so a nested volume wins over the disk it sits on.
        if (!best || volume.rootUri.size() > best->rootUri.size())
            best = volume;
    }
    return best;
}

bool LiveSearchController::indexCoversRoot() const
{
    return coveringVolume().has_value();
}

QString LiveSearchController::indexNote() const
{
    const std::optional<IndexVolume> volume = coveringVolume();
    if (!volume)
        return {};

    // How stale it might be, in words: the whole reason the index is safe to
    // default to is that it admits its own age.
    const qint64 seconds = volume->lastScan.secsTo(QDateTime::currentDateTime());
    QString age;
    if (seconds < 120)
        age = QStringLiteral("just now");
    else if (seconds < 7200)
        age = QStringLiteral("%1 minutes ago").arg(seconds / 60);
    else if (seconds < 172800)
        age = QStringLiteral("%1 hours ago").arg(seconds / 3600);
    else
        age = QStringLiteral("%1 days ago").arg(seconds / 86400);

    return QStringLiteral("%1 is indexed, scanned %2").arg(volume->label, age);
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

    // Which engine answers, and saying so. ADR-0005: the index when it covers the
    // whole subtree and has not been turned off, a walk otherwise.
    if (const std::optional<IndexVolume> volume = m_useIndex ? coveringVolume() : std::nullopt) {
        IndexSearchQuery query;
        query.text = m_queryText;
        query.extension = m_extension;
        query.caseSensitive = m_caseSensitive;
        query.volumeId = volume->id;
        query.minSize = m_minSize;
        query.maxSize = m_maxSize;

        auto* indexTask = new IndexSearchTask(m_services.index, query);
        m_indexTask = indexTask;
        connect(
            indexTask, &IndexSearchTask::resultsReady, this, [this, indexTask](const FileEntryList& hits) {
                if (m_indexTask != indexTask)
                    return;
                // Only what is under the folder being searched: the volume can be a
                // whole disk and the question was about one folder in it.
                FileEntryList inScope;
                for (const FileEntry& entry : hits) {
                    if (entry.uri.toString().startsWith(m_rootUri))
                        inScope.append(entry);
                }
                m_results->setEntries(inScope);
            });
        connect(indexTask, &Task::finished, this, [this, indexTask] {
            if (m_indexTask != indexTask)
                return;
            setRunning(false);
            setStatusText(indexTask->state() == Task::State::Failed
                    ? indexTask->error().message
                    : QStringLiteral("%1 from the index — %2")
                          .arg(QLocale().toString(m_results->totalCount()), indexNote()));
            m_indexTask.clear();
        });

        setRunning(true);
        setStatusText(QStringLiteral("Asking the index…"));
        m_services.tasks->submit(indexTask);
        return;
    }

    LiveSearchTask::Criteria criteria;
    criteria.text = m_queryText;
    criteria.extension = m_extension;
    criteria.caseSensitive = m_caseSensitive;
    criteria.minSize = m_minSize;
    criteria.maxSize = m_maxSize;

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
    if (m_indexTask) {
        m_indexTask->requestCancel();
        m_indexTask.clear();
    }
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
