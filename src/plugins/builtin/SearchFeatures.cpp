#include "plugins/builtin/SearchFeatures.h"

#include "ui/models/FileListModel.h"

#include "core/data/FileType.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/IndexSearchTask.h"
#include "core/index/ScanTask.h"
#include "core/sets/FileSetStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QIODevice>
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

namespace {

    /// How long ago, in words. The whole reason the index is safe to default to is
    /// that it admits its own age, so this is said rather than implied.
    QString ageInWords(const QDateTime& when)
    {
        const qint64 seconds = when.secsTo(QDateTime::currentDateTime());
        if (seconds < 120)
            return QStringLiteral("just now");
        if (seconds < 7200)
            return QStringLiteral("%1 minutes ago").arg(seconds / 60);
        if (seconds < 172800)
            return QStringLiteral("%1 hours ago").arg(seconds / 3600);
        return QStringLiteral("%1 days ago").arg(seconds / 86400);
    }

} // namespace

QString LiveSearchController::indexNote() const
{
    const std::optional<IndexVolume> volume = coveringVolume();
    if (!volume)
        return {};
    return QStringLiteral("%1 is indexed, scanned %2").arg(volume->label, ageInWords(volume->lastScan));
}

QList<IndexVolume> LiveSearchController::volumesInsideRoot() const
{
    if (!m_services.isValid() || !m_services.index || !m_services.index->isOpen())
        return {};

    Result<QList<IndexVolume>> volumes = m_services.index->volumes();
    if (!volumes.ok())
        return {};

    QList<IndexVolume> inside;
    for (const IndexVolume& volume : volumes.value()) {
        if (volume.fileCount <= 0 || !volume.lastScan.isValid())
            continue;
        // Inside the folder, not around it: a volume that contains the search
        // root covers the whole of it and is the other case entirely.
        if (volume.rootUri != m_rootUri && isUnder(volume.rootUri, m_rootUri))
            inside.append(volume);
    }
    return inside;
}

QString LiveSearchController::oldestScanNote(const QList<IndexVolume>& volumes)
{
    QDateTime oldest;
    for (const IndexVolume& volume : volumes) {
        if (!oldest.isValid() || volume.lastScan < oldest)
            oldest = volume.lastScan;
    }
    return oldest.isValid() ? QStringLiteral("scanned %1").arg(ageInWords(oldest)) : QString();
}

QString LiveSearchController::walkStatus(const QString& walkText, bool finished) const
{
    if (m_primedFromIndex <= 0)
        return walkText;

    const int remembered = m_results->fromIndexCount();
    if (!finished) {
        return QStringLiteral("%1 from the index, %2 · %3")
            .arg(QLocale().toString(remembered), m_primedNote, walkText);
    }
    if (remembered > 0) {
        // A directory the walk could not read leaves its rows as the scan left
        // them, and calling that current would be the lie this whole
        // arrangement exists to avoid.
        return QStringLiteral("%1 · %2 still only from the index, %3")
            .arg(walkText, QLocale().toString(remembered), m_primedNote);
    }
    return QStringLiteral("%1 · every row current").arg(walkText);
}

namespace {

    /// A comma or newline separated list, trimmed, with the blanks dropped.
    QStringList splitList(const QString& text)
    {
        QStringList out;
        for (const QString& part : text.split(QRegularExpression(QStringLiteral("[,;\n]")))) {
            const QString trimmed = part.trimmed();
            if (!trimmed.isEmpty())
                out.append(trimmed);
        }
        return out;
    }

} // namespace

#define MOLE_SEARCH_SETTER(Setter, Member, Type)                                                             \
    void LiveSearchController::Setter(Type value)                                                            \
    {                                                                                                        \
        if (Member == value)                                                                                 \
            return;                                                                                          \
        Member = value;                                                                                      \
        emit criteriaChanged();                                                                              \
        emit stateChanged();                                                                                 \
    }

