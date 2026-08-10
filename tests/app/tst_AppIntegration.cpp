#include "host/FeatureRegistry.h"
#include "host/PreviewRegistry.h"
#include "plugins/builtin/BrowserFeature.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/SearchFeatures.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/FileLauncher.h"
#include "ui/models/DriveListModel.h"
#include "ui/models/TabsModel.h"
#include "ui/models/TaskListModel.h"

#include "core/alerts/AlertStore.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/sets/FileSet.h"
#include "core/sets/FileSetStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// Exercises the assembled application the way main() builds it, minus the QML
/// engine. If the layers do not fit together, this is where it shows.
class TestAppIntegration : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void startsWithBuiltinFeaturesAndDrives();
    void opensABrowserTabByDefault();
    void everyRegisteredFeatureCanOpenATab();
    void browserTabBrowsesTheRealFilesystem();
    void measuringFoldersFillsTheirSizesIntoTheListing();
    void measuringUsesTheTickedFoldersWhenThereAreAny();
    void splitViewGivesTwoIndependentPanes();
    void browserTabRenamesItselfWhileNavigating();
    void directoryChangedEventRefreshesOpenPanes();
    void scanThenIndexSearchFindsTheFile();
    void liveSearchFindsFilesOnDisk();
    void searchSizesAreTypedTheWayPeopleWriteThem();
    void searchWalksWhenNothingIsIndexed();
    void searchAsksTheIndexWhenItCoversTheFolder();
    void turningTheIndexOffForcesAWalk();
    void narrowingResultsDoesNotSearchAgain();
    void aSetCanBeBuiltFromWhatTheSearchFound();
    void revealingAFileOpensItsFolderWithTheCursorOnIt();
    void compressActsOnTheCursorWhenNothingIsTicked();
    void textPreviewProviderClaimsTextFiles();
    void archivePluginMountsAZip();
    void dualPaneIsItsOwnFeature();
    void copyBetweenPanesMovesRealFiles();
    void moveBetweenPanesRemovesTheSource();
    void transferRefusesWhenBothPanesShowTheSameFolder();
    void transferIsUnavailableInSinglePane();
    void transferAvailabilityIsNotified();
    void thePlanNamesWhatWouldBeOverwritten();
    void aSingleItemCanArriveUnderANewName();
    void skipLeavesTheExistingFileAlone();
    void overwriteReplacesIt();
    void tellsWhatIsAlreadyKnownAboutTheFolder();
    void reportsWhoMayDoWhatHere();
    void openExternallyGoesThroughTheLauncher();
    void menuHasTheClassicSections();
    void theTypeScaleIsOrderedAndAboveTheFloor();
    void menuOffersOneNewTabEntryPerFeature();
    void menuEntryOpensTheTab();
    void viewMenuReflectsAndTogglesTheCurrentTab();
    void menuEntriesGreyOutWhenTheyDoNotApply();
    void viewMenuOffersThreeExclusiveLayouts();
    void manyTabsOpenAndCloseWithoutLeaking();
    void shutsDownCleanlyWithWorkInFlight();

private:
    std::vector<std::unique_ptr<IPlugin>> builtIns() const;
    /// Puts both panes in place for a transfer and returns the browser.
    BrowserController* readyToTransfer(const QString& destination);

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
};

void TestAppIntegration::initTestCase()
{
    // Never touch the developer's real index while testing.
    QVERIFY(m_profile.isValid());
    qputenv("MOLE_PLUGIN_PATH", QByteArray(MOLE_TEST_PLUGIN_DIR));
}

std::vector<std::unique_ptr<IPlugin>> TestAppIntegration::builtIns() const
{
    std::vector<std::unique_ptr<IPlugin>> plugins;
    plugins.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    return plugins;
}

void TestAppIntegration::init()
{
    // A fresh profile per test: a session left by the previous one would
    // restore tabs pointing at a temporary tree that no longer exists.
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_BOOKMARKS_PATH")));

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->writeFile(QStringLiteral("notes.txt"), QByteArray("some text to preview")));
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/quarterly.txt"), QByteArray("q1")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("empty")));

    m_app = std::make_unique<AppController>();
    QString error;
    QVERIFY2(m_app->initialise(builtIns(), &error), qPrintable(error));
}

void TestAppIntegration::cleanup()
{
    m_app.reset();
    m_tree.reset();
}

void TestAppIntegration::startsWithBuiltinFeaturesAndDrives()
{
    QVERIFY2(m_app->pluginErrors().isEmpty(), qPrintable(m_app->pluginErrors().join(QLatin1Char('\n'))));

    QVERIFY(m_app->features()->feature(QStringLiteral("mole.browser")) != nullptr);
    QVERIFY(m_app->features()->feature(QStringLiteral("mole.livesearch")) != nullptr);
    QVERIFY(m_app->features()->feature(QStringLiteral("mole.indexsearch")) != nullptr);

    // Home and Filesystem are always there.
    QVERIFY(m_app->drives()->rowCount() >= 2);
    QVERIFY(m_app->services().isValid());
}

void TestAppIntegration::opensABrowserTabByDefault()
{
    QCOMPARE(m_app->tabs()->rowCount(), 1);
    QCOMPARE(m_app->tabs()->currentIndex(), 0);
    QCOMPARE(
        m_app->tabs()->index(0, 0).data(TabsModel::FeatureIdRole).toString(), QStringLiteral("mole.browser"));
}

void TestAppIntegration::everyRegisteredFeatureCanOpenATab()
{
    // A feature that registers but cannot produce a working tab is worse than
    // one that is missing, so open every single one.
    const QList<IFeature*> features = m_app->features()->features();
    QVERIFY(!features.isEmpty());

    for (IFeature* feature : features) {
        const int row = m_app->tabs()->openTab(feature->id());
        QVERIFY2(row >= 0, qPrintable(QStringLiteral("feature %1 refused to open").arg(feature->id())));

        QObject* controller = m_app->tabs()->controllerAt(row);
        QVERIFY2(controller, qPrintable(QStringLiteral("feature %1 made no controller").arg(feature->id())));
        QVERIFY(!feature->viewSource().isEmpty());
    }

    drainEvents();
    QCOMPARE(m_app->tabs()->rowCount(), 1 + static_cast<int>(features.size()));
}

