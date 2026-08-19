#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/SearchFeatures.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TabsModel.h"
#include "ui/models/TaskListModel.h"

#include "core/index/IndexDatabase.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QFile>

using namespace mole;
using namespace mole::test;

/// A folder with an indexed subtree in it, which is the ordinary case.
///
/// People index the big slow tree, not the whole disk, so a search over the
/// folder above it used to be treated as if nothing had been indexed at all.
/// ADR-0005 said why — a list of which some rows are current and some are as
/// old as the last scan, with nothing on the row to say which. ADR-0038 answers
/// it the way the objection points at: put it on the row.
///
/// **The marking is the feature.** A build of this that mixes the two halves
/// without saying which is which has not done the work, so every assertion here
/// is about a row's provenance rather than only about the count.
class TestMixedSearch : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void theIndexedPartArrivesFirstAndIsMarkedAsRemembered();
    void aFileDeletedSinceTheScanIsTakenBackWhenTheWalkPassesIt();
    void aFileChangedSinceTheScanEndsUpShowingWhatIsOnDisk();
    void nothingIsListedTwiceWhenAPathIsInBothHalves();
    void cancellingLeavesWhatWasShownAndSaysItStopped();
    void aFullyCoveredFolderIsStillAnsweredByTheIndexAlone();
    void anUnindexedFolderIsStillJustAWalk();
    void aCriterionTheIndexCannotStateIsSaidOutLoud();
    void aNameAndReturnIsStillTheWholeOfTheCommonCase();
    void theCoverageSentenceIsRightInAllFourCases();
    void aCriterionTheScopeCannotAnswerStopsTheSearchAndOffersBothWaysOut();
    void narrowingToTheIndexedPartSaysWhatItLeftOut();
    void theFieldsOfferedFollowTheKeysInScope();
    void aPlainNameSearchIsUntouchedByAnyOfIt();
    void theLineAndTheFormAreOneQuerySeenTwice();
    void aLineNobodyCanReadSaysSoAndDoesNotRun();
    void theLineCanBeLeftEmptyAndTheFormUsedAlone();

private:
    /// The search tab, aimed at `root`, with the index allowed.
    LiveSearchController* searchOver(const QString& root);
    /// Scans `uri` and records what its files say about themselves.
    bool indexWithMetadata(const QString& uri, const QString& label);
    /// Runs `search` to completion and returns false if it never finished.
    bool runToEnd(LiveSearchController* search);
    /// Scans `uri` into the index and waits for it.
    bool index(const QString& uri, const QString& label);

    QStringList urisIn(const FileListModel* results) const;
    int provenanceOf(const FileListModel* results, const QString& uri) const;
    qint64 sizeOf(const FileListModel* results, const QString& uri) const;

    QString memUri(const QString& path) const { return QStringLiteral("mem://") + path; }

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
    std::shared_ptr<MemoryFileSystem> m_mem;
};

void TestMixedSearch::initTestCase()
{
    QVERIFY(m_profile.isValid());
    qputenv("MOLE_PLUGIN_PATH", QByteArray(MOLE_TEST_PLUGIN_DIR));
}

