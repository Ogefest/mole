#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "ui/models/FileListModel.h"

#include "core/index/IndexDatabase.h"
#include "core/search/LiveSearchTask.h"
#include "core/search/SearchQuery.h"

#include <QPointer>
#include <QStringList>

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
    Q_INVOKABLE void scanDirectory(const QString& uri, const QString& label);

    /// Takes what a person types -- "10M", "1.5 GB", "500k", or nothing at all --
    /// and returns the bytes, or -1 for anything it cannot make sense of. A form
    /// should not make someone count zeros.
    static qint64 parseSize(const QString& text);
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
    SearchIo searchIoFor(const FileSystemPtr& fileSystem, const VfsUri& root) const;
    /// What a file says about itself, through the readers that fill the details
    /// panel. The same readers, so the index and the panel can never disagree.
    std::function<QList<SearchFact>(const FileEntry&)> factReaderFor(const FileSystemPtr& fileSystem) const;
    /// Records what `source` could not state, for the form to show.
    void notePlan(const SearchQuery& query, SearchSource source);
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
    bool m_scanReadsMetadata = false;
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