void TestAppIntegration::browserTabBrowsesTheRealFilesystem()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);

    BrowserPaneController* pane = browser->activePane();
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() > 0; }));

    // notes.txt, reports/ and empty/
    QCOMPARE(pane->files()->rowCount(), 3);
    QVERIFY(pane->errorText().isEmpty());
}

void TestAppIntegration::measuringFoldersFillsTheirSizesIntoTheListing()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/deeper/more.txt"), QByteArray(700, 'x')));

    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    BrowserPaneController* pane = browser->activePane();
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 3; }));

    FileListModel* files = pane->files();
    const QString reports = m_tree->rootUri().child(QStringLiteral("reports")).toString();
    const QString empty = m_tree->rootUri().child(QStringLiteral("empty")).toString();

    // A folder is unmeasured until asked, and the listing says nothing rather
    // than showing the inode's own size as though it were the contents.
    QCOMPARE(files->measuredSize(reports), -1);
    QCOMPARE(files->data(files->index(files->rowOfUri(reports), 0), FileListModel::SizeTextRole).toString(),
        QString());

    m_app->measureFolderSizes();

    // Nothing ticked, so every folder in the listing is measured -- "which of
    // these is the big one" is the question being asked.
    QVERIFY(waitFor([files, reports] { return files->measuredSize(reports) >= 0; }, 15000));
    QVERIFY(waitFor([files, empty] { return files->measuredSize(empty) >= 0; }, 15000));

    // "q1" is two bytes, plus the 700 nested one level down.
    QCOMPARE(files->measuredSize(reports), 702);
    QCOMPARE(files->measuredSize(empty), 0);

    // And the row shows it, which is the whole point of measuring in the listing.
    const QString shown
        = files->data(files->index(files->rowOfUri(reports), 0), FileListModel::SizeTextRole).toString();
    QVERIFY2(!shown.isEmpty(), "the measured total has to reach the row");

    // A file's size is untouched by any of this.
    const QString notes = m_tree->rootUri().child(QStringLiteral("notes.txt")).toString();
    QCOMPARE(files->measuredSize(notes), -1);

    // Refreshing throws the measurements away: they describe the tree as it was
    // when they were taken, and a stale number is worse than an empty cell.
    pane->refresh();
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 3; }));
    QCOMPARE(files->measuredSize(reports), -1);
}

void TestAppIntegration::measuringUsesTheTickedFoldersWhenThereAreAny()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    BrowserPaneController* pane = browser->activePane();
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 3; }));

    FileListModel* files = pane->files();
    const QString reports = m_tree->rootUri().child(QStringLiteral("reports")).toString();
    const QString empty = m_tree->rootUri().child(QStringLiteral("empty")).toString();

    files->setSelected(files->rowOfUri(empty), true);
    m_app->measureFolderSizes();

    QVERIFY(waitFor([files, empty] { return files->measuredSize(empty) >= 0; }, 15000));
    // The one that was ticked, and only that one: measuring everything when a
    // selection exists would ignore what the user asked for.
    QCOMPARE(files->measuredSize(reports), -1);
}

void TestAppIntegration::splitViewGivesTwoIndependentPanes()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(!browser->splitEnabled());

    browser->toggleSplit();
    QVERIFY(browser->splitEnabled());
    QCOMPARE(browser->activePane(), browser->left());

    browser->setActivePaneIndex(1);
    QCOMPARE(browser->activePane(), browser->right());

    browser->navigateActive(m_tree->rootUri().child(QStringLiteral("reports")).toString());
    QVERIFY(waitFor([browser] { return !browser->right()->isLoading(); }));

    // Moving one pane must not drag the other along.
    QCOMPARE(browser->right()->currentUri(), m_tree->rootUri().child(QStringLiteral("reports")).toString());
    QCOMPARE(browser->left()->currentUri(), m_tree->rootUri().toString());

    browser->mirrorToOtherPane();
    QVERIFY(waitFor([browser] { return !browser->left()->isLoading(); }));
    QCOMPARE(browser->left()->currentUri(), browser->right()->currentUri());
}

void TestAppIntegration::browserTabRenamesItselfWhileNavigating()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);

    browser->navigateActive(m_tree->rootUri().child(QStringLiteral("reports")).toString());
    QVERIFY(waitFor([browser] { return browser->title() == QStringLiteral("reports"); }));

    QCOMPARE(m_app->tabs()->index(0, 0).data(TabsModel::TitleRole).toString(), QStringLiteral("reports"));
}

void TestAppIntegration::directoryChangedEventRefreshesOpenPanes()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    BrowserPaneController* pane = browser->left();
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 3; }));

    // Something outside the app created a file; the bus is how the pane hears
    // about it, without polling and without anyone wiring the two together.
    QVERIFY(m_tree->writeFile(QStringLiteral("appeared.txt")));
    m_app->services().events->postDirectoryChanged(m_tree->rootUri());

    QVERIFY(waitFor([pane] { return pane->files()->rowCount() == 4; }));
}

void TestAppIntegration::scanThenIndexSearchFindsTheFile()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.indexsearch"));
    QVERIFY(row >= 0);
    auto* search = qobject_cast<IndexSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(search);

    search->scanDirectory(m_tree->rootUri().toString(), QStringLiteral("fixture"));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 15000));

    search->refreshVolumes();
    QVERIFY2(search->volumeLabels().size() >= 2, "the scanned volume must appear in the picker");

    search->setQueryText(QStringLiteral("quarterly"));
    search->search();
    QVERIFY(waitFor([search] { return !search->isRunning(); }, 10000));

    QCOMPARE(search->results()->rowCount(), 1);
    QCOMPARE(search->results()->index(0, 0).data(FileListModel::NameRole).toString(),
        QStringLiteral("quarterly.txt"));
}