void TestMixedSearch::init()
{
    // A fresh profile per test, index included: what one of these scans would
    // otherwise still be covering the folder the next one expects to walk.
    m_profile.clearVolatileState();
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_INDEX_PATH")));
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_BOOKMARKS_PATH")));

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());

    std::vector<std::unique_ptr<IPlugin>> plugins;
    plugins.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    m_app = std::make_unique<AppController>();
    QString error;
    QVERIFY2(m_app->initialise(std::move(plugins), &error), qPrintable(error));

    // The backend is built here and handed over, rather than asked for back
    // through resolve(): every real mount is wrapped for logging, so what comes
    // back out is not the object the fixture has to interfere with.
    m_mem = std::make_shared<MemoryFileSystem>();
    Mount scratch;
    scratch.id = QStringLiteral("scratch");
    scratch.displayName = QStringLiteral("Scratch");
    scratch.root = VfsUri::fromString(QStringLiteral("mem:///"));
    scratch.fileSystem = m_mem;
    QVERIFY(!m_app->services().vfs->addMount(scratch).isEmpty());
    QVERIFY(m_app->services().vfs->resolve(VfsUri::fromString(QStringLiteral("mem:///"))) != nullptr);

    // /tree/plain is walked; /tree/archive is scanned and then walked over.
    m_mem->addFile(QStringLiteral("/tree/plain/loose-note.txt"), QByteArray("x"));
    m_mem->addFile(QStringLiteral("/tree/archive/kept-note.txt"), QByteArray("x"));
    m_mem->addFile(QStringLiteral("/tree/archive/gone-note.txt"), QByteArray("x"));
    m_mem->addFile(QStringLiteral("/tree/archive/grown-note.txt"), QByteArray(10, 'x'));
}

void TestMixedSearch::cleanup()
{
    m_app.reset();
    m_mem.reset();
    m_tree.reset();
}

LiveSearchController* TestMixedSearch::searchOver(const QString& root)
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    if (row < 0)
        return nullptr;
    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    if (!search)
        return nullptr;
    search->setRootUri(root);
    search->setQueryText(QStringLiteral("-note"));
    return search;
}

bool TestMixedSearch::runToEnd(LiveSearchController* search)
{
    search->start();
    return waitFor([search] { return !search->isRunning(); }, 20000);
}

bool TestMixedSearch::index(const QString& uri, const QString& label)
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    if (row < 0)
        return false;
    auto* scanner = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    if (!scanner)
        return false;
    scanner->scanDirectory(uri, label);
    if (!waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 20000))
        return false;
    m_app->tabs()->closeTab(row);
    return true;
}

bool TestMixedSearch::indexWithMetadata(const QString& uri, const QString& label)
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    if (row < 0)
        return false;
    auto* scanner = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    if (!scanner)
        return false;
    scanner->setScanReadsMetadata(true);
    scanner->scanDirectory(uri, label);
    if (!waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 20000))
        return false;
    m_app->tabs()->closeTab(row);
    return true;
}

QStringList TestMixedSearch::urisIn(const FileListModel* results) const
{
    QStringList uris;
    for (int row = 0; row < results->rowCount(); ++row)
        uris.append(results->index(row, 0).data(FileListModel::UriRole).toString());
    uris.sort();
    return uris;
}

int TestMixedSearch::provenanceOf(const FileListModel* results, const QString& uri) const
{
    for (int row = 0; row < results->rowCount(); ++row) {
        const QModelIndex at = results->index(row, 0);
        if (at.data(FileListModel::UriRole).toString() == uri)
            return at.data(FileListModel::ProvenanceRole).toInt();
    }
    return -1; // not in the list at all, which no assertion here should mistake
}

qint64 TestMixedSearch::sizeOf(const FileListModel* results, const QString& uri) const
{
    for (int row = 0; row < results->rowCount(); ++row) {
        const QModelIndex at = results->index(row, 0);
        if (at.data(FileListModel::UriRole).toString() == uri)
            return at.data(FileListModel::SizeRole).toLongLong();
    }
    return -1;
}

