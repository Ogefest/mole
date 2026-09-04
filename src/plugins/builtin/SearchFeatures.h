#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "ui/models/FileListModel.h"

#include "core/index/IndexDatabase.h"
#include "core/index/ScanOptions.h"
#include "core/search/LiveSearchTask.h"
#include "core/search/QueryLine.h"
#include "core/search/SearchQuery.h"

#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace mole {

class IndexSearchTask;

/// The search.
///
/// One tab, one form, one idea of what a search is. Which engine answers it is a
/// fact about the question -- *what is there now* against *what did we last see*
/// -- rather than a choice of tool: there used to be a second tab for the index,
/// and the only thing it really offered that this one could not was a scope of
/// *everywhere I have scanned*, which is a field here.
class LiveSearchController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(mole::FileListModel* results READ results CONSTANT)
    Q_PROPERTY(QString rootUri READ rootUri WRITE setRootUri NOTIFY rootUriChanged)
    /// Search every indexed volume instead of one folder. What the retired
    /// second tab meant, and the only question it could ask that this one could
    /// not: a walk of "everywhere" is not a thing anybody can wait for.
    Q_PROPERTY(bool everywhere READ everywhere WRITE setEverywhere NOTIFY scopeChanged)
    /// "All volumes" and then each scanned root with how much is in it. Only
    /// meaningful while `everywhere` is set.
    Q_PROPERTY(QStringList volumeLabels READ volumeLabels NOTIFY volumesChanged)
    Q_PROPERTY(int volumeIndex READ volumeIndex WRITE setVolumeIndex NOTIFY volumeIndexChanged)
    Q_PROPERTY(QString queryText READ queryText WRITE setQueryText NOTIFY queryTextChanged)
    /// The line, and what is wrong with it.
    ///
    /// Declared as properties late, which is the whole of a fault worth writing
    /// down: the line's widget bound to `controller.queryLine` and assigned to it
    /// on every keystroke, and with no Q_PROPERTY behind the name both did
    /// nothing at all -- the read gave undefined and the write went nowhere. So
    /// the box existed, was tested through setQueryLine() in C++, and could not
    /// be typed into. Found by making it the box the tab opens with. See
    /// ADR-0067.
    Q_PROPERTY(QString queryLine READ queryLine WRITE setQueryLine NOTIFY queryLineChanged)
    Q_PROPERTY(QString queryLineError READ queryLineError NOTIFY queryLineChanged)
    Q_PROPERTY(int queryLineErrorAt READ queryLineErrorAt NOTIFY queryLineChanged)
    /// A comma-separated list, because a search for photographs means jpg and
    /// jpeg and heic and is one question.
    Q_PROPERTY(QString extension READ extension WRITE setExtension NOTIFY criteriaChanged)
    Q_PROPERTY(bool caseSensitive READ caseSensitive WRITE setCaseSensitive NOTIFY criteriaChanged)
    /// How the name is read: 0 a substring, 1 a glob, 2 an expression. Chosen
    /// rather than guessed at from the text -- guessing turns a file called
    /// `a.b` into a pattern.
    Q_PROPERTY(int nameMode READ nameMode WRITE setNameMode NOTIFY criteriaChanged)
    Q_PROPERTY(bool wholeWord READ wholeWord WRITE setWholeWord NOTIFY criteriaChanged)
    Q_PROPERTY(bool excludeName READ excludeName WRITE setExcludeName NOTIFY criteriaChanged)
    /// Anywhere in the uri. Different from the name, and what does the work
    /// when somebody remembers the folder and not the file.
    Q_PROPERTY(QString pathText READ pathText WRITE setPathText NOTIFY criteriaChanged)
    Q_PROPERTY(bool excludePath READ excludePath WRITE setExcludePath NOTIFY criteriaChanged)
    /// Any of "image", "video", "audio", "document", "archive", "code",
    /// "folder" -- from what is in the file rather than what it is called.
    Q_PROPERTY(QStringList typeClasses READ typeClasses WRITE setTypeClasses NOTIFY criteriaChanged)
    Q_PROPERTY(QStringList allTypeClasses READ allTypeClasses CONSTANT)
    /// Typed the way people say it: `today`, `last 7 days`, `>30d`, `2026-08-01`.
    Q_PROPERTY(QString modifiedFrom READ modifiedFrom WRITE setModifiedFrom NOTIFY criteriaChanged)
    Q_PROPERTY(QString modifiedTo READ modifiedTo WRITE setModifiedTo NOTIFY criteriaChanged)
    Q_PROPERTY(QString createdFrom READ createdFrom WRITE setCreatedFrom NOTIFY criteriaChanged)
    Q_PROPERTY(QString accessedFrom READ accessedFrom WRITE setAccessedFrom NOTIFY criteriaChanged)
    /// 0 both, 1 files only, 2 folders only.
    Q_PROPERTY(int kindMode READ kindMode WRITE setKindMode NOTIFY criteriaChanged)
    Q_PROPERTY(bool emptyOnly READ emptyOnly WRITE setEmptyOnly NOTIFY criteriaChanged)
    Q_PROPERTY(bool includeHidden READ includeHidden WRITE setIncludeHidden NOTIFY criteriaChanged)
    /// -1 for the whole tree, 0 for this folder alone.
    Q_PROPERTY(int maxDepth READ maxDepth WRITE setMaxDepth NOTIFY criteriaChanged)
    /// Directory globs the walk must not enter: `node_modules, .git, build`.
    Q_PROPERTY(QString excluded READ excluded WRITE setExcluded NOTIFY criteriaChanged)
    /// What the last search could not ask its source, in words, for the form to
    /// show beside the criteria. Empty when everything was pushed down.
    Q_PROPERTY(QString unpushedNote READ unpushedNote NOTIFY statusChanged)
    /// Text to look for inside the files. The dearest criterion there is, so it
    /// runs last and only over what everything else has kept.
    Q_PROPERTY(QString contentText READ contentText WRITE setContentText NOTIFY criteriaChanged)
    Q_PROPERTY(bool contentRegex READ contentRegex WRITE setContentRegex NOTIFY criteriaChanged)
    Q_PROPERTY(bool searchBinary READ searchBinary WRITE setSearchBinary NOTIFY criteriaChanged)
    /// Whether a scan also records what each file says about itself. Off by
    /// default: the cost is bounded per file and unbounded in aggregate, so it
    /// is a choice made per scan with the number of files in front of it.
    Q_PROPERTY(
        bool scanReadsMetadata READ scanReadsMetadata WRITE setScanReadsMetadata NOTIFY criteriaChanged)
    /// True while the search is one that opens files, so the form can say what
    /// it is about to cost before it starts rather than after.
    Q_PROPERTY(bool readsFileContents READ readsFileContents NOTIFY criteriaChanged)
    /// Bytes; -1 for "no limit". Set through setSizeRange() from the form, which
    /// takes what a person types.
    Q_PROPERTY(qint64 minSize READ minSize NOTIFY criteriaChanged)
    Q_PROPERTY(qint64 maxSize READ maxSize NOTIFY criteriaChanged)
    /// Answer from the index when it covers this folder. On by default: the index
    /// is enormously faster and, for a folder that was indexed, usually right.
    /// See docs/adr/0005-which-engine-answers-a-search.md.
    Q_PROPERTY(bool useIndex READ useIndex WRITE setUseIndex NOTIFY criteriaChanged)
    /// True when an indexed volume's root is a prefix of this folder, so the index
    /// covers the whole subtree. Partial coverage counts as none.
    Q_PROPERTY(bool indexCoversRoot READ indexCoversRoot NOTIFY rootUriChanged)
    /// "last scanned 2 hours ago", for the form to show beside the toggle. Empty
    /// when nothing covers this folder.
    Q_PROPERTY(QString indexNote READ indexNote NOTIFY rootUriChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(bool truncated READ isTruncated NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)

public:
    LiveSearchController(PluginServices services, QString rootUri, QObject* parent = nullptr);
    ~LiveSearchController() override;

    FileListModel* results() const { return m_results; }
    QString rootUri() const { return m_rootUri; }
    void setRootUri(const QString& uri);
    QString queryText() const { return m_queryText; }
    void setQueryText(const QString& text);
    QString extension() const { return m_extension; }
    void setExtension(const QString& extension);
    bool caseSensitive() const { return m_caseSensitive; }
    void setCaseSensitive(bool sensitive);
    bool isRunning() const { return m_running; }
    bool isTruncated() const { return m_truncated; }
    QString statusText() const { return m_statusText; }

    int nameMode() const { return m_nameMode; }
    void setNameMode(int mode);
    bool wholeWord() const { return m_wholeWord; }
    void setWholeWord(bool whole);
    bool excludeName() const { return m_excludeName; }
    void setExcludeName(bool exclude);
    QString pathText() const { return m_pathText; }
    void setPathText(const QString& text);
    bool excludePath() const { return m_excludePath; }
    void setExcludePath(bool exclude);
    QStringList typeClasses() const { return m_typeClasses; }
    void setTypeClasses(const QStringList& classes);
    static QStringList allTypeClasses() { return knownTypeClasses(); }
    QString modifiedFrom() const { return m_modifiedFrom; }
    void setModifiedFrom(const QString& text);
    QString modifiedTo() const { return m_modifiedTo; }
    void setModifiedTo(const QString& text);
    QString createdFrom() const { return m_createdFrom; }
    void setCreatedFrom(const QString& text);
    QString accessedFrom() const { return m_accessedFrom; }
    void setAccessedFrom(const QString& text);
    int kindMode() const { return m_kindMode; }
    void setKindMode(int mode);
    bool emptyOnly() const { return m_emptyOnly; }
    void setEmptyOnly(bool empty);
    bool includeHidden() const { return m_includeHidden; }
    void setIncludeHidden(bool include);
    int maxDepth() const { return m_maxDepth; }
    void setMaxDepth(int depth);
    QString excluded() const { return m_excluded; }
    void setExcluded(const QString& text);
    QString unpushedNote() const { return m_unpushedNote; }
    QString contentText() const { return m_contentText; }
    void setContentText(const QString& text);
    bool contentRegex() const { return m_contentRegex; }
    void setContentRegex(bool asRegex);
    bool searchBinary() const { return m_searchBinary; }
    void setSearchBinary(bool include);
    bool readsFileContents() const { return !m_contentText.isEmpty(); }
    bool scanReadsMetadata() const { return m_scanReadsMetadata; }
    void setScanReadsMetadata(bool read);
    bool scanOpensArchives() const { return m_scanOpensArchives; }
    void setScanOpensArchives(bool open);

    QString queryLine() const { return m_queryLine; }
    void setQueryLine(const QString& text);
    QString queryLineError() const { return m_queryLineError; }
    int queryLineErrorAt() const { return m_queryLineErrorAt; }
    QStringList queryKeys() const;
    /// The values worth completing for `key`, where the set is small and known:
    /// the type classes, the cameras actually in the index. Empty otherwise,
    /// which is most keys.
    Q_INVOKABLE QStringList queryValuesFor(const QString& key) const;

    QString coverageNote() const;
    QStringList factKeys() const;
    bool metadataAvailable() const { return !factKeys().isEmpty(); }
    QVariantMap factCriteria() const { return m_factCriteria; }
    void setFactCriteria(const QVariantMap& criteria);
    QString blockedReason() const { return m_blockedReason; }
    bool blocked() const { return !m_blockedReason.isEmpty(); }
    bool hasIndexedPart() const { return coveringVolume().has_value() || !volumesInsideRoot().isEmpty(); }

    /// The two ways out of a blocked search, both one click.
    ///
    /// Indexing this folder with metadata on, or narrowing the search to the
    /// part that is already indexed -- which says what it is leaving out,
    /// because a search that quietly shrinks its own scope is the same fault as
    /// one that quietly widens it.
    Q_INVOKABLE void indexThisFolderForMetadata();
    Q_INVOKABLE void narrowToIndexedPart();

    qint64 minSize() const { return m_minSize; }
    qint64 maxSize() const { return m_maxSize; }
    bool useIndex() const { return m_useIndex; }
    void setUseIndex(bool use);
    bool indexCoversRoot() const;
    QString indexNote() const;

    bool everywhere() const { return m_everywhere; }
    void setEverywhere(bool everywhere);
    QStringList volumeLabels() const { return m_volumeLabels; }
    int volumeIndex() const { return m_volumeIndex; }
    void setVolumeIndex(int index);
    /// Rebuilds the volume list from the index. Called on its own when a scan
    /// finishes anywhere, so a folder just indexed is searchable without the
    /// user knowing there is a list to reload.
    Q_INVOKABLE void refreshVolumes();
    /// Queues a background scan that fills the index for `uri`.
    ///
    /// Incremental by default: a re-scan keeps what has not changed rather than
    /// walking the tree again. `full` is what somebody reaches for when they
    /// suspect the index.
    Q_INVOKABLE void scanDirectory(const QString& uri, const QString& label, bool full = false);

    /// Asks for `uri` to be re-indexed every `seconds`, through the same
    /// scheduler every other repeating job goes through -- so it survives a
    /// restart and catches up on a run missed while the machine was off.
    ///
    /// Seconds rather than hours, because that is what a `ScheduleRule` holds
    /// and what `ScheduleRule::presets()` offers; hours bought nothing and made
    /// the two dialogs that put work on a clock read differently.
    ///
    /// `seconds <= 0` removes the rule, which is how *Repeat: never* is said --
    /// the same shape as `AnalysisTarget::setSchedule()`. A folder that already
    /// has a rule has its interval changed, keeping the rule's id, rather than
    /// being left on the interval it was first given.
    ///
    /// Returns the rule's id, or empty when it was removed or there is no
    /// scheduler.
    Q_INVOKABLE QString scheduleScan(const QString& uri, qint64 seconds);
    /// Whether a rule already exists for this folder, so the form can offer to
    /// stop rather than to start again.
    Q_INVOKABLE QString scheduledScanId(const QString& uri) const;
    /// How often this folder is re-indexed, or zero when it is not. What the
    /// picker shows, so it opens on the interval a folder is already on.
    Q_INVOKABLE qint64 scheduledScanSeconds(const QString& uri) const;
    /// The intervals to offer, as `{ label, seconds }`. The same list every
    /// other repeating job is offered.
    Q_INVOKABLE QVariantList schedulePresets() const;
    Q_INVOKABLE void unscheduleScan(const QString& uri);

    /// Takes what a person types -- "10M", "1.5 GB", "500k", or nothing at all --
    /// and returns the bytes, or -1 for anything it cannot make sense of. A form
    /// should not make someone count zeros.
    static qint64 parseSize(const QString& text);
    /// The same, telling "left blank" from "cannot be read".
    ///
    /// -1 for an empty field, which is a criterion left blank; nothing for text
    /// that is not a size. parseSize() answers -1 for both, and every caller that
    /// has to tell them apart was getting it wrong: `10 MG` in the form meant no
    /// lower bound. See MOLE-376.
    static std::optional<qint64> sizeFrom(const QString& text);
    Q_INVOKABLE void setSizeRange(const QString& minText, const QString& maxText);

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

    /// Builds a file set from the results and returns its id, or an empty string
    /// when there is nothing to build one from.
    ///
    /// A snapshot of what is on screen -- including the narrowing filter, because
    /// the rows in front of the user are what "these results" means -- rather than
    /// something that re-runs the query later. "Build a set from this" is what
    /// anyone reading it expects, and it is the one that cannot surprise them.
    Q_INVOKABLE QString buildSetFromResults(const QString& name);

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void rootUriChanged();
    void queryTextChanged();
    void criteriaChanged();
    void runningChanged();
    void statusChanged();
    void scopeChanged();
    void coverageChanged();
    void queryLineChanged();
    void volumesChanged();
    void volumeIndexChanged();

private:
    /// The deepest indexed volume whose root is a prefix of the search root, if
    /// any covers it at all.
    std::optional<IndexVolume> coveringVolume() const;
    /// Volumes whose root sits *inside* the search root, so each covers part of
    /// it and none covers the whole. The ordinary case: people index the big
    /// slow tree, not the disk it is on.
    QList<IndexVolume> volumesInsideRoot() const;
    /// Runs the walk over `root`, superseding whatever the index already put on
    /// screen. `primed` is the rows it did, keyed by their parent directory.
    void startWalk(FileSystemPtr fileSystem, const VfsUri& root, const SearchQuery& query,
        const QHash<QString, QStringList>& primed);
    /// "scanned 3 days ago", taken from the oldest of `volumes` -- the age a
    /// mixed list has to admit to is the age of its stalest part.
    static QString oldestScanNote(const QList<IndexVolume>& volumes);
    /// The status line for a walk, accounting for the indexed half when there
    /// was one. Unchanged text when there was not.
    QString walkStatus(const QString& walkText, bool finished) const;
    /// Asks the index and reports the count through `doneFormat`, which takes
    /// it as %1. Both scopes that reach the index come through here, so the two
    /// differ in what they say and in nothing else.
    void startIndexSearch(const SearchQuery& query, const QString& doneFormat);
    /// Everything the form is asking for, as one query.
    SearchQuery buildQuery() const;
    /// How to read a file, for the criteria an entry cannot answer. The ceiling
    /// is smaller for anything that is not the local disk, where reading is
    /// downloading.
    /// How a hit is read, whichever drive it turns out to be on.
    ///
    /// `root` decides only the read ceiling. Every uri goes through
    /// backendFor(), because with `everywhere:yes` a hit can be on any volume
    /// and one backend per scheme opened host B's path on host A. See MOLE-375.
    SearchIo searchIoFor(const VfsUri& root) const;
    /// Everything the form is asking a scan for. `incremental` is the caller's,
    /// because "full rescan" is a button and not a checkbox; the other two are
    /// the form's own.
    ScanOptions scanOptions(bool incremental) const;
    /// The backend that owns `uri`, mounting a container on demand. What lets a
    /// content search reach inside an archive nobody has opened.
    FileSystemPtr backendFor(const VfsUri& uri) const;
    /// Records what `source` could not state, for the form to show.
    void notePlan(const SearchQuery& query, SearchSource source);
    /// Writes the line out of the fields. Called whenever one of them moves.
    void rewriteQueryLine();
    void setRunning(bool running);
    void setStatusText(const QString& text);

    PluginServices m_services;
    FileListModel* m_results = nullptr;
    QString m_rootUri;
    QString m_queryText;
    QString m_extension;
    bool m_caseSensitive = false;
    int m_nameMode = 0;
    bool m_wholeWord = false;
    bool m_excludeName = false;
    QString m_pathText;
    bool m_excludePath = false;
    QStringList m_typeClasses;
    QString m_modifiedFrom;
    QString m_modifiedTo;
    QString m_createdFrom;
    QString m_accessedFrom;
    int m_kindMode = 0;
    bool m_emptyOnly = false;
    bool m_includeHidden = true;
    int m_maxDepth = -1;
    QString m_excluded;
    QString m_unpushedNote;
    QString m_contentText;
    bool m_contentRegex = false;
    bool m_searchBinary = false;
    bool m_scanReadsMetadata = ScanOptions::dialogDefaults().metadata;
    bool m_scanOpensArchives = ScanOptions::dialogDefaults().archives;
    QVariantMap m_factCriteria;
    QString m_blockedReason;
    QString m_queryLine;
    QString m_queryLineError;
    int m_queryLineErrorAt = -1;
    /// Guards the loop: the line writes the fields and the fields rewrite the
    /// line, and without this the two would chase each other while somebody
    /// types.
    bool m_rewriting = false;
    qint64 m_minSize = -1;
    qint64 m_maxSize = -1;
    bool m_useIndex = true;
    bool m_everywhere = false;
    QStringList m_volumeLabels;
    QList<qint64> m_volumeIds;
    int m_volumeIndex = 0;
    bool m_running = false;
    bool m_truncated = false;
    QString m_statusText;
    QPointer<LiveSearchTask> m_task;
    /// The other engine. Both run for a partly indexed folder, one after the
    /// other: the index primes the list and the walk corrects it.
    QPointer<IndexSearchTask> m_indexTask;
    /// How the status line accounts for a search answered by both halves.
    int m_primedFromIndex = 0;
    QString m_primedNote;
};

class LiveSearchFeature final : public IFeature
{
public:
    LiveSearchFeature(PluginServices services, QString defaultRoot);

    QString id() const override { return QStringLiteral("mole.livesearch"); }
    QString title() const override { return QStringLiteral("Search"); }
    QString description() const override
    {
        return QStringLiteral("Find files by name, in a folder or across everything indexed.");
    }
    QString iconText() const override { return QStringLiteral("\U0001F50D"); }
    int sortOrder() const override { return 20; }
    /// One search per question, and people ask several at once.
    bool opensFromNothing() const override { return true; }
    /// The indexed search was a second tab for a scope this one has as a field.
    QStringList absorbedIds() const override { return { QStringLiteral("mole.indexsearch") }; }

    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
    QString m_defaultRoot;
};

} // namespace mole