void TestAppIntegration::liveSearchFindsFilesOnDisk()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    QVERIFY(row >= 0);
    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(search);

    search->setRootUri(m_tree->rootUri().toString());
    search->setQueryText(QStringLiteral("quarterly"));
    search->start();

    QVERIFY(waitFor([search] { return !search->isRunning(); }, 10000));
    QCOMPARE(search->results()->rowCount(), 1);
    QVERIFY(!search->isTruncated());
}

void TestAppIntegration::searchSizesAreTypedTheWayPeopleWriteThem()
{
    // Nobody should have to count zeros to say "bigger than ten megabytes".
    QCOMPARE(LiveSearchController::parseSize(QStringLiteral("100")), 100);
    QCOMPARE(LiveSearchController::parseSize(QStringLiteral("10k")), 10 * 1024);
    QCOMPARE(LiveSearchController::parseSize(QStringLiteral("10 KB")), 10 * 1024);
    QCOMPARE(LiveSearchController::parseSize(QStringLiteral("2M")), 2 * 1024 * 1024);
    QCOMPARE(LiveSearchController::parseSize(QStringLiteral("1.5 GiB")),
        static_cast<qint64>(1.5 * 1024 * 1024 * 1024));
    // A comma is a decimal point in most of Europe, and this application already
    // shows sizes that way.
    QCOMPARE(LiveSearchController::parseSize(QStringLiteral("1,5M")), static_cast<qint64>(1.5 * 1024 * 1024));

    // Nothing, and nonsense, both mean "no limit" rather than zero -- a limit of
    // zero bytes would quietly match nothing at all.
    QCOMPARE(LiveSearchController::parseSize(QString()), -1);
    QCOMPARE(LiveSearchController::parseSize(QStringLiteral("  ")), -1);
    QCOMPARE(LiveSearchController::parseSize(QStringLiteral("big")), -1);
    QCOMPARE(LiveSearchController::parseSize(QStringLiteral("-5M")), -1);
}

void TestAppIntegration::searchWalksWhenNothingIsIndexed()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    QVERIFY(row >= 0);
    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(search);
    search->setRootUri(m_tree->rootUri().toString());

    // Nothing has been scanned, so there is nothing to ask and the form says so by
    // saying nothing.
    QVERIFY(!search->indexCoversRoot());
    QVERIFY(search->indexNote().isEmpty());

    search->setQueryText(QStringLiteral("quarterly"));
    search->start();
    QVERIFY(waitFor([search] { return !search->isRunning(); }, 15000));

    QCOMPARE(search->results()->rowCount(), 1);
    QVERIFY2(!search->statusText().contains(QStringLiteral("index")),
        "a walk must not claim to have come from the index");
}

void TestAppIntegration::searchAsksTheIndexWhenItCoversTheFolder()
{
    // Indexed first, through the same path the index-search tab uses.
    const int indexRow = m_app->tabs()->openTab(QStringLiteral("mole.indexsearch"));
    QVERIFY(indexRow >= 0);
    auto* indexed = qobject_cast<IndexSearchController*>(m_app->tabs()->controllerAt(indexRow));
    QVERIFY(indexed);
    indexed->scanDirectory(m_tree->rootUri().toString(), QStringLiteral("fixture"));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 20000));

    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    QVERIFY(row >= 0);
    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(search);
    search->setRootUri(m_tree->rootUri().toString());

    QVERIFY2(search->indexCoversRoot(), "the folder that was just scanned is covered");
    QVERIFY2(search->indexNote().contains(QStringLiteral("indexed")),
        "and the form can say so, with how old it is");

    search->setQueryText(QStringLiteral("quarterly"));
    search->start();
    QVERIFY(waitFor([search] { return !search->isRunning(); }, 15000));

    QCOMPARE(search->results()->rowCount(), 1);
    QVERIFY2(search->statusText().contains(QStringLiteral("from the index")),
        "an answer that might be stale has to say where it came from");
}

void TestAppIntegration::turningTheIndexOffForcesAWalk()
{
    const int indexRow = m_app->tabs()->openTab(QStringLiteral("mole.indexsearch"));
    auto* indexed = qobject_cast<IndexSearchController*>(m_app->tabs()->controllerAt(indexRow));
    QVERIFY(indexed);
    indexed->scanDirectory(m_tree->rootUri().toString(), QStringLiteral("fixture"));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 20000));

    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(search);
    search->setRootUri(m_tree->rootUri().toString());
    QVERIFY(search->indexCoversRoot());

    // A file written after the scan: the index cannot know about it, which is
    // exactly the case the toggle exists for.
    QVERIFY(m_tree->writeFile(QStringLiteral("written-after-the-scan.txt"), QByteArray("new")));

    search->setUseIndex(false);
    search->setQueryText(QStringLiteral("written-after"));
    search->start();
    QVERIFY(waitFor([search] { return !search->isRunning(); }, 15000));

    QCOMPARE(search->results()->rowCount(), 1);
    QVERIFY2(!search->statusText().contains(QStringLiteral("from the index")),
        "with the index off, the walk answers and says so");
}