// One shape, sixteen times. Written out by hand it would be two hundred lines
// of the same four, and a reader checking that one of them notifies would have
// to check all of them.
MOLE_SEARCH_SETTER(setNameMode, m_nameMode, int)
MOLE_SEARCH_SETTER(setWholeWord, m_wholeWord, bool)
MOLE_SEARCH_SETTER(setExcludeName, m_excludeName, bool)
MOLE_SEARCH_SETTER(setPathText, m_pathText, const QString&)
MOLE_SEARCH_SETTER(setExcludePath, m_excludePath, bool)
MOLE_SEARCH_SETTER(setTypeClasses, m_typeClasses, const QStringList&)
MOLE_SEARCH_SETTER(setModifiedFrom, m_modifiedFrom, const QString&)
MOLE_SEARCH_SETTER(setModifiedTo, m_modifiedTo, const QString&)
MOLE_SEARCH_SETTER(setCreatedFrom, m_createdFrom, const QString&)
MOLE_SEARCH_SETTER(setAccessedFrom, m_accessedFrom, const QString&)
MOLE_SEARCH_SETTER(setKindMode, m_kindMode, int)
MOLE_SEARCH_SETTER(setEmptyOnly, m_emptyOnly, bool)
MOLE_SEARCH_SETTER(setIncludeHidden, m_includeHidden, bool)
MOLE_SEARCH_SETTER(setMaxDepth, m_maxDepth, int)
MOLE_SEARCH_SETTER(setExcluded, m_excluded, const QString&)
MOLE_SEARCH_SETTER(setContentText, m_contentText, const QString&)
MOLE_SEARCH_SETTER(setContentRegex, m_contentRegex, bool)
MOLE_SEARCH_SETTER(setSearchBinary, m_searchBinary, bool)

#undef MOLE_SEARCH_SETTER

SearchQuery LiveSearchController::buildQuery() const
{
    const QDateTime now = QDateTime::currentDateTime();
    SearchQuery query;

    // The name, read the way the form was told to read it.
    SearchPredicate named = m_nameMode == 1 ? SearchPredicate::nameGlob(m_queryText, m_caseSensitive)
        : m_nameMode == 2                   ? SearchPredicate::nameRegex(m_queryText, m_caseSensitive)
                                            : SearchPredicate::name(m_queryText, m_caseSensitive);
    named.wholeWord = m_wholeWord;
    named.negate = m_excludeName;
    query.addIfSet(named);

    SearchPredicate inPath = SearchPredicate::pathContains(m_pathText, m_caseSensitive);
    inPath.negate = m_excludePath;
    query.addIfSet(inPath);

    query.addIfSet(SearchPredicate::extensions(splitList(m_extension)));
    query.addIfSet(SearchPredicate::typeClasses(m_typeClasses));
    query.addIfSet(SearchPredicate::minSize(m_minSize));
    query.addIfSet(SearchPredicate::maxSize(m_emptyOnly ? 0 : m_maxSize));

    // A date nobody could parse is not a date, so nothing is added for it --
    // narrowing by a criterion the user did not manage to state would be worse
    // than ignoring it, and matching everything would be worse still.
    const auto when = [&](const QString& text) { return parseWhen(text, now); };
    if (const QDateTime from = when(m_modifiedFrom); from.isValid())
        query.add(SearchPredicate::modifiedAfter(from.toSecsSinceEpoch()));
    if (const QDateTime to = when(m_modifiedTo); to.isValid())
        query.add(SearchPredicate::modifiedBefore(to.toSecsSinceEpoch()));
    if (const QDateTime from = when(m_createdFrom); from.isValid())
        query.add(SearchPredicate::createdAfter(from.toSecsSinceEpoch()));
    if (const QDateTime from = when(m_accessedFrom); from.isValid())
        query.add(SearchPredicate::accessedAfter(from.toSecsSinceEpoch()));

    if (m_kindMode == 1)
        query.add(SearchPredicate::kind(false));
    else if (m_kindMode == 2)
        query.add(SearchPredicate::kind(true));
    if (!m_includeHidden)
        query.add(SearchPredicate::hidden(false));

    // Last, because it is written last and the planner sorts by cost anyway --
    // but written last as well, so a reader of this function meets the criteria
    // in the order they are paid for.
    SearchPredicate inside = SearchPredicate::content(m_contentText, m_contentRegex, m_caseSensitive);
    inside.wholeWord = m_wholeWord;
    inside.includeBinary = m_searchBinary;
    query.addIfSet(inside);

    query.excluded = splitList(m_excluded);
    query.maxDepth = m_maxDepth;
    return query;
}

