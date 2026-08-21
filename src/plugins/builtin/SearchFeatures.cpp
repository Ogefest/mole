#include "plugins/builtin/SearchFeatures.h"

#include "plugins/builtin/IndexScanJob.h"
#include "plugins/builtin/TimeWords.h"
#include "sdk/ScanReaders.h"
#include "ui/models/FileListModel.h"

#include "core/automation/ScheduleStore.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/IndexSearchTask.h"
#include "core/index/IndexSummary.h"
#include "core/index/ScanTask.h"
#include "core/sets/FileSetStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QIODevice>
#include <QLocale>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>

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
        // Not EventBus::indexUpdated: that is what starts the snapshot's own
        // refresh, so re-reading on it would read the state from before the
        // scan. The snapshot says when it has the new answer.
        if (m_services.indexSummary)
            connect(m_services.indexSummary, &IndexSummary::changed, this, [this] { refreshVolumes(); });

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
    emit coverageChanged();
    emit stateChanged();
}

void LiveSearchController::setQueryText(const QString& text)
{
    if (m_queryText == text)
        return;
    m_queryText = text;
    setTitle(text.isEmpty() ? QStringLiteral("Quick search") : QStringLiteral("\"%1\"").arg(text));
    rewriteQueryLine();
    emit queryTextChanged();
    emit stateChanged();
}

void LiveSearchController::setExtension(const QString& extension)
{
    if (m_extension == extension)
        return;
    m_extension = extension;
    rewriteQueryLine();
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
    // Scope is part of the query now, so choosing it in More has to appear on the
    // line -- otherwise the two would be saying different things, which is the one
    // thing this pair is built not to do. See ADR-0067.
    rewriteQueryLine();
    emit scopeChanged();
    emit coverageChanged();
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

    // From the snapshot: this is the interface thread, and everything below
    // that reads a volume is a property getter QML may evaluate at any time.
    // See ADR-0066.
    if (m_services.indexSummary) {
        for (const IndexVolume& volume : m_services.indexSummary->volumes()) {
            m_volumeLabels.append(QStringLiteral("%1 (%2 entries)").arg(volume.label).arg(volume.fileCount));
            m_volumeIds.append(volume.id);
        }
    }

    if (m_volumeIndex >= m_volumeLabels.size())
        setVolumeIndex(0);
    emit volumesChanged();
    emit coverageChanged();
}

namespace {
    /// What to call a folder in a list of rules: its own name, not the whole uri
    /// of a tree four levels down. The uri when there is no name to take, which
    /// is what the root of a drive looks like.
    QString folderNameOf(const QString& uri)
    {
        const QString path = VfsUri::fromString(uri).path();
        const QString name = path.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
        return name.isEmpty() ? uri : name;
    }
}

QString LiveSearchController::scheduleScan(const QString& uri, qint64 seconds)
{
    if (!m_services.isValid() || !m_services.scheduler->store())
        return {};

    // Incremental whatever the dialog's "full rescan" box says: that box is a
    // one-off for when somebody suspects the index, and a nightly full walk of
    // the tree this exists for is hours a night for nothing.
    ScanOptions nightly = scanOptions(true);
    // Read in Automation beside the report rules, where the whole uri of a tree
    // four levels down is a line nobody can tell from the one below it.
    return IndexScanJob::schedule(*m_services.scheduler->store(), uri, seconds, nightly,
        QStringLiteral("Re-index %1").arg(folderNameOf(uri)));
}

qint64 LiveSearchController::scheduledScanSeconds(const QString& uri) const
{
    if (!m_services.isValid() || !m_services.scheduler->store())
        return 0;
    const ScheduleRule rule = m_services.scheduler->store()->rule(scheduledScanId(uri));
    return rule.isValid() && rule.enabled ? rule.intervalSeconds : 0;
}

QVariantList LiveSearchController::schedulePresets() const
{
    QVariantList out;
    const auto presets = ScheduleRule::presets();
    for (const auto& preset : presets) {
        out.append(QVariantMap { { QStringLiteral("label"), preset.first },
            { QStringLiteral("seconds"), QVariant::fromValue(preset.second) } });
    }
    return out;
}

QString LiveSearchController::scheduledScanId(const QString& uri) const
{
    if (!m_services.isValid() || !m_services.scheduler->store())
        return {};
    return IndexScanJob::ruleFor(*m_services.scheduler->store(), uri).id;
}