void TestAppIntegration::narrowingResultsDoesNotSearchAgain()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/alpha-notes.txt"), QByteArray("a")));
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/beta-notes.txt"), QByteArray("b")));

    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(search);
    search->setRootUri(m_tree->rootUri().toString());
    search->setQueryText(QStringLiteral("-notes"));
    search->start();
    QVERIFY(waitFor([search] { return !search->isRunning(); }, 15000));

    QCOMPARE(search->results()->rowCount(), 2);
    const QString statusAfterSearch = search->statusText();

    // Narrowing works on the matches already found. No walk, no query -- just less
    // of what is there, which is why it is instant and why it must not restart
    // anything.
    search->results()->setFilterText(QStringLiteral("alpha"));
    QCOMPARE(search->results()->rowCount(), 1);
    QCOMPARE(search->results()->totalCount(), 2);
    QVERIFY2(!search->isRunning(), "narrowing must not start a search");
    QCOMPARE(search->statusText(), statusAfterSearch);

    // And clearing it brings the rest back, since they were never thrown away.
    search->results()->setFilterText(QString());
    QCOMPARE(search->results()->rowCount(), 2);
}

void TestAppIntegration::aSetCanBeBuiltFromWhatTheSearchFound()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/alpha-notes.txt"), QByteArray("a")));
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/beta-notes.txt"), QByteArray("b")));

    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(search);
    search->setRootUri(m_tree->rootUri().toString());
    search->setQueryText(QStringLiteral("-notes"));
    search->start();
    QVERIFY(waitFor([search] { return !search->isRunning(); }, 15000));
    QCOMPARE(search->results()->rowCount(), 2);

    // A snapshot of what is on screen, narrowing included: the rows in front of the
    // user are what "these results" means.
    search->results()->setFilterText(QStringLiteral("beta"));
    QCOMPARE(search->results()->rowCount(), 1);

    const QString id = search->buildSetFromResults(QStringLiteral("Notes to sort"));
    QVERIFY2(!id.isEmpty(), "a set is built from the visible results");

    const FileSet built = m_app->services().sets->set(id);
    QCOMPARE(built.name, QStringLiteral("Notes to sort"));
    QCOMPARE(built.uris.size(), 1);
    QVERIFY(built.uris.first().endsWith(QStringLiteral("beta-notes.txt")));

    // Named for itself when no name is given, rather than refusing or leaving a
    // set called nothing at all.
    search->results()->setFilterText(QString());
    const QString unnamed = search->buildSetFromResults(QString());
    QVERIFY(!unnamed.isEmpty());
    QVERIFY(m_app->services().sets->set(unnamed).name.contains(QStringLiteral("-notes")));
    QCOMPARE(m_app->services().sets->set(unnamed).uris.size(), 2);

    // Nothing to build from is not a set: an empty one would just be litter.
    search->results()->setFilterText(QStringLiteral("nothing matches this"));
    QCOMPARE(search->buildSetFromResults(QStringLiteral("Empty")), QString());
}

void TestAppIntegration::revealingAFileOpensItsFolderWithTheCursorOnIt()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/deep/needle.txt"), QByteArray("x")));
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/deep/haystack.txt"), QByteArray("y")));

    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    BrowserPaneController* pane = browser->activePane();
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 3; }));

    const QString needle = m_tree->rootUri().child(QStringLiteral("reports/deep/needle.txt")).toString();
    m_app->revealFile(needle);

    // The folder it is in, and the cursor on the file itself -- arriving in the
    // right folder with the cursor somewhere else is only half an answer.
    QVERIFY(waitFor([pane] { return pane->currentUri().endsWith(QStringLiteral("/reports/deep")); }, 10000));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }, 10000));
    QVERIFY(waitFor(
        [pane, needle] {
            return pane->currentIndex() >= 0 && pane->files()->uriAt(pane->currentIndex()) == needle;
        },
        10000));

    // Asked for again while already there: no navigation to wait for, and the
    // cursor still lands on what was asked for.
    const QString other = m_tree->rootUri().child(QStringLiteral("reports/deep/haystack.txt")).toString();
    m_app->revealFile(other);
    QCOMPARE(pane->files()->uriAt(pane->currentIndex()), other);
}

void TestAppIntegration::compressActsOnTheCursorWhenNothingIsTicked()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    BrowserPaneController* pane = browser->activePane();
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 3; }));

    const QString notes = m_tree->rootUri().child(QStringLiteral("notes.txt")).toString();
    const QString reports = m_tree->rootUri().child(QStringLiteral("reports")).toString();

    // Ticked wins, and it says so before anything happens.
    pane->files()->setSelected(pane->files()->rowOfUri(notes), true);
    QCOMPARE(m_app->currentTargetsOrCursor(), QStringList { notes });
    QCOMPARE(m_app->compressionSubject(), QStringLiteral("notes.txt"));
    QVERIFY(m_app->suggestedArchiveName(QStringLiteral("zip")).endsWith(QStringLiteral("notes.zip")));

    // Nothing ticked means the row under the cursor -- the rule F5, F8 and analysing
    // already follow, and the one compressing used to be alone in ignoring.
    pane->files()->clearSelection();
    pane->setCurrentIndex(pane->files()->rowOfUri(reports));
    QCOMPARE(m_app->currentTargetsOrCursor(), QStringList { reports });
    QCOMPARE(m_app->compressionSubject(), QStringLiteral("reports"));

    // Several ticked are counted rather than listed, because a dialog is not a
    // listing.
    pane->files()->selectAll();
    QVERIFY(m_app->compressionSubject().contains(QStringLiteral("selected items")));

    // Only zip can carry a password, and the interface is told so rather than
    // offering a box that would be ignored.
    QVERIFY(m_app->formatSupportsPassword(QStringLiteral("zip")));
    QVERIFY(!m_app->formatSupportsPassword(QStringLiteral("tar.gz")));
    QVERIFY(!m_app->formatSupportsPassword(QStringLiteral("tar.xz")));
}

void TestAppIntegration::textPreviewProviderClaimsTextFiles()
{
    FileEntry text;
    text.name = QStringLiteral("notes.txt");
    text.uri = m_tree->rootUri().child(QStringLiteral("notes.txt"));

    IPreviewProvider* provider = m_app->previews()->providerFor(text);
    QVERIFY2(provider, "a .txt file must have a preview provider");
    QCOMPARE(provider->id(), QStringLiteral("mole.preview.text"));

    FileEntry directory;
    directory.isDir = true;
    directory.uri = m_tree->rootUri();
    QVERIFY2(m_app->previews()->providerFor(directory) == nullptr, "directories are not previewable");
}