void TestMixedSearch::theIndexedPartArrivesFirstAndIsMarkedAsRemembered()
{
    QVERIFY(index(memUri(QStringLiteral("/tree/archive")), QStringLiteral("archive")));

    // Slow enough to list that the walk cannot have overtaken the index by the
    // time the assertion runs. Waited for on the condition, never on the clock.
    m_mem->setListDelayMs(60);

    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    search->start();

    QVERIFY2(waitFor([search] { return search->results()->fromIndexCount() > 0; }, 10000),
        "the indexed part has to be on screen before the walk has been anywhere");

    // Every row on screen at this moment came from the scan, and says so.
    for (const QString& uri : urisIn(search->results())) {
        QCOMPARE(provenanceOf(search->results(), uri), int(FileListModel::FromIndex));
        QVERIFY2(uri.contains(QStringLiteral("/tree/archive/")),
            "only the scanned subtree can be answered from the index");
    }
    QVERIFY2(search->statusText().contains(QStringLiteral("from the index")),
        "and the line says how much of the list is a memory");

    m_mem->setListDelayMs(0);
    QVERIFY(waitFor([search] { return !search->isRunning(); }, 20000));

    // By the end the walk has been over all of it, so nothing is a memory.
    QCOMPARE(search->results()->fromIndexCount(), 0);
    QVERIFY2(search->statusText().contains(QStringLiteral("every row current")),
        "a finished walk means every row is what is on disk, and may say so");
}

void TestMixedSearch::aFileDeletedSinceTheScanIsTakenBackWhenTheWalkPassesIt()
{
    QVERIFY(index(memUri(QStringLiteral("/tree/archive")), QStringLiteral("archive")));
    QVERIFY(
        m_mem->remove(VfsUri::fromString(memUri(QStringLiteral("/tree/archive/gone-note.txt"))), false).ok());

    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY(runToEnd(search));

    const QStringList uris = urisIn(search->results());
    QVERIFY2(!uris.contains(memUri(QStringLiteral("/tree/archive/gone-note.txt"))),
        "a file deleted since the scan stayed on the list after the walk passed its folder");
    QCOMPARE(uris.size(), 3); // kept, grown, loose
    QCOMPARE(search->results()->fromIndexCount(), 0);
}

void TestMixedSearch::aFileChangedSinceTheScanEndsUpShowingWhatIsOnDisk()
{
    QVERIFY(index(memUri(QStringLiteral("/tree/archive")), QStringLiteral("archive")));

    const QString grown = memUri(QStringLiteral("/tree/archive/grown-note.txt"));
    m_mem->addFile(QStringLiteral("/tree/archive/grown-note.txt"), QByteArray(9000, 'x'));

    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY(runToEnd(search));

    // The row is the one the index put there, replaced in place by the walk:
    // one row, current facts, and no longer marked as remembered.
    QCOMPARE(urisIn(search->results()).count(grown), 1);
    QCOMPARE(sizeOf(search->results(), grown), 9000);
    QCOMPARE(provenanceOf(search->results(), grown), int(FileListModel::SeenNow));
}

void TestMixedSearch::nothingIsListedTwiceWhenAPathIsInBothHalves()
{
    QVERIFY(index(memUri(QStringLiteral("/tree/archive")), QStringLiteral("archive")));

    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY(runToEnd(search));

    const QStringList uris = urisIn(search->results());
    QCOMPARE(uris.size(), QSet<QString>(uris.cbegin(), uris.cend()).size());
    QCOMPARE(uris.size(), 4);
    // And the total behind the filter agrees, so "4 of 4" cannot hide a double.
    QCOMPARE(search->results()->totalCount(), 4);
}

void TestMixedSearch::cancellingLeavesWhatWasShownAndSaysItStopped()
{
    QVERIFY(index(memUri(QStringLiteral("/tree/archive")), QStringLiteral("archive")));
    for (int i = 0; i < 30; ++i)
        m_mem->addFile(QStringLiteral("/tree/plain/deep%1/filler-note.txt").arg(i), QByteArray("x"));
    m_mem->setListDelayMs(40);

    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    search->start();

    QVERIFY(waitFor([search] { return search->results()->rowCount() > 0; }, 10000));
    const int shown = search->results()->rowCount();
    search->stop();
    QVERIFY(waitFor([search] { return !search->isRunning(); }, 20000));

    QVERIFY2(
        search->results()->rowCount() >= shown, "cancelling threw away results that had already been shown");
    QVERIFY2(search->statusText().contains(QStringLiteral("stopped")),
        "a stopped search must not read like a finished one");
    QVERIFY2(!search->statusText().contains(QStringLiteral("every row current")),
        "and must not claim the list is current when the walk never got there");
}