SearchIo LiveSearchController::searchIoFor(const FileSystemPtr& fileSystem, const VfsUri& root) const
{
    if (!fileSystem)
        return {};

    SearchIo io;
    io.read = [fileSystem](const VfsUri& uri, qint64 offset, qint64 bytes) -> QByteArray {
        Result<std::unique_ptr<QIODevice>> stream = fileSystem->openRead(uri, offset + bytes);
        if (!stream.ok() || !stream.value())
            return {};
        if (offset > 0 && !stream.value()->seek(offset) && stream.value()->read(offset).size() != offset) {
            return {};
        }
        return stream.value()->read(bytes);
    };
    // On anything but the local disk a read is a download, so a file has to be
    // worth much less before it is opened.
    io.ceiling = root.scheme() == QLatin1String("file") ? SearchIo::kLocalCeiling : SearchIo::kRemoteCeiling;
    return io;
}

void LiveSearchController::notePlan(const SearchQuery& query, SearchSource source)
{
    const SearchPlan plan = planSearch(query, source);
    if (source == SearchSource::Walk || plan.pushedDownEverything()) {
        m_unpushedNote.clear();
        return;
    }

    // Named rather than counted: "3 criteria" tells nobody which of the things
    // they typed made the search slower. ADR-0005 asked for this out loud.
    static const QHash<int, QString> names {
        { int(SearchPredicate::Field::Name), QStringLiteral("the name") },
        { int(SearchPredicate::Field::Path), QStringLiteral("the path") },
        { int(SearchPredicate::Field::Extension), QStringLiteral("the extension") },
        { int(SearchPredicate::Field::Size), QStringLiteral("the size") },
        { int(SearchPredicate::Field::Modified), QStringLiteral("the date changed") },
        { int(SearchPredicate::Field::Created), QStringLiteral("the date made") },
        { int(SearchPredicate::Field::Accessed), QStringLiteral("the date read") },
        { int(SearchPredicate::Field::Kind), QStringLiteral("files or folders") },
        { int(SearchPredicate::Field::Hidden), QStringLiteral("hidden files") },
        { int(SearchPredicate::Field::TypeClass), QStringLiteral("what the file is") },
    };

    QStringList said;
    for (const SearchPredicate& predicate : plan.remainder()) {
        if (predicate.field == SearchPredicate::Field::Under)
            continue; // the folder itself, which nobody typed
        const QString name = names.value(int(predicate.field));
        if (!name.isEmpty() && !said.contains(name))
            said.append(name);
    }
    m_unpushedNote = said.isEmpty()
        ? QString()
        : QStringLiteral("the index cannot answer %1, so it was checked afterwards")
              .arg(said.join(QStringLiteral(", ")));
}