void TestAppIntegration::archivePluginMountsAZip()
{
    if (!QDir(QStringLiteral(MOLE_TEST_PLUGIN_DIR)).exists())
        QSKIP("archive plugin was not built");

    const QString tar = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (tar.isEmpty())
        QSKIP("tar is not available to build a fixture");

    const QString archivePath = m_tree->absolute(QStringLiteral("bundle.tar.gz"));
    QProcess pack;
    pack.setWorkingDirectory(m_tree->absolute(QStringLiteral("reports")));
    pack.start(tar, { QStringLiteral("-czf"), archivePath, QStringLiteral(".") });
    QVERIFY(pack.waitForFinished(30000));
    QCOMPARE(pack.exitCode(), 0);

    const QString archiveUri = VfsUri::fromLocalPath(archivePath).toString();
    QVERIFY2(m_app->isMountableArchive(archiveUri),
        "the loaded plugin must make .tar.gz mountable without the host knowing about archives");

    const int mountsBefore = m_app->drives()->rowCount();
    const QString root = m_app->openArchive(archiveUri);

    QVERIFY2(!root.isEmpty(), "mounting the archive should have produced a root uri");
    QCOMPARE(m_app->drives()->rowCount(), mountsBefore + 1);
    QVERIFY(root.startsWith(QStringLiteral("archive://")));

    // Mounting the same archive again must reuse the existing drive.
    QCOMPARE(m_app->openArchive(archiveUri), root);
    QCOMPARE(m_app->drives()->rowCount(), mountsBefore + 1);
}

void TestAppIntegration::dualPaneIsItsOwnFeature()
{
    // The two browsing contexts are separate entries in the new-tab menu but
    // the same controller underneath.
    IFeature* commander = m_app->features()->feature(QStringLiteral("mole.commander"));
    QVERIFY2(commander, "dual pane must be offered as its own context");
    QVERIFY(commander->id() != QStringLiteral("mole.browser"));

    const int row = m_app->tabs()->openTab(QStringLiteral("mole.commander"));
    QVERIFY(row >= 0);

    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(browser);
    QVERIFY2(browser->splitEnabled(), "a dual-pane tab must open already split");

    // And the plain browser tab does not.
    auto* single = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(single);
    QVERIFY(!single->splitEnabled());

    // Switching modes keeps both panes' locations, so it is a view change and
    // not a reset.
    const QString before = browser->right()->currentUri();
    browser->setViewMode(BrowserController::ViewMode::Single);
    browser->setViewMode(BrowserController::ViewMode::Dual);
    QCOMPARE(browser->right()->currentUri(), before);
}

void TestAppIntegration::copyBetweenPanesMovesRealFiles()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.commander"));
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(browser);

    browser->left()->navigateTo(m_tree->rootUri().toString());
    browser->right()->navigateTo(m_tree->rootUri().child(QStringLiteral("empty")).toString());
    QVERIFY(waitFor([browser] {
        return !browser->left()->isLoading() && !browser->right()->isLoading()
            && browser->left()->files()->rowCount() > 0;
    }));

    // Tick notes.txt in the left pane.
    const int noteRow
        = browser->left()->files()->rowOfUri(m_tree->rootUri().child(QStringLiteral("notes.txt")).toString());
    QVERIFY(noteRow >= 0);
    browser->left()->files()->setSelected(noteRow, true);

    QVERIFY(browser->canTransfer());
    QVERIFY(browser->transferSummary().contains(QStringLiteral("notes.txt")));

    browser->copyToOtherPane();
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 10000));

    QVERIFY2(QFile::exists(m_tree->absolute(QStringLiteral("empty/notes.txt"))),
        "the file must actually be on disk in the target folder");
    QVERIFY2(
        QFile::exists(m_tree->absolute(QStringLiteral("notes.txt"))), "a copy leaves the original alone");

    // The target pane refreshes itself through the event bus.
    QVERIFY(waitFor([browser] { return browser->right()->files()->rowCount() == 1; }));
}

void TestAppIntegration::moveBetweenPanesRemovesTheSource()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.commander"));
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(browser);

    browser->left()->navigateTo(m_tree->rootUri().child(QStringLiteral("reports")).toString());
    browser->right()->navigateTo(m_tree->rootUri().child(QStringLiteral("empty")).toString());
    QVERIFY(waitFor([browser] {
        return !browser->left()->isLoading() && !browser->right()->isLoading()
            && browser->left()->files()->rowCount() == 1;
    }));

    browser->left()->files()->selectAll();
    browser->moveToOtherPane();
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 10000));

    QVERIFY(QFile::exists(m_tree->absolute(QStringLiteral("empty/quarterly.txt"))));
    QVERIFY2(!QFile::exists(m_tree->absolute(QStringLiteral("reports/quarterly.txt"))),
        "a move must remove the source once the copy landed");
}

void TestAppIntegration::transferRefusesWhenBothPanesShowTheSameFolder()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.commander"));
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(browser);
    QVERIFY(waitFor(
        [browser] { return !browser->left()->isLoading() && browser->left()->files()->rowCount() > 0; }));

    // Both panes start on the same folder; copying onto itself is a mistake,
    // not an operation.
    browser->left()->files()->selectAll();
    QSignalSpy failed(browser, &BrowserController::operationFailed);
    browser->copyToOtherPane();

    QCOMPARE(failed.count(), 1);
    QCOMPARE(m_app->tasks()->activeCount(), 0);
}