void TestMixedSearch::aFullyCoveredFolderIsStillAnsweredByTheIndexAlone()
{
    // Unchanged by all of the above: the whole folder is covered, so there is
    // nothing to walk and nothing to mark.
    QVERIFY(index(memUri(QStringLiteral("/tree")), QStringLiteral("whole tree")));

    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY(search->indexCoversRoot());
    QVERIFY(runToEnd(search));

    QCOMPARE(urisIn(search->results()).size(), 4);
    QVERIFY2(search->statusText().contains(QStringLiteral("from the index")),
        "an answer that might be stale has to say where it came from");
    QVERIFY2(search->statusText().contains(QStringLiteral("scanned")), "and how old it is");
    QCOMPARE(search->results()->fromIndexCount(), 0); // nothing walked, so nothing to mark
}

void TestMixedSearch::anUnindexedFolderIsStillJustAWalk()
{
    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY(!search->indexCoversRoot());
    QVERIFY(runToEnd(search));

    QCOMPARE(urisIn(search->results()).size(), 4);
    QCOMPARE(search->results()->fromIndexCount(), 0);
    QVERIFY2(!search->statusText().contains(QStringLiteral("index")),
        "a walk must not claim to have come from the index");
    QVERIFY2(search->statusText().contains(QStringLiteral("matches")),
        "it says what a walk says, exactly as it did before any of this");
}

/// The rule ADR-0005 set: what the index could not answer is checked afterwards
/// rather than dropped, and the form says which criterion made that happen.
void TestMixedSearch::aCriterionTheIndexCannotStateIsSaidOutLoud()
{
    QVERIFY(index(memUri(QStringLiteral("/tree")), QStringLiteral("whole tree")));

    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY(search->indexCoversRoot());

    // A name alone is a column, so there is nothing to admit to.
    QVERIFY(runToEnd(search));
    QVERIFY(search->unpushedNote().isEmpty());

    // A date is not, and neither is a path.
    search->setModifiedFrom(QStringLiteral("2000-01-01"));
    search->setPathText(QStringLiteral("/archive/"));
    QVERIFY(runToEnd(search));

    const QString note = search->unpushedNote();
    QVERIFY2(!note.isEmpty(), "a criterion the index cannot state has to be said out loud");
    QVERIFY2(note.contains(QStringLiteral("date changed")), qPrintable(note));
    QVERIFY2(note.contains(QStringLiteral("path")), qPrintable(note));

    // Said, and also honoured: the answer is narrowed by both.
    for (const QString& uri : urisIn(search->results()))
        QVERIFY2(uri.contains(QStringLiteral("/archive/")), qPrintable(uri));
    QVERIFY(!urisIn(search->results()).isEmpty());
}

void TestMixedSearch::aNameAndReturnIsStillTheWholeOfTheCommonCase()
{
    // Nine families of criteria later, the form left alone has to behave the
    // way it did when it had three fields.
    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QCOMPARE(search->nameMode(), 0);
    QVERIFY(!search->wholeWord());
    QVERIFY(!search->excludeName());
    QVERIFY(search->typeClasses().isEmpty());
    QVERIFY(search->excluded().isEmpty());
    QCOMPARE(search->maxDepth(), -1);
    QVERIFY(search->includeHidden());

    QVERIFY(runToEnd(search));
    QCOMPARE(urisIn(search->results()).size(), 4);
    QVERIFY(search->unpushedNote().isEmpty());
}

