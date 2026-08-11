#include "plugins/builtin/SearchFeatures.h"

#include "ui/models/FileListModel.h"

#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/IndexSearchTask.h"
#include "core/index/ScanTask.h"
#include "core/sets/FileSetStore.h"
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

    // A finished scan should make its results searchable at once, without the
    // user knowing there is a list to reload. Announced on the bus, so every
    // open search hears it rather than only the one that started the scan.
    if (m_services.events)
        connect(m_services.events, &EventBus::indexUpdated, this, [this] { refreshVolumes(); });

    refreshVolumes();
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

void LiveSearchController::setEverywhere(bool everywhere)
{
    if (m_everywhere == everywhere)
        return;
    m_everywhere = everywhere;
    // The subtitle is where the tab says what it is aimed at, and "everywhere
    // indexed" is as much an answer to that as a path is.
    setSubtitle(m_everywhere ? QStringLiteral("Everywhere indexed") : m_rootUri);
    emit scopeChanged();
    emit stateChanged();
}

void LiveSearchController::setVolumeIndex(int index)
{
    const int clamped = qBound(0, index, static_cast<int>(m_volumeLabels.size()) - 1);
    if (m_volumeIndex == clamped)
        return;
    m_volumeIndex = clamped;
    emit volumeIndexChanged();
    emit stateChanged();
}

void LiveSearchController::refreshVolumes()
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

void LiveSearchController::scanDirectory(const QString& uri, const QString& label)
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

    // Announced on the bus rather than called back directly, so every open
    // search refreshes and not only the one that asked for the scan.
    connect(task, &Task::finished, this, [this, task] {
        if (task->state() == Task::State::Succeeded && m_services.events)
            m_services.events->postIndexUpdated(-1, task->filesIndexed());
        setStatusText(task->statusText());
    });

    setStatusText(QStringLiteral("Scanning %1...").arg(uri));
    m_services.tasks->submit(task);
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

void LiveSearchController::startIndexSearch(const SearchQuery& query, const QString& doneFormat)
{
    auto* indexTask = new IndexSearchTask(m_services.index, query);
    m_indexTask = indexTask;

    connect(indexTask, &IndexSearchTask::resultsReady, this, [this, indexTask](const FileEntryList& hits) {
        if (m_indexTask == indexTask)
            m_results->setEntries(hits);
    });
    connect(indexTask, &Task::finished, this, [this, indexTask, doneFormat] {
        if (m_indexTask != indexTask)
            return;
        setRunning(false);
        setStatusText(indexTask->state() == Task::State::Failed
                ? indexTask->error().message
                : doneFormat.arg(QLocale().toString(m_results->totalCount())));
        m_indexTask.clear();
    });

    setRunning(true);
    setStatusText(QStringLiteral("Asking the index…"));
    m_services.tasks->submit(indexTask);
}

void LiveSearchController::start()
{
    if (!m_services.isValid())
        return;

    // Starting a new search abandons the old one rather than racing it.
    stop();

    // One query, whichever engine answers it. The criteria mean the same thing
    // to each of them because there is only one place that says what they mean.
    SearchQuery query;
    query.addIfSet(SearchPredicate::name(m_queryText, m_caseSensitive));
    query.addIfSet(SearchPredicate::extension(m_extension));
    query.addIfSet(SearchPredicate::minSize(m_minSize));
    query.addIfSet(SearchPredicate::maxSize(m_maxSize));

    // Everywhere indexed is a question only the index can answer -- a walk of
    // every volume anybody ever scanned is not something to wait for -- so the
    // scope decides the engine here rather than coverage doing it below.
    if (m_everywhere) {
        m_results->clear();
        m_truncated = false;
        query.volumeId
            = m_volumeIndex >= 0 && m_volumeIndex < m_volumeIds.size() ? m_volumeIds.at(m_volumeIndex) : -1;
        startIndexSearch(query, QStringLiteral("%1 from the index"));
        return;
    }

    const VfsUri root = VfsUri::fromString(m_rootUri);
    FileSystemPtr fs = m_services.vfs->resolve(root);
    if (!fs) {
        setStatusText(QStringLiteral("No drive is mounted for %1").arg(m_rootUri));
        return;
    }

    m_results->clear();
    m_truncated = false;

    // The folder the question was about. A walk is already inside it; the index
    // is not, because a volume can be a whole disk -- and rather than the answer
    // being narrowed by hand afterwards, the narrowing is part of the question.
    query.addIfSet(SearchPredicate::underPath(m_rootUri));

    // Which engine answers, and saying so. ADR-0005: the index when it covers the
    // whole subtree and has not been turned off, a walk otherwise.
    if (const std::optional<IndexVolume> volume = m_useIndex ? coveringVolume() : std::nullopt) {
        query.volumeId = volume->id;
        startIndexSearch(query, QStringLiteral("%1 from the index — ") + indexNote());
        return;
    }

    auto* task = new LiveSearchTask(std::move(fs), root, query);
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

QString LiveSearchController::buildSetFromResults(const QString& name)
{
    if (!m_services.isValid() || !m_services.sets)
        return {};

    // What is visible, filter and all: narrowing the results is how someone says
    // "these ones", so the set has to mean the same thing the screen does.
    QStringList uris;
    for (int row = 0; row < m_results->rowCount(); ++row)
        uris.append(m_results->data(m_results->index(row, 0), FileListModel::UriRole).toString());
    if (uris.isEmpty())
        return {};

    const QString chosen = name.trimmed().isEmpty()
        ? QStringLiteral("Search: %1").arg(m_queryText.isEmpty() ? m_rootUri : m_queryText)
        : name.trimmed();

    const FileSet built = m_services.sets->create(chosen, uris);
    m_services.sets->save();
    return built.id;
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
        { QStringLiteral("everywhere"), m_everywhere },
        { QStringLiteral("volumeIndex"), m_volumeIndex },
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

    // A tab of the retired indexed search, in a session written before the two
    // became one. It saved a volume and no root, which nothing else ever did,
    // and what it meant was this search asked of everywhere indexed.
    const bool wasTheIndexTab = root.isEmpty() && state.contains(QStringLiteral("volumeIndex"));
    setEverywhere(state.value(QStringLiteral("everywhere"), wasTheIndexTab).toBool());
    // The volume list is rebuilt from the index, so a remembered position may no
    // longer exist; setVolumeIndex clamps.
    setVolumeIndex(state.value(QStringLiteral("volumeIndex"), 0).toInt());
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

} // namespace mole