void TestAppIntegration::transferIsUnavailableInSinglePane()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(!browser->splitEnabled());
    QVERIFY(waitFor([browser] { return !browser->left()->isLoading(); }));

    browser->left()->files()->selectAll();
    QVERIFY2(!browser->canTransfer(), "there is nowhere to copy to with one pane");

    QSignalSpy failed(browser, &BrowserController::operationFailed);
    browser->copyToOtherPane();
    QCOMPARE(failed.count(), 1);
}

void TestAppIntegration::transferAvailabilityIsNotified()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }));

    // As an invokable this was evaluated once by QML and never again, so Copy
    // stayed greyed out after switching to dual pane. A property without a
    // change signal is the same bug with better syntax, so the signal is what
    // the test watches.
    QSignalSpy availability(browser, &BrowserController::transferAvailabilityChanged);

    QVERIFY(!browser->canTransfer());
    browser->setViewMode(BrowserController::ViewMode::Dual);
    browser->activePane()->setCurrentIndex(0);
    browser->otherPane()->navigateTo(m_tree->rootUri().child(QStringLiteral("empty")).toString());
    QVERIFY(waitFor([browser] { return browser->canTransfer(); }));

    QVERIFY2(availability.count() > 0, "the interface has to be told, not asked");
}

void TestAppIntegration::tellsWhatIsAlreadyKnownAboutTheFolder()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }));

    // Nothing has been done to this folder yet, so the strip says so rather
    // than saying nothing.
    QVERIFY(!browser->hasReport());
    QVERIFY(!browser->isIndexed());
    QCOMPARE(browser->alertCount(), 0);

    // An alert added anywhere shows up here without navigating away and back.
    AlertRule rule;
    rule.id = QStringLiteral("watch");
    rule.label = QStringLiteral("Watch");
    rule.targetUri = browser->activePane()->currentUri();
    rule.state = AlertState::Triggered;
    QVERIFY(m_app->alerts()->put(rule));

    QVERIFY(waitFor([browser] { return browser->alertCount() == 1; }));
    QCOMPARE(browser->triggeredAlertCount(), 1);
}

void TestAppIntegration::openExternallyGoesThroughTheLauncher()
{
    // Swap the desktop hand-off for a recorder so the suite never actually
    // spawns a text editor.
    QStringList opened;
    m_app->launcher()->setOpenHook([&opened](const QString& path) {
        opened.append(path);
        return true;
    });

    m_app->openExternally(m_tree->rootUri().child(QStringLiteral("notes.txt")).toString());

    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first(), m_tree->absolute(QStringLiteral("notes.txt")));
}

namespace {

QVariantList menuSection(const QVariantList& menu, const QString& title)
{
    for (const QVariant& entry : menu) {
        const QVariantMap section = entry.toMap();
        if (section.value(QStringLiteral("title")).toString() == title)
            return section.value(QStringLiteral("actions")).toList();
    }
    return {};
}

QVariantMap menuEntry(const QVariantList& menu, const QString& id)
{
    for (const QVariant& sectionEntry : menu) {
        const QVariantList actions = sectionEntry.toMap().value(QStringLiteral("actions")).toList();
        for (const QVariant& action : actions) {
            if (action.toMap().value(QStringLiteral("id")).toString() == id)
                return action.toMap();
        }
    }
    return {};
}

} // namespace

void TestAppIntegration::menuHasTheClassicSections()
{
    const QVariantList menu = m_app->buildMenu();
    QStringList titles;
    for (const QVariant& section : menu)
        titles.append(section.toMap().value(QStringLiteral("title")).toString());

    QCOMPARE(titles,
        QStringList({ QStringLiteral("File"), QStringLiteral("View"), QStringLiteral("Operations"),
            QStringLiteral("Workflows"), QStringLiteral("Bookmarks"), QStringLiteral("Help") }));

    // And the split holds for the real entries, not only for a registry fed by a
    // test. Doing something to the files in front of you on one side, being handed
    // a tool to work in on the other -- ADR-0003 has the rule and the cases that
    // sound like both.
    const auto sectionOf = [&menu](const QString& id) {
        for (const QVariant& sectionEntry : menu) {
            const QVariantMap section = sectionEntry.toMap();
            for (const QVariant& action : section.value(QStringLiteral("actions")).toList()) {
                if (action.toMap().value(QStringLiteral("id")).toString() == id)
                    return section.value(QStringLiteral("title")).toString();
            }
        }
        return QString();
    };

    QCOMPARE(sectionOf(QStringLiteral("mole.tools.preview")), QStringLiteral("Operations"));
    QCOMPARE(sectionOf(QStringLiteral("mole.tools.indexFolder")), QStringLiteral("Operations"));
    QCOMPARE(sectionOf(QStringLiteral("mole.tools.addToSet")), QStringLiteral("Operations"));
    QCOMPARE(sectionOf(QStringLiteral("mole.tools.terminal")), QStringLiteral("Operations"));

    QCOMPARE(sectionOf(QStringLiteral("mole.tools.bulkRename")), QStringLiteral("Workflows"));
    QCOMPARE(sectionOf(QStringLiteral("mole.tools.duplicates")), QStringLiteral("Workflows"));
    QCOMPARE(sectionOf(QStringLiteral("mole.tools.analyse")), QStringLiteral("Workflows"));
    QCOMPARE(sectionOf(QStringLiteral("mole.tools.sync")), QStringLiteral("Workflows"));
    QCOMPARE(sectionOf(QStringLiteral("mole.tools.reports")), QStringLiteral("Workflows"));
    QCOMPARE(sectionOf(QStringLiteral("mole.tools.alerts")), QStringLiteral("Workflows"));
    QCOMPARE(sectionOf(QStringLiteral("mole.tools.automation")), QStringLiteral("Workflows"));
}