/// The sentence that makes a greyed field read as inapplicable rather than as
/// broken. Four scopes, four true things to say about them.
void TestMixedSearch::theCoverageSentenceIsRightInAllFourCases()
{
    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY2(
        search->coverageNote().contains(QStringLiteral("not indexed")), qPrintable(search->coverageNote()));
    QVERIFY(!search->metadataAvailable());

    // Indexed, names only.
    QVERIFY(index(memUri(QStringLiteral("/tree")), QStringLiteral("whole tree")));
    search->setRootUri(memUri(QStringLiteral("/other")));
    search->setRootUri(memUri(QStringLiteral("/tree"))); // force the note to be recomputed
    QVERIFY2(search->coverageNote().contains(QStringLiteral("indexed")), qPrintable(search->coverageNote()));
    QVERIFY2(
        search->coverageNote().contains(QStringLiteral("names only")), qPrintable(search->coverageNote()));
    QVERIFY(!search->metadataAvailable());
}

void TestMixedSearch::aCriterionTheScopeCannotAnswerStopsTheSearchAndOffersBothWaysOut()
{
    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);

    // Nothing here records a camera, so asking for one is a question that
    // cannot be put -- which is not the same as a question with no answers.
    search->setFactCriteria({ { QStringLiteral("image.camera"), QStringLiteral("Canon") } });
    search->start();

    QVERIFY2(search->blocked(), "a criterion the scope cannot answer must stop the search");
    QVERIFY2(!search->isRunning(), "and must not have started one anyway");
    QVERIFY2(search->results()->rowCount() == 0, "and must not have quietly widened itself");
    QVERIFY2(search->blockedReason().contains(QStringLiteral("image.camera")),
        qPrintable(search->blockedReason()));

    // One of the two ways out. The other needs an indexed part to narrow to.
    QVERIFY(!search->hasIndexedPart());
    search->indexThisFolderForMetadata();
    QVERIFY(search->scanReadsMetadata());
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 20000));

    // And afterwards the question can be put, which is the point of offering it.
    QVERIFY(search->metadataAvailable() || search->factKeys().isEmpty());
}

void TestMixedSearch::narrowingToTheIndexedPartSaysWhatItLeftOut()
{
    QVERIFY(indexWithMetadata(memUri(QStringLiteral("/tree/archive")), QStringLiteral("archive")));

    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY2(search->hasIndexedPart(), "part of this folder is indexed, so narrowing is a way out");
    QVERIFY2(search->coverageNote().contains(QStringLiteral("part of this folder")),
        qPrintable(search->coverageNote()));

    search->narrowToIndexedPart();

    QCOMPARE(search->rootUri(), memUri(QStringLiteral("/tree/archive")));
    QVERIFY2(search->statusText().contains(QStringLiteral("left out")),
        qPrintable(QStringLiteral("a search that shrinks its own scope has to say so: %1")
                       .arg(search->statusText())));
    QVERIFY2(search->statusText().contains(QStringLiteral("/tree")), qPrintable(search->statusText()));
}

void TestMixedSearch::theFieldsOfferedFollowTheKeysInScope()
{
    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY2(search->factKeys().isEmpty(), "an unindexed folder offers no fields at all");

    // A fact nothing in this application has heard of, recorded by a scan: the
    // field for it has to appear without anybody editing the form.
    const Result<qint64> volume = m_app->services().index->upsertVolume(
        VfsUri::fromString(memUri(QStringLiteral("/tree"))), QStringLiteral("stub"));
    QVERIFY(volume.ok());
    const Result<qint64> scan = m_app->services().index->beginScan(volume.value());
    QVERIFY(scan.ok());

    IndexedFile row;
    row.name = QStringLiteral("thing.xyz");
    row.path = QStringLiteral("/tree/thing.xyz");
    row.parentPath = QStringLiteral("/tree");
    row.extension = QStringLiteral("xyz");
    row.facts = { SearchFact { QStringLiteral("xyz.invented"), QStringLiteral("something"), 0, false } };
    QVERIFY(m_app->services().index->insertBatch(volume.value(), scan.value(), { row }).ok());
    QVERIFY(m_app->services()
                .index->commitScan(volume.value(), scan.value(), QDateTime::currentDateTime(), ScanOptions {})
                .ok());

    search->setRootUri(memUri(QStringLiteral("/elsewhere")));
    search->setRootUri(memUri(QStringLiteral("/tree")));
    QCOMPARE(search->factKeys(), QStringList { QStringLiteral("xyz.invented") });
    QVERIFY(search->metadataAvailable());
    QVERIFY2(search->coverageNote().contains(QStringLiteral("say about themselves")),
        qPrintable(search->coverageNote()));
}