void LiveSearchController::startIndexSearch(const SearchQuery& query, const QString& doneFormat)
{
    auto* indexTask = new IndexSearchTask(m_services.index, query);
    // What a row cannot say about a file, read from the file. Only reached by
    // hits that survived everything the database could state.
    if (planSearch(query, SearchSource::Index).needsFile() && m_services.vfs) {
        const VfsUri root = VfsUri::fromString(m_rootUri);
        indexTask->setSearchIo(searchIoFor(m_services.vfs->resolve(root), root));
    }
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
    SearchQuery query = buildQuery();

    // Everywhere indexed is a question only the index can answer -- a walk of
    // every volume anybody ever scanned is not something to wait for -- so the
    // scope decides the engine here rather than coverage doing it below.
    if (m_everywhere) {
        m_results->clear();
        m_truncated = false;
        query.volumeId
            = m_volumeIndex >= 0 && m_volumeIndex < m_volumeIds.size() ? m_volumeIds.at(m_volumeIndex) : -1;
        notePlan(query, SearchSource::Index);
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
    m_primedFromIndex = 0;
    m_primedNote.clear();

    // The folder the question was about. A walk is already inside it; the index
    // is not, because a volume can be a whole disk -- and rather than the answer
    // being narrowed by hand afterwards, the narrowing is part of the question.
    query.addIfSet(SearchPredicate::underPath(m_rootUri));

    // Which engine answers, and saying so. ADR-0005: the index when it covers the
    // whole subtree and has not been turned off, a walk otherwise.
    if (const std::optional<IndexVolume> volume = m_useIndex ? coveringVolume() : std::nullopt) {
        query.volumeId = volume->id;
        notePlan(query, SearchSource::Index);
        startIndexSearch(query, QStringLiteral("%1 from the index — ") + indexNote());
        return;
    }

    // Partly covered: volumes sitting inside the folder cover some of it and
    // none of it whole. The index answers for what it has -- instantly, and
    // marked as a memory -- and the walk goes over the lot and corrects it, so
    // the list converges on the truth while somebody is reading it. See
    // ADR-0038; ADR-0005 used to call this no coverage at all.
    const QList<IndexVolume> partial = m_useIndex ? volumesInsideRoot() : QList<IndexVolume> {};
    if (partial.isEmpty()) {
        notePlan(query, SearchSource::Walk);
        startWalk(std::move(fs), root, query, {});
        return;
    }
    notePlan(query, SearchSource::Index);

    SearchQuery primingQuery = query;
    primingQuery.volumeId = -1; // any volume, narrowed to the folder by the path

    auto* indexTask = new IndexSearchTask(m_services.index, primingQuery);
    m_indexTask = indexTask;

    connect(indexTask, &IndexSearchTask::resultsReady, this,
        [this, indexTask, partial](const FileEntryList& hits) {
            if (m_indexTask != indexTask)
                return;
            m_results->setEntries(hits);
            // Per volume, because two volumes under one folder were scanned at
            // different times and a row may not claim its neighbour's age.
            for (const IndexVolume& volume : partial) {
                QStringList mine;
                for (const FileEntry& entry : hits) {
                    if (isUnder(entry.uri.toString(), volume.rootUri))
                        mine.append(entry.uri.toString());
                }
                m_results->markFromIndex(mine, volume.lastScan);
            }
            m_primedFromIndex = static_cast<int>(hits.size());
            m_primedNote = oldestScanNote(partial);
            // Said now rather than when the walk gets round to its first status:
            // between those two moments the list is on screen, and a line still
            // reading "asking the index" is a line describing the past.
            setStatusText(walkStatus(QStringLiteral("walking the rest"), false));
        });

    connect(indexTask, &Task::finished, this, [this, indexTask, fs, root, query, partial]() mutable {
        if (m_indexTask != indexTask)
            return;
        m_indexTask.clear();

        // What the index put on screen, grouped by the directory it is in,
        // so the walk can tell each of those directories what is missing.
        QHash<QString, QStringList> primed;
        for (int row = 0; row < m_results->rowCount(); ++row) {
            const QModelIndex at = m_results->index(row, 0);
            primed[at.data(FileListModel::ParentUriRole).toString()].append(
                at.data(FileListModel::UriRole).toString());
        }

        startWalk(std::move(fs), root, query, primed);
    });

    setRunning(true);
    setStatusText(QStringLiteral("Asking the index…"));
    m_services.tasks->submit(indexTask);
}

void LiveSearchController::startWalk(FileSystemPtr fileSystem, const VfsUri& root, const SearchQuery& query,
    const QHash<QString, QStringList>& primed)
{
    auto* task = new LiveSearchTask(fileSystem, root, query);
    task->supersede(primed);
    task->setContentCeiling(
        root.scheme() == QLatin1String("file") ? SearchIo::kLocalCeiling : SearchIo::kRemoteCeiling);
    m_task = task;

    connect(task, &LiveSearchTask::hitsFound, this,
        [this, task](const FileEntryList& batch, const QList<ContentMatch>& why) {
            if (m_task != task)
                return;
            // Merged rather than appended: a row the index already put here is
            // replaced where it sits, so nothing is listed twice and the marking
            // on it stops saying "remembered".
            m_results->mergeEntries(batch);
            for (qsizetype i = 0; i < why.size() && i < batch.size(); ++i) {
                if (why.at(i).isValid())
                    m_results->setContentMatch(batch.at(i).uri.toString(), why.at(i));
            }
        });

    connect(task, &LiveSearchTask::hitsGone, this, [this, task](const QStringList& uris) {
        if (m_task == task)
            m_results->removeEntries(uris);
    });

    connect(task, &Task::statusTextChanged, this, [this, task] {
        if (m_task == task)
            setStatusText(walkStatus(task->statusText(), false));
    });

    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_truncated = task->truncated();
        setRunning(false);

        if (task->state() == Task::State::Failed) {
            setStatusText(task->error().message);
        } else if (task->state() == Task::State::Cancelled) {
            // Cancelled from the task strip rather than from the form, which is
            // the one route that does not come through stop().
            setStatusText(walkStatus(QStringLiteral("stopped — %1").arg(task->statusText()), false));
        } else {
            setStatusText(walkStatus(task->statusText(), true));
        }
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
    // The index task's own completion is what starts the walk, so a stopped
    // search has to forget it rather than let it finish and carry on.
    if (m_indexTask) {
        m_indexTask->requestCancel();
        m_indexTask.clear();
    }
    if (m_task) {
        m_task->requestCancel();
        m_task.clear();
        // Said here rather than when the pool thread notices: the user pressed
        // Stop, and what is on the line until then reads like a search still
        // running. What was found stays on screen; only the line changes.
        setStatusText(walkStatus(
            QStringLiteral("stopped — %1 found").arg(QLocale().toString(m_results->totalCount())), false));
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
        { QStringLiteral("nameMode"), m_nameMode },
        { QStringLiteral("wholeWord"), m_wholeWord },
        { QStringLiteral("excludeName"), m_excludeName },
        { QStringLiteral("pathText"), m_pathText },
        { QStringLiteral("excludePath"), m_excludePath },
        { QStringLiteral("typeClasses"), m_typeClasses },
        { QStringLiteral("modifiedFrom"), m_modifiedFrom },
        { QStringLiteral("modifiedTo"), m_modifiedTo },
        { QStringLiteral("createdFrom"), m_createdFrom },
        { QStringLiteral("accessedFrom"), m_accessedFrom },
        { QStringLiteral("kindMode"), m_kindMode },
        { QStringLiteral("emptyOnly"), m_emptyOnly },
        { QStringLiteral("includeHidden"), m_includeHidden },
        { QStringLiteral("maxDepth"), m_maxDepth },
        { QStringLiteral("excluded"), m_excluded },
        { QStringLiteral("contentText"), m_contentText },
        { QStringLiteral("contentRegex"), m_contentRegex },
        { QStringLiteral("searchBinary"), m_searchBinary },
        { QStringLiteral("minSize"), m_minSize },
        { QStringLiteral("maxSize"), m_maxSize },
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

    // Every criterion, because a query somebody built out of eight fields is
    // not something to make them build again after a restart.
    setNameMode(state.value(QStringLiteral("nameMode"), 0).toInt());
    setWholeWord(state.value(QStringLiteral("wholeWord"), false).toBool());
    setExcludeName(state.value(QStringLiteral("excludeName"), false).toBool());
    setPathText(state.value(QStringLiteral("pathText")).toString());
    setExcludePath(state.value(QStringLiteral("excludePath"), false).toBool());
    setTypeClasses(state.value(QStringLiteral("typeClasses")).toStringList());
    setModifiedFrom(state.value(QStringLiteral("modifiedFrom")).toString());
    setModifiedTo(state.value(QStringLiteral("modifiedTo")).toString());
    setCreatedFrom(state.value(QStringLiteral("createdFrom")).toString());
    setAccessedFrom(state.value(QStringLiteral("accessedFrom")).toString());
    setKindMode(state.value(QStringLiteral("kindMode"), 0).toInt());
    setEmptyOnly(state.value(QStringLiteral("emptyOnly"), false).toBool());
    setIncludeHidden(state.value(QStringLiteral("includeHidden"), true).toBool());
    setMaxDepth(state.value(QStringLiteral("maxDepth"), -1).toInt());
    setExcluded(state.value(QStringLiteral("excluded")).toString());
    setContentText(state.value(QStringLiteral("contentText")).toString());
    setContentRegex(state.value(QStringLiteral("contentRegex"), false).toBool());
    setSearchBinary(state.value(QStringLiteral("searchBinary"), false).toBool());
    m_minSize = state.value(QStringLiteral("minSize"), -1).toLongLong();
    m_maxSize = state.value(QStringLiteral("maxSize"), -1).toLongLong();
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