void TestAppIntegration::theTypeScaleIsOrderedAndAboveTheFloor()
{
    // Sizes live here so that a listing, a preview and a form line up instead of
    // each picking a number. What a test can hold is the shape of the scale: the
    // steps in order, and nothing so small that it stops being readable.
    QVERIFY(m_app->smallTextSize() < m_app->secondaryTextSize());
    QVERIFY(m_app->secondaryTextSize() < m_app->textSize());
    QVERIFY(m_app->textSize() < m_app->headingSize());

    // The floor. Text below about eleven pixels is a squint, and the listing used
    // to reach down to nine.
    QVERIFY2(m_app->smallTextSize() >= 11, "nothing in the interface may be smaller than this");

    // Code sits level with prose, and never over it. Level is the decision; the
    // assertion is what stops it drifting above prose later.
    QVERIFY(m_app->monospaceSize() <= m_app->textSize());

    // Derived, so raising the text size cannot crop a row.
    QVERIFY(m_app->listRowHeight() > m_app->textSize() * 2);
}

void TestAppIntegration::menuOffersOneNewTabEntryPerFeature()
{
    // The menu is generated from the feature registry, so a plugin that adds a
    // tab kind appears here without the shell being edited.
    const QList<IFeature*> features = m_app->features()->features();
    QVERIFY(!features.isEmpty());

    const QVariantList menu = m_app->buildMenu();
    for (IFeature* feature : features) {
        const QString id = QStringLiteral("mole.file.newTab.%1").arg(feature->id());
        QVERIFY2(!menuEntry(menu, id).isEmpty(),
            qPrintable(QStringLiteral("no menu entry for feature %1").arg(feature->id())));
    }

    // Plus Drives, Close tab and Quit.
    QCOMPARE(menuSection(menu, QStringLiteral("File")).size(), features.size() + 3);
    QVERIFY(!menuEntry(menu, QStringLiteral("mole.file.drives")).isEmpty());
}

void TestAppIntegration::menuEntryOpensTheTab()
{
    const int before = m_app->tabs()->rowCount();
    QVERIFY(m_app->triggerAction(QStringLiteral("mole.file.newTab.mole.commander")));

    QCOMPARE(m_app->tabs()->rowCount(), before + 1);
    QCOMPARE(m_app->tabs()->index(m_app->tabs()->currentIndex(), 0).data(TabsModel::FeatureIdRole).toString(),
        QStringLiteral("mole.commander"));

    QVERIFY2(!m_app->triggerAction(QStringLiteral("nope")), "unknown ids must be ignored");
}

void TestAppIntegration::viewMenuReflectsAndTogglesTheCurrentTab()
{
    // Tab 0 is a single-pane browser.
    QVariantMap dual = menuEntry(m_app->buildMenu(), QStringLiteral("mole.view.dualPane"));
    QVERIFY(dual.value(QStringLiteral("checkable")).toBool());
    QVERIFY(dual.value(QStringLiteral("enabled")).toBool());
    QVERIFY(!dual.value(QStringLiteral("checked")).toBool());

    QVERIFY(m_app->triggerAction(QStringLiteral("mole.view.dualPane")));

    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY2(browser->splitEnabled(), "the View entry must act on the current tab");

    // Rebuilding the menu has to show the new state, not a cached one.
    dual = menuEntry(m_app->buildMenu(), QStringLiteral("mole.view.dualPane"));
    QVERIFY(dual.value(QStringLiteral("checked")).toBool());
}

void TestAppIntegration::viewMenuOffersThreeExclusiveLayouts()
{
    // Exactly one is ticked at a time: they are alternatives, not switches.
    const auto tickedLayout = [this]() -> QString {
        const QVariantList menu = m_app->buildMenu();
        QStringList ticked;
        for (const QString& id : { QStringLiteral("mole.view.singlePane"),
                 QStringLiteral("mole.view.dualPane"), QStringLiteral("mole.view.gridView") }) {
            if (menuEntry(menu, id).value(QStringLiteral("checked")).toBool())
                ticked.append(id);
        }
        return ticked.size() == 1 ? ticked.first() : QStringLiteral("<%1 ticked>").arg(ticked.size());
    };

    QCOMPARE(tickedLayout(), QStringLiteral("mole.view.singlePane"));

    QVERIFY(m_app->triggerAction(QStringLiteral("mole.view.gridView")));
    QCOMPARE(tickedLayout(), QStringLiteral("mole.view.gridView"));

    QVERIFY(m_app->triggerAction(QStringLiteral("mole.view.dualPane")));
    QCOMPARE(tickedLayout(), QStringLiteral("mole.view.dualPane"));

    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(browser->splitEnabled());
    QVERIFY(!browser->gridEnabled());
}

void TestAppIntegration::menuEntriesGreyOutWhenTheyDoNotApply()
{
    // A search tab has no panes, so the pane-related entries must go grey
    // rather than disappear or misfire. The shell learns this by asking the
    // controller for a property, not by knowing what kind of tab it is.
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    QVERIFY(row >= 0);
    m_app->tabs()->setCurrentIndex(row);

    const QVariantList menu = m_app->buildMenu();
    QVERIFY(!menuEntry(menu, QStringLiteral("mole.view.dualPane")).value(QStringLiteral("enabled")).toBool());
    QVERIFY(
        !menuEntry(menu, QStringLiteral("mole.tools.indexFolder")).value(QStringLiteral("enabled")).toBool());

    // And a disabled entry does nothing if it is reached anyway. Counting
    // queued tasks rather than running ones: the browser tab opened in init()
    // may still be listing its directory, which is unrelated.
    const int tasksBefore = m_app->tasks()->rowCount();
    QVERIFY(!m_app->triggerAction(QStringLiteral("mole.tools.indexFolder")));
    QCOMPARE(m_app->tasks()->rowCount(), tasksBefore);

    // Close tab stays available regardless of what the tab is.
    QVERIFY(menuEntry(menu, QStringLiteral("mole.file.closeTab")).value(QStringLiteral("enabled")).toBool());
}