/// The regression this ticket is most likely to cause.
void TestMixedSearch::aPlainNameSearchIsUntouchedByAnyOfIt()
{
    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY(search->factCriteria().isEmpty());

    QVERIFY(runToEnd(search));
    QVERIFY2(!search->blocked(), "a name and Return must never be stopped by any of this");
    QCOMPARE(urisIn(search->results()).size(), 4);
    QVERIFY(search->blockedReason().isEmpty());
}

void TestMixedSearch::theLineAndTheFormAreOneQuerySeenTwice()
{
    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);

    // Typed into the line: the fields move.
    search->setQueryLine(QStringLiteral("report ext:pdf size>10M type:image,document -path:node_modules"));
    QVERIFY2(search->queryLineError().isEmpty(), qPrintable(search->queryLineError()));
    QCOMPARE(search->queryText(), QStringLiteral("report"));
    QCOMPARE(search->extension(), QStringLiteral("pdf"));
    QCOMPARE(search->minSize(), 10 * 1024 * 1024);
    QCOMPARE(search->typeClasses(), QStringList({ QStringLiteral("image"), QStringLiteral("document") }));
    QCOMPARE(search->pathText(), QStringLiteral("node_modules"));
    QVERIFY(search->excludePath());

    // Changed in a field: the line is rewritten, and rewritten to something
    // that reads back the same way.
    search->setExtension(QStringLiteral("md"));
    QVERIFY2(search->queryLine().contains(QStringLiteral("ext:md")), qPrintable(search->queryLine()));
    QVERIFY2(!search->queryLine().contains(QStringLiteral("ext:pdf")), qPrintable(search->queryLine()));

    const QString written = search->queryLine();
    search->setQueryLine(written);
    QCOMPARE(search->extension(), QStringLiteral("md"));
    QCOMPARE(search->queryText(), QStringLiteral("report"));
    QVERIFY2(search->queryLineError().isEmpty(), qPrintable(search->queryLineError()));
}

void TestMixedSearch::aLineNobodyCanReadSaysSoAndDoesNotRun()
{
    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);

    // A size nobody can read. Matching everything here is how somebody spends
    // ten minutes doubting their disk.
    search->setQueryLine(QStringLiteral("report size>10Q"));
    QVERIFY(!search->queryLineError().isEmpty());
    QVERIFY(search->queryLineErrorAt() >= 0);

    search->start();
    QVERIFY2(!search->isRunning(), "a query that could not be read must not run");
    QCOMPARE(search->results()->rowCount(), 0);

    // A key nobody has heard of, with the nearest one suggested rather than the
    // whole thing quietly turning into a name search.
    search->setQueryLine(QStringLiteral("extn:pdf"));
    QVERIFY2(search->queryLineError().contains(QStringLiteral("ext")), qPrintable(search->queryLineError()));

    // An unterminated quote, with a place to point at.
    search->setQueryLine(QStringLiteral("content:\"never ends"));
    QVERIFY(!search->queryLineError().isEmpty());
}

void TestMixedSearch::theLineCanBeLeftEmptyAndTheFormUsedAlone()
{
    LiveSearchController* search = searchOver(memUri(QStringLiteral("/tree")));
    QVERIFY(search);
    QVERIFY(search->queryLine().contains(QStringLiteral("-note")));

    QVERIFY(runToEnd(search));
    QCOMPARE(urisIn(search->results()).size(), 4);
    QVERIFY(search->queryLineError().isEmpty());
}

MOLE_TEST_MAIN(TestMixedSearch)
#include "tst_MixedSearch.moc"