void LiveSearchController::unscheduleScan(const QString& uri)
{
    if (!m_services.isValid() || !m_services.scheduler->store())
        return;
    if (const QString id = scheduledScanId(uri); !id.isEmpty()) {
        m_services.scheduler->store()->remove(id);
        m_services.scheduler->store()->save();
    }
}

ScanOptions LiveSearchController::scanOptions(bool incremental) const
{
    ScanOptions options;
    options.incremental = incremental;
    options.metadata = m_scanReadsMetadata;
    options.archives = m_scanOpensArchives;
    return options;
}

void LiveSearchController::scanDirectory(const QString& uri, const QString& label, bool full)
{
    if (!m_services.isValid())
        return;

    const VfsUri root = VfsUri::fromString(uri);
    FileSystemPtr fs = m_services.vfs->resolve(root);
    if (!fs) {
        setStatusText(QStringLiteral("No drive is mounted for %1").arg(uri));
        return;
    }

    auto* task = new ScanTask(fs, root, label.isEmpty() ? uri : label, m_services.index);
    applyScanOptions(*task, scanOptions(!full), m_services, fs, root);

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
    rewriteQueryLine();
    emit criteriaChanged();
}

std::optional<IndexVolume> LiveSearchController::coveringVolume() const
{
    // isKnown() rather than isOpen(): an open index nobody has read yet must not
    // answer "nothing covers this", because indexCoversRoot() and indexNote()
    // below turn that into a claim about the folder. See ADR-0066.
    if (!m_services.isValid() || !m_services.indexSummary || !m_services.indexSummary->isKnown())
        return std::nullopt;

    // The volume's root has to be a prefix of what is being searched: an index
    // that covers only part of the subtree covers none of it, because a list where
    // some rows are current and some are as old as the last scan is an answer
    // nobody can reason about. See ADR-0005.
    std::optional<IndexVolume> best;
    for (const IndexVolume& volume : m_services.indexSummary->volumes()) {
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
    return QStringLiteral("%1 is indexed, scanned %2").arg(volume->label, ageInWords(volume->lastScan));
}

QList<IndexVolume> LiveSearchController::volumesInsideRoot() const
{
    if (!m_services.isValid() || !m_services.indexSummary || !m_services.indexSummary->isKnown())
        return {};

    QList<IndexVolume> inside;
    for (const IndexVolume& volume : m_services.indexSummary->volumes()) {
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
        rewriteQueryLine();                                                                                  \
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
MOLE_SEARCH_SETTER(setScanReadsMetadata, m_scanReadsMetadata, bool)
MOLE_SEARCH_SETTER(setScanOpensArchives, m_scanOpensArchives, bool)

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

    for (auto it = m_factCriteria.cbegin(); it != m_factCriteria.cend(); ++it)
        query.addIfSet(SearchPredicate::metadataIs(it.key(), it.value().toString().trimmed()));

    query.excluded = splitList(m_excluded);
    query.maxDepth = m_maxDepth;
    return query;
}

SearchIo LiveSearchController::searchIoFor(const FileSystemPtr& fileSystem, const VfsUri& root) const
{
    if (!fileSystem)
        return {};

    SearchIo io;
    // Resolved per uri rather than captured once, because a hit can live inside
    // an archive that nobody has mounted -- and a member is a file like any
    // other once something can open the container.
    io.read = [this, fileSystem, root](const VfsUri& uri, qint64 offset, qint64 bytes) -> QByteArray {
        FileSystemPtr backend = uri.scheme() == root.scheme() ? fileSystem : backendFor(uri);
        if (!backend)
            return {};
        Result<std::unique_ptr<QIODevice>> stream = backend->openRead(uri, offset + bytes);
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

namespace {

    /// The vocabulary the line accepts, and the field each word moves.
    ///
    /// Written down once, in the order somebody meets them, because this list
    /// is what the completion offers and what an unknown key is measured
    /// against.
    const QList<QPair<QString, QString>>& queryVocabulary()
    {
        static const QList<QPair<QString, QString>> words {
            { QStringLiteral("name"), QStringLiteral("the file's name") },
            { QStringLiteral("ext"), QStringLiteral("one of a list of extensions") },
            { QStringLiteral("type"), QStringLiteral("image, video, audio, document, archive, code") },
            { QStringLiteral("size"), QStringLiteral("10M, 1.5 GiB") },
            { QStringLiteral("modified"), QStringLiteral("today, last 7 days, 2026-03-01") },
            { QStringLiteral("created"), QStringLiteral("the same, where the drive says") },
            { QStringLiteral("accessed"), QStringLiteral("the same, where the drive says") },
            { QStringLiteral("path"), QStringLiteral("anywhere in the folder path") },
            { QStringLiteral("content"), QStringLiteral("text inside the file") },
            { QStringLiteral("kind"), QStringLiteral("file or folder") },
            { QStringLiteral("hidden"), QStringLiteral("yes or no") },
            { QStringLiteral("depth"), QStringLiteral("0 for this folder alone") },
            { QStringLiteral("skip"), QStringLiteral("folders not to go into") },
            // Scope is part of the query rather than a picker beside it. Not a
            // bare `everywhere`, because a bare word is a name substring and
            // giving one word a second meaning would make it the one word
            // nobody can search for. See ADR-0067.
            { QStringLiteral("everywhere"), QStringLiteral("yes to search every indexed volume") },
        };
        return words;
    }

    /// How far apart two words are, for suggesting what somebody meant. A plain
    /// edit distance: the keys are short and the list is small.
    int distanceBetween(const QString& a, const QString& b)
    {
        QList<int> previous(b.size() + 1);
        QList<int> current(b.size() + 1);
        for (int j = 0; j <= b.size(); ++j)
            previous[j] = j;
        for (int i = 1; i <= a.size(); ++i) {
            current[0] = i;
            for (int j = 1; j <= b.size(); ++j) {
                const int cost = a.at(i - 1) == b.at(j - 1) ? 0 : 1;
                current[j] = qMin(qMin(current[j - 1] + 1, previous[j] + 1), previous[j - 1] + cost);
            }
            previous = current;
        }
        return previous[b.size()];
    }

} // namespace

QStringList LiveSearchController::queryKeys() const
{
    QStringList keys;
    for (const auto& [word, help] : queryVocabulary())
        keys.append(word);
    // The facts this scope actually records are keys like any other, which is
    // what makes the vocabulary one thing rather than two.
    keys += factKeys();
    return keys;
}

QStringList LiveSearchController::queryValuesFor(const QString& key) const
{
    if (key == QLatin1String("type"))
        return knownTypeClasses();
    if (key == QLatin1String("kind"))
        return { QStringLiteral("file"), QStringLiteral("folder") };
    if (key == QLatin1String("hidden") || key == QLatin1String("everywhere"))
        return { QStringLiteral("yes"), QStringLiteral("no") };
    return {};
}

void LiveSearchController::setQueryLine(const QString& text)
{
    if (m_queryLine == text)
        return;
    m_queryLine = text;
    m_queryLineError.clear();
    m_queryLineErrorAt = -1;

    const ParsedQueryLine parsed = parseQueryLine(text);
    if (!parsed.ok()) {
        m_queryLineError = parsed.errors.first().message;
        m_queryLineErrorAt = parsed.errors.first().position;
        emit queryLineChanged();
        return;
    }

    // Applied to the fields, which are the query. The guard is what stops the
    // rewrite below coming back round and overwriting what is being typed.
    m_rewriting = true;
    const QStringList known = queryKeys();
    QStringList names;
    QStringList content;
    QStringList paths;
    QVariantMap facts;
    m_extension.clear();
    m_typeClasses.clear();
    m_modifiedFrom.clear();
    m_modifiedTo.clear();
    m_createdFrom.clear();
    m_accessedFrom.clear();
    m_excluded.clear();
    m_minSize = -1;
    m_maxSize = -1;
    m_kindMode = 0;
    m_maxDepth = -1;
    m_nameMode = 0;
    m_excludeName = false;
    m_excludePath = false;
    // Absent means this folder, the way an absent ext: means no extension filter.
    // Anything else and the round trip would drift: the rewrite writes
    // everywhere:yes when the scope is set, so the line and the fields keep
    // saying the same thing. The volume is deliberately not reset -- it is not on
    // the line, so the picker is what sets it. See ADR-0067.
    bool everywhere = false;

    for (const QueryTerm& term : parsed.terms) {
        // A bare word is a name substring, which is what a bare word means
        // everywhere else -- and so is one containing a colon whose left half
        // nobody has heard of, so a file really called `notes:2026.txt` is
        // findable.
        if (term.key.isEmpty() || !known.contains(term.key)) {
            if (!term.key.isEmpty()) {
                // Close to a real key is a typo; far from every one is a name.
                QString nearest;
                int best = 3;
                for (const QString& candidate : known) {
                    const int distance = distanceBetween(term.key.toLower(), candidate.toLower());
                    if (distance < best) {
                        best = distance;
                        nearest = candidate;
                    }
                }
                if (!nearest.isEmpty()) {
                    m_queryLineError
                        = QStringLiteral("There is no %1. Did you mean %2?").arg(term.key, nearest);
                    m_queryLineErrorAt = term.position;
                    m_rewriting = false;
                    emit queryLineChanged();
                    return;
                }
                names.append(term.key + QLatin1Char(':') + term.value);
                continue;
            }
            names.append(term.value);
            if (term.isRegex)
                m_nameMode = 2;
            continue;
        }

        const QString key = term.key;
        const QString value = term.value;
        const auto asWhen = [&](QString& into) { into = value; };

        if (key == QLatin1String("name")) {
            names.append(value);
            m_nameMode = term.isRegex ? 2 : (value.contains(QLatin1Char('*')) ? 1 : 0);
            m_excludeName = term.negate;
        } else if (key == QLatin1String("ext")) {
            m_extension = value;
        } else if (key == QLatin1String("type")) {
            m_typeClasses = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
        } else if (key == QLatin1String("size")) {
            const qint64 bytes = parseSize(value);
            if (bytes < 0) {
                m_queryLineError = QStringLiteral("%1 is not a size").arg(value);
                m_queryLineErrorAt = term.position;
                m_rewriting = false;
                emit queryLineChanged();
                return;
            }
            if (term.op == QueryTerm::Op::Below || term.op == QueryTerm::Op::AtMost)
                m_maxSize = bytes;
            else
                m_minSize = bytes;
        } else if (key == QLatin1String("modified") || key == QLatin1String("created")
            || key == QLatin1String("accessed")) {
            if (!parseWhen(value, QDateTime::currentDateTime()).isValid()) {
                m_queryLineError = QStringLiteral("%1 is not a date anybody can read").arg(value);
                m_queryLineErrorAt = term.position;
                m_rewriting = false;
                emit queryLineChanged();
                return;
            }
            if (key == QLatin1String("created"))
                asWhen(m_createdFrom);
            else if (key == QLatin1String("accessed"))
                asWhen(m_accessedFrom);
            else if (term.op == QueryTerm::Op::Below || term.op == QueryTerm::Op::AtMost)
                asWhen(m_modifiedFrom); // "changed in the last 30 days"
            else
                asWhen(m_modifiedFrom);
        } else if (key == QLatin1String("path")) {
            paths.append(value);
            m_excludePath = term.negate;
        } else if (key == QLatin1String("content")) {
            content.append(value);
        } else if (key == QLatin1String("kind")) {
            m_kindMode = value.startsWith(QLatin1String("f"), Qt::CaseInsensitive)
                    && !value.startsWith(QLatin1String("fo"), Qt::CaseInsensitive)
                ? 1
                : 2;
        } else if (key == QLatin1String("hidden")) {
            m_includeHidden = !value.startsWith(QLatin1Char('n'), Qt::CaseInsensitive);
        } else if (key == QLatin1String("depth")) {
            m_maxDepth = value.toInt();
        } else if (key == QLatin1String("everywhere")) {
            // Read the way hidden: is, so there is one rule for a yes/no value
            // rather than two.
            everywhere = !value.startsWith(QLatin1Char('n'), Qt::CaseInsensitive);
        } else if (key == QLatin1String("skip")) {
            m_excluded = value;
        } else {
            facts.insert(key, value);
        }
    }

    m_queryText = names.join(QLatin1Char(' '));
    m_pathText = paths.join(QLatin1Char(' '));
    m_contentText = content.join(QLatin1Char(' '));
    m_factCriteria = facts;
    // Through the setter, so the subtitle and the coverage note follow it. The
    // rewrite it asks for is suppressed while m_rewriting is set, which is what
    // stops it overwriting what is being typed.
    setEverywhere(everywhere);
    m_rewriting = false;

    setTitle(
        m_queryText.isEmpty() ? QStringLiteral("Quick search") : QStringLiteral("\"%1\"").arg(m_queryText));
    emit queryTextChanged();
    emit criteriaChanged();
    emit queryLineChanged();
    emit stateChanged();
}

void LiveSearchController::rewriteQueryLine()
{
    if (m_rewriting)
        return;

    QList<QueryTerm> terms;
    const auto add = [&terms](const QString& key, const QString& value, QueryTerm::Op op = QueryTerm::Op::Is,
                         bool negate = false) {
        if (value.isEmpty())
            return;
        QueryTerm term;
        term.key = key;
        term.value = value;
        term.op = op;
        term.negate = negate;
        term.wasQuoted = value.contains(QLatin1Char(' '));
        terms.append(term);
    };

    if (!m_queryText.isEmpty()) {
        QueryTerm name;
        name.value = m_queryText;
        name.isRegex = m_nameMode == 2;
        name.negate = m_excludeName;
        name.wasQuoted = m_queryText.contains(QLatin1Char(' '));
        terms.append(name);
    }
    add(QStringLiteral("ext"), m_extension);
    add(QStringLiteral("type"), m_typeClasses.join(QLatin1Char(',')));
    if (m_minSize >= 0)
        add(QStringLiteral("size"), FileListModel::formatSize(m_minSize), QueryTerm::Op::AtLeast);
    if (m_maxSize >= 0)
        add(QStringLiteral("size"), FileListModel::formatSize(m_maxSize), QueryTerm::Op::AtMost);
    add(QStringLiteral("modified"), m_modifiedFrom);
    add(QStringLiteral("created"), m_createdFrom);
    add(QStringLiteral("accessed"), m_accessedFrom);
    add(QStringLiteral("path"), m_pathText, QueryTerm::Op::Is, m_excludePath);
    add(QStringLiteral("content"), m_contentText);
    if (m_kindMode == 1)
        add(QStringLiteral("kind"), QStringLiteral("file"));
    else if (m_kindMode == 2)
        add(QStringLiteral("kind"), QStringLiteral("folder"));
    if (!m_includeHidden)
        add(QStringLiteral("hidden"), QStringLiteral("no"));
    if (m_maxDepth >= 0)
        add(QStringLiteral("depth"), QString::number(m_maxDepth));
    add(QStringLiteral("skip"), m_excluded);
    if (m_everywhere)
        add(QStringLiteral("everywhere"), QStringLiteral("yes"));
    for (auto it = m_factCriteria.cbegin(); it != m_factCriteria.cend(); ++it)
        add(it.key(), it.value().toString());

    const QString written = printQueryLine(terms);
    if (written == m_queryLine)
        return;
    m_queryLine = written;
    m_queryLineError.clear();
    m_queryLineErrorAt = -1;
    emit queryLineChanged();
}

QStringList LiveSearchController::factKeys() const
{
    if (!m_services.isValid() || !m_services.indexSummary || !m_services.indexSummary->isKnown())
        return {};

    // This is the getter that made a snapshot the only workable answer: it used
    // to run two volumes() queries plus one factKeys() per volume in scope, and
    // coverageNote() calls it again. QML evaluates both whenever anything they
    // depend on changes. See ADR-0066.
    if (m_everywhere) {
        return m_services.indexSummary->factKeys(
            m_volumeIndex > 0 && m_volumeIndex < m_volumeIds.size() ? m_volumeIds.at(m_volumeIndex) : -1);
    }

    // Every volume that covers any of this folder, because a field is worth
    // offering when anything in scope can answer it.
    QList<IndexVolume> inScope = volumesInsideRoot();
    if (const std::optional<IndexVolume> whole = coveringVolume())
        inScope.append(*whole);

    QStringList keys;
    for (const IndexVolume& volume : inScope) {
        for (const QString& key : m_services.indexSummary->factKeys(volume.id)) {
            if (!keys.contains(key))
                keys.append(key);
        }
    }
    keys.sort();
    return keys;
}

QString LiveSearchController::coverageNote() const
{
    const bool withMetadata = !factKeys().isEmpty();

    if (m_everywhere) {
        return withMetadata ? QStringLiteral("every indexed volume, with what the files say about themselves")
                            : QStringLiteral("every indexed volume, names only");
    }

    if (const std::optional<IndexVolume> whole = coveringVolume()) {
        return QStringLiteral("indexed %1, %2")
            .arg(ageInWords(whole->lastScan),
                withMetadata ? QStringLiteral("with what the files say about themselves")
                             : QStringLiteral("names only"));
    }

    const QList<IndexVolume> partial = volumesInsideRoot();
    if (!partial.isEmpty()) {
        return QStringLiteral("part of this folder is indexed (%1), %2; the rest is walked")
            .arg(oldestScanNote(partial),
                withMetadata ? QStringLiteral("with what the files say about themselves")
                             : QStringLiteral("names only"));
    }

    // The plain case, said plainly: everything the walk can answer is still on
    // offer, and what it cannot is what the greyed section is about.
    return QStringLiteral("not indexed — names, sizes, dates and contents only");
}

void LiveSearchController::setFactCriteria(const QVariantMap& criteria)
{
    if (m_factCriteria == criteria)
        return;
    m_factCriteria = criteria;
    rewriteQueryLine();
    emit criteriaChanged();
    emit stateChanged();
}

void LiveSearchController::indexThisFolderForMetadata()
{
    setScanReadsMetadata(true);
    scanDirectory(m_rootUri, QString());
    setStatusText(
        QStringLiteral("Indexing %1 — the search can be run again when it finishes").arg(m_rootUri));
    m_blockedReason.clear();
    emit statusChanged();
}

void LiveSearchController::narrowToIndexedPart()
{
    const QList<IndexVolume> partial = volumesInsideRoot();
    if (partial.isEmpty())
        return;

    // Said, not done quietly: a search that shrinks its own scope without
    // saying so is the same fault as one that widens it.
    const QString was = m_rootUri;
    setRootUri(partial.first().rootUri);
    m_blockedReason.clear();
    emit coverageChanged();
    setStatusText(QStringLiteral("Now searching %1 only — everything else under %2 is left out")
                      .arg(partial.first().rootUri, was));
}

FileSystemPtr LiveSearchController::backendFor(const VfsUri& uri) const
{
    if (!m_services.vfs)
        return {};
    if (FileSystemPtr mounted = m_services.vfs->resolve(uri))
        return mounted;

    // Not mounted, and a container's own uri says which file it is. Built here
    // rather than added to the sidebar: a search reaching inside an archive is
    // not somebody asking for a new drive.
    for (IFileSystemFactory* factory : m_services.vfs->factories()) {
        if (factory->scheme() != uri.scheme())
            continue;
        const QString host = uri.authority();
        if (host.isEmpty())
            continue;
        QString error;
        // configForFile takes the path of the file being opened, which is what
        // the authority encodes; the factory is the only thing that knows how.
        if (FileSystemPtr built
            = factory->create(factory->configForFile(QUrl::fromPercentEncoding(host.toUtf8())), &error)) {
            return built;
        }
    }
    return {};
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

    // A line nobody could read does not run. Matching everything because of a
    // typo is how somebody spends ten minutes doubting their disk.
    if (!m_queryLineError.isEmpty()) {
        setStatusText(m_queryLineError);
        return;
    }

    // A criterion the scope cannot answer does not mean "everything": it means
    // the question could not be asked. ADR-0005's own rule, and this is where
    // it bites hardest -- so the search stops and offers the two ways out.
    m_blockedReason.clear();
    QStringList unanswerable;
    const QStringList answerable = factKeys();
    for (auto it = m_factCriteria.cbegin(); it != m_factCriteria.cend(); ++it) {
        if (it.value().toString().trimmed().isEmpty())
            continue;
        if (!answerable.contains(it.key()))
            unanswerable.append(it.key());
    }
    if (!unanswerable.isEmpty()) {
        unanswerable.sort();
        m_blockedReason = QStringLiteral("Nothing here has %1 recorded, so this cannot be searched for yet.")
                              .arg(unanswerable.join(QStringLiteral(", ")));
        setStatusText(m_blockedReason);
        emit statusChanged();
        return;
    }

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
        { QStringLiteral("factCriteria"), m_factCriteria },
        { QStringLiteral("queryLine"), m_queryLine },
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
    setFactCriteria(state.value(QStringLiteral("factCriteria")).toMap());
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