void TestAppIntegration::manyTabsOpenAndCloseWithoutLeaking()
{
    QList<QPointer<QObject>> controllers;
    for (int i = 0; i < 12; ++i) {
        const int row = m_app->tabs()->openTab(QStringLiteral("mole.browser"));
        QVERIFY(row >= 0);
        controllers.append(m_app->tabs()->controllerAt(row));
    }

    while (m_app->tabs()->rowCount() > 0)
        m_app->tabs()->closeTab(0);

    drainEvents();
    QVERIFY(waitFor([&controllers] {
        for (const QPointer<QObject>& controller : controllers) {
            if (!controller.isNull())
                return false;
        }
        return true;
    }));
}

void TestAppIntegration::shutsDownCleanlyWithWorkInFlight()
{
    // Tearing the application down while a scan is still writing to the index
    // is exactly what closing the window does.
    m_app->queueScan(VfsUri::fromLocalPath(QDir::rootPath()).toString(), QStringLiteral("root"));
    m_app.reset();
    QVERIFY(true);
}

BrowserController* TestAppIntegration::readyToTransfer(const QString& destination)
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    if (!browser)
        return nullptr;
    browser->setViewMode(BrowserController::ViewMode::Dual);
    browser->otherPane()->navigateTo(m_tree->rootUri().child(destination).toString());
    if (!waitFor([browser] { return !browser->otherPane()->isLoading(); }))
        return nullptr;
    if (!waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }))
        return nullptr;
    return browser;
}

void TestAppIntegration::thePlanNamesWhatWouldBeOverwritten()
{
    // The same name on both sides, so a transfer would replace something.
    QVERIFY(m_tree->writeFile(QStringLiteral("empty/notes.txt"), QByteArray("theirs")));

    auto* browser = readyToTransfer(QStringLiteral("empty"));
    QVERIFY(browser);

    const int row = browser->activePane()->files()->rowOfUri(
        m_tree->rootUri().child(QStringLiteral("notes.txt")).toString());
    QVERIFY(row >= 0);
    browser->activePane()->setCurrentIndex(row);

    const QVariantMap plan = browser->transferPlan();
    QCOMPARE(plan.value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(plan.value(QStringLiteral("singleName")).toString(), QStringLiteral("notes.txt"));

    // Named before anything happens. Discovering an overwrite afterwards is
    // discovering it too late.
    QCOMPARE(
        plan.value(QStringLiteral("collisions")).toStringList(), QStringList { QStringLiteral("notes.txt") });
    QVERIFY(!plan.value(QStringLiteral("sizeText")).toString().isEmpty());
}

void TestAppIntegration::aSingleItemCanArriveUnderANewName()
{
    auto* browser = readyToTransfer(QStringLiteral("empty"));
    QVERIFY(browser);

    const int row = browser->activePane()->files()->rowOfUri(
        m_tree->rootUri().child(QStringLiteral("notes.txt")).toString());
    QVERIFY(row >= 0);
    browser->activePane()->setCurrentIndex(row);

    browser->runTransfer(false, QStringLiteral("renamed.txt"), QStringLiteral("stop"));
    QVERIFY(waitFor(
        [this] { return QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("empty/renamed.txt"))); },
        10000));

    // Under the new name only -- not both.
    QVERIFY(!QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("empty/notes.txt"))));
    QVERIFY(QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("notes.txt"))));
}

void TestAppIntegration::skipLeavesTheExistingFileAlone()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("empty/notes.txt"), QByteArray("theirs")));

    auto* browser = readyToTransfer(QStringLiteral("empty"));
    QVERIFY(browser);
    const int row = browser->activePane()->files()->rowOfUri(
        m_tree->rootUri().child(QStringLiteral("notes.txt")).toString());
    QVERIFY(row >= 0);
    browser->activePane()->setCurrentIndex(row);

    browser->runTransfer(false, QString(), QStringLiteral("skip"));
    QVERIFY(waitFor([this] { return m_app->tasks()->activeCount() == 0; }, 10000));
    drainEvents();

    QFile kept(QDir(m_tree->path()).filePath(QStringLiteral("empty/notes.txt")));
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArray("theirs"));
}

void TestAppIntegration::overwriteReplacesIt()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("empty/notes.txt"), QByteArray("theirs")));

    auto* browser = readyToTransfer(QStringLiteral("empty"));
    QVERIFY(browser);
    const int row = browser->activePane()->files()->rowOfUri(
        m_tree->rootUri().child(QStringLiteral("notes.txt")).toString());
    QVERIFY(row >= 0);
    browser->activePane()->setCurrentIndex(row);

    browser->runTransfer(false, QString(), QStringLiteral("overwrite"));
    QVERIFY(waitFor(
        [this] {
            QFile file(QDir(m_tree->path()).filePath(QStringLiteral("empty/notes.txt")));
            return file.open(QIODevice::ReadOnly) && file.readAll() == QByteArray("some text to preview");
        },
        10000));
}

void TestAppIntegration::reportsWhoMayDoWhatHere()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(browser);
    QVERIFY(waitFor([browser] { return browser->activePane()->files()->rowCount() > 0; }));

    // Answered through a task, so it arrives a moment after the listing.
    QVERIFY(waitFor([browser] { return browser->isAccessKnown(); }, 10000));

    // A temporary directory the test just created is writable, and on this
    // platform the native form is the one worth showing.
    QCOMPARE(browser->accessText().size(), 9);
    QVERIFY(browser->accessText().startsWith(QLatin1Char('r')));
    QVERIFY(!browser->isReadOnlyHere());

    // The detail spells out the questions rather than leaving nine characters
    // to be decoded -- which is also what a drive with no mode bits will show.
    QVERIFY(browser->accessDetail().contains(QStringLiteral("can read")));
    QVERIFY(browser->accessDetail().contains(QStringLiteral("can write")));
}

MOLE_TEST_MAIN(TestAppIntegration)
#include "tst_AppIntegration.moc"
