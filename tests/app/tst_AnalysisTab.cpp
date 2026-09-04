#include "plugins/builtin/AnalysisFeature.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/ReportsFeature.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/analysis/AnalysisStore.h"
#include "core/events/EventBus.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The analysis tab as the user meets it: one or more folders, a report each,
/// a history each, and a filter over the breakdown.
class TestAnalysisTab : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void analysesTheCurrentFolderWhenNothingIsSelected();
    void analysesEverySelectedFolder();
    void keepsTargetsIndependent();
    void filtersTheBreakdown();
    void filterTotalsFollowTheFilter();
    void ranksByCountWhenAsked();
    void buildsHistoryAndDiffsAgainstIt();
    void remembersItsFoldersAcrossRestart();
    void openingAReportDoesNotRescan();
    void theLibraryListsEverySavedReport();
    void theLibraryShowsHowEachRunChanged();
    void forgettingAFolderRemovesItFromTheLibrary();
    void aReportFiledWithNoDirectoryChangingAppearsAtOnce();
    void theLibraryDoesNotReadTheStoreOnEveryDirectoryChange();
    void theGuardItselfNoticesADirectRead();
    void nothingTheInterfaceDoesReadsTheStoreOnTheDrawingThread();

private:
    AnalysisTabController* openAnalysis(const QStringList& uris);
    ReportsController* openLibrary();
    void waitForReports(AnalysisTabController* tab);

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
};

void TestAnalysisTab::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

void TestAnalysisTab::init()
{
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));
    QDir(QString::fromLocal8Bit(qgetenv("MOLE_ANALYSIS_PATH"))).removeRecursively();

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    // Two folders with clearly different make-up, so a mix-up would be obvious.
    QVERIFY(m_tree->writeFile(QStringLiteral("media/film.mkv"), QByteArray(9000, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("media/clip.mkv"), QByteArray(1000, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("media/cover.jpg"), QByteArray(500, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("docs/a.txt"), QByteArray(100, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("docs/b.txt"), QByteArray(200, 'x')));

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));
}

void TestAnalysisTab::cleanup()
{
    m_app.reset();
    m_tree.reset();
}

AnalysisTabController* TestAnalysisTab::openAnalysis(const QStringList& uris)
{
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.analysis"));
    if (row < 0)
        return nullptr;
    auto* tab = qobject_cast<AnalysisTabController*>(m_app->tabs()->controllerAt(row));
    if (tab)
        tab->setTargets(uris);
    return tab;
}

void TestAnalysisTab::waitForReports(AnalysisTabController* tab)
{
    // Not merely "a report appeared": the report is published before the task
    // reaches its terminal state, and a refresh requested in that window is
    // silently dropped because the target still counts itself busy. Waiting on
    // both is what "the run is complete" actually means.
    QVERIFY(waitFor(
        [tab] {
            for (int i = 0; i < tab->targetCount(); ++i) {
                tab->setCurrentIndex(i);
                if (!tab->current() || !tab->current()->hasReport() || tab->current()->isBusy())
                    return false;
            }
            return true;
        },
        30000));
    tab->setCurrentIndex(0);
}

void TestAnalysisTab::analysesTheCurrentFolderWhenNothingIsSelected()
{
    // The browser tab is sitting on the fixture root with nothing ticked.
    m_app->analyseSelection();

    auto* tab = qobject_cast<AnalysisTabController*>(m_app->tabs()->currentController());
    QVERIFY2(tab, "Ctrl+Shift+A must open an analysis tab");
    QCOMPARE(tab->targetCount(), 1);
    QCOMPARE(tab->current()->rootUri(), m_tree->rootUri().toString());

    waitForReports(tab);
    const QVariantMap headline = tab->current()->headline();
    QCOMPARE(headline.value(QStringLiteral("files")).toLongLong(), 5);
    QCOMPARE(headline.value(QStringLiteral("bytes")).toLongLong(), 10800);
}

void TestAnalysisTab::analysesEverySelectedFolder()
{
    AnalysisTabController* tab = openAnalysis({ m_tree->rootUri().child(QStringLiteral("media")).toString(),
        m_tree->rootUri().child(QStringLiteral("docs")).toString() });
    QVERIFY(tab);
    QCOMPARE(tab->targetCount(), 2);

    // Both are walked, not just the visible one: the user asked about both.
    waitForReports(tab);

    tab->setCurrentIndex(0);
    QCOMPARE(tab->current()->headline().value(QStringLiteral("files")).toLongLong(), 3);
    tab->setCurrentIndex(1);
    QCOMPARE(tab->current()->headline().value(QStringLiteral("files")).toLongLong(), 2);
}

void TestAnalysisTab::keepsTargetsIndependent()
{
    AnalysisTabController* tab = openAnalysis({ m_tree->rootUri().child(QStringLiteral("media")).toString(),
        m_tree->rootUri().child(QStringLiteral("docs")).toString() });
    QVERIFY(tab);
    waitForReports(tab);

    // Merging two folders into one report would answer a question nobody
    // asked, so each keeps its own breakdown.
    tab->setCurrentIndex(0);
    QCOMPARE(tab->current()->extensions()->index(0, 0).data(BreakdownModel::ExtensionRole).toString(),
        QStringLiteral("mkv"));

    tab->setCurrentIndex(1);
    QCOMPARE(tab->current()->extensions()->index(0, 0).data(BreakdownModel::ExtensionRole).toString(),
        QStringLiteral("txt"));
}

void TestAnalysisTab::filtersTheBreakdown()
{
    AnalysisTabController* tab = openAnalysis({ m_tree->rootUri().toString() });
    QVERIFY(tab);
    waitForReports(tab);

    BreakdownModel* extensions = tab->current()->extensions();
    QCOMPARE(extensions->rowCount(), 3); // mkv, jpg, txt

    extensions->setFilterText(QStringLiteral("mk"));
    QCOMPARE(extensions->rowCount(), 1);
    QCOMPARE(extensions->hiddenRows(), 2);

    extensions->clearFilters();
    QCOMPARE(extensions->rowCount(), 3);

    // Hiding the small stuff is how you find what is actually filling a disk.
    extensions->setMinimumBytes(1000);
    QCOMPARE(extensions->rowCount(), 1);
    QCOMPARE(extensions->index(0, 0).data(BreakdownModel::ExtensionRole).toString(), QStringLiteral("mkv"));
}

void TestAnalysisTab::filterTotalsFollowTheFilter()
{
    AnalysisTabController* tab = openAnalysis({ m_tree->rootUri().toString() });
    QVERIFY(tab);
    waitForReports(tab);

    BreakdownModel* extensions = tab->current()->extensions();
    QCOMPARE(extensions->visibleBytes(), 10800);
    QCOMPARE(extensions->visibleCount(), 5);

    // The totals describe what is on screen, or the numbers would contradict
    // the chart above them.
    extensions->setFilterText(QStringLiteral("txt"));
    QCOMPARE(extensions->visibleBytes(), 300);
    QCOMPARE(extensions->visibleCount(), 2);
    QCOMPARE(extensions->index(0, 0).data(BreakdownModel::ShareRole).toDouble(), 1.0);
}

void TestAnalysisTab::ranksByCountWhenAsked()
{
    AnalysisTabController* tab = openAnalysis({ m_tree->rootUri().toString() });
    QVERIFY(tab);
    waitForReports(tab);

    BreakdownModel* extensions = tab->current()->extensions();
    QCOMPARE(extensions->index(0, 0).data(BreakdownModel::ExtensionRole).toString(),
        QStringLiteral("mkv")); // biggest by size

    extensions->setByCount(true);
    // Two .txt and two .mkv; the ranking must follow the measure on show.
    QCOMPARE(extensions->index(0, 0).data(BreakdownModel::CountRole).toLongLong(), 2);
    QCOMPARE(extensions->peak(), 2);
}

void TestAnalysisTab::buildsHistoryAndDiffsAgainstIt()
{
    AnalysisTabController* tab = openAnalysis({ m_tree->rootUri().toString() });
    QVERIFY(tab);
    waitForReports(tab);
    QCOMPARE(tab->current()->history().size(), 1);

    // Something big arrives between runs.
    QVERIFY(m_tree->writeFile(QStringLiteral("media/huge.mkv"), QByteArray(50000, 'x')));
    QVERIFY(QFile::remove(m_tree->absolute(QStringLiteral("docs/a.txt"))));

    tab->refreshAll();
    QVERIFY(waitFor([tab] { return tab->current()->history().size() == 2; }, 20000));

    // The reason reports are kept at all: the second run is compared with the
    // first without the user asking.
    QVERIFY2(tab->current()->hasDiff(), "a second run must diff against the first");

    const QVariantMap diff = tab->current()->diffHeadline();
    QCOMPARE(diff.value(QStringLiteral("filesDelta")).toLongLong(), 0); // one added, one removed
    QCOMPARE(diff.value(QStringLiteral("bytesDelta")).toLongLong(), 49900);
    QVERIFY(diff.value(QStringLiteral("grew")).toBool());

    const QVariantList rows = tab->current()->diffRows();
    QVERIFY(!rows.isEmpty());
    QCOMPARE(rows.first().toMap().value(QStringLiteral("extension")).toString(), QStringLiteral("mkv"));
    QVERIFY(rows.first().toMap().value(QStringLiteral("grew")).toBool());
}

void TestAnalysisTab::remembersItsFoldersAcrossRestart()
{
    const QString media = m_tree->rootUri().child(QStringLiteral("media")).toString();
    const QString docs = m_tree->rootUri().child(QStringLiteral("docs")).toString();

    AnalysisTabController* tab = openAnalysis({ media, docs });
    QVERIFY(tab);
    waitForReports(tab);
    m_app->saveSessionNow();

    m_app.reset();
    drainEvents();

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));

    AnalysisTabController* restored = nullptr;
    for (int row = 0; row < m_app->tabs()->rowCount(); ++row) {
        if (auto* candidate = qobject_cast<AnalysisTabController*>(m_app->tabs()->controllerAt(row))) {
            restored = candidate;
        }
    }
    QVERIFY2(restored, "the analysis tab must come back");
    QCOMPARE(restored->targetCount(), 2);

    // Restoring shows the stored report; it does not re-walk, which could be
    // minutes of disk on startup.
    restored->setCurrentIndex(0);
    QVERIFY2(restored->current()->hasReport(), "the stored report comes back without a new walk");
    QVERIFY(!restored->current()->isBusy());
    QCOMPARE(restored->current()->headline().value(QStringLiteral("files")).toLongLong(), 3);
}

ReportsController* TestAnalysisTab::openLibrary()
{
    const int row = m_app->tabs()->openTab(QStringLiteral("core.reports"));
    return row < 0 ? nullptr : qobject_cast<ReportsController*>(m_app->tabs()->controllerAt(row));
}

void TestAnalysisTab::theLibraryListsEverySavedReport()
{
    const QString media = m_tree->rootUri().child(QStringLiteral("media")).toString();
    const QString docs = m_tree->rootUri().child(QStringLiteral("docs")).toString();

    AnalysisTabController* tab = openAnalysis({ media, docs });
    QVERIFY(tab);
    waitForReports(tab);

    ReportsController* library = openLibrary();
    QVERIFY(library);
    QCOMPARE(library->folderCount(), 2);
    QCOMPARE(library->reportCount(), 2);

    const QVariantList folders = library->folders();
    QCOMPARE(folders.size(), 2);

    // Something is selected on arrival: a list where nothing is picked shows an
    // empty right-hand pane and looks broken.
    QVERIFY(!library->selectedRoot().isEmpty());
    QCOMPARE(library->runs().size(), 1);

    // The filter narrows by path, which is the only thing that distinguishes
    // two folders with the same name.
    library->setFilter(QStringLiteral("media"));
    QCOMPARE(library->folders().size(), 1);
    QCOMPARE(library->folders().first().toMap().value(QStringLiteral("label")).toString(),
        QStringLiteral("media"));
}

void TestAnalysisTab::theLibraryShowsHowEachRunChanged()
{
    const QString media = m_tree->rootUri().child(QStringLiteral("media")).toString();

    AnalysisTabController* tab = openAnalysis({ media });
    QVERIFY(tab);
    waitForReports(tab);

    // A second run after the folder grew, so the two runs differ.
    QVERIFY(m_tree->writeFile(QStringLiteral("media/extra.mkv"), QByteArray(4000, 'z')));
    tab->refreshAll();
    QVERIFY(waitFor([tab] { return tab->current() && tab->current()->history().size() == 2; }, 20000));

    ReportsController* library = openLibrary();
    QVERIFY(library);
    library->setSelectedRoot(media);

    const QVariantList runs = library->runs();
    QCOMPARE(runs.size(), 2);

    // Newest first, and the newest carries the comparison: "one report says how
    // big a folder is, a series says what is happening to it".
    QCOMPARE(runs.first().toMap().value(QStringLiteral("grew")).toBool(), true);
    QVERIFY(
        runs.first().toMap().value(QStringLiteral("changeText")).toString().startsWith(QStringLiteral("+")));
    // The oldest has nothing before it to compare against.
    QCOMPARE(runs.last().toMap().value(QStringLiteral("changeText")).toString(), QString());
}

void TestAnalysisTab::aReportFiledWithNoDirectoryChangingAppearsAtOnce()
{
    // The Reports tab rebuilt on EventBus::directoryChanged and on nothing else,
    // so the comment "a run filed by the scheduler appears here without the user
    // reopening the tab" was true only when some directory also happened to
    // change. AnalysisJob::reportStored had no listener anywhere in the tree.
    // See MOLE-380.
    const QString media = m_tree->rootUri().child(QStringLiteral("media")).toString();

    ReportsController* library = openLibrary();
    QVERIFY(library);
    QCOMPARE(library->folderCount(), 0);

    // Filed straight into the store, which is what the scheduler does. No
    // directory is touched and no bus event is posted.
    AnalysisStore* store = m_app->services().reports;
    QVERIFY(store);
    AnalysisReport report;
    report.id = QStringLiteral("20260101-000000-000");
    report.rootUri = media;
    report.label = QStringLiteral("media");
    report.createdAt = QDateTime::currentDateTime();
    report.fileCount = 3;
    report.totalBytes = 10500;
    QVERIFY(store->save(report));

    QVERIFY2(waitFor([library] { return library->folderCount() == 1; }, 5000),
        "a filed report did not reach the Reports tab");
    QCOMPARE(library->reportCount(), 1);
    QCOMPARE(library->selectedRoot(), media);
}

void TestAnalysisTab::theLibraryDoesNotReadTheStoreOnEveryDirectoryChange()
{
    // history() opens and parses every report of a folder, and this happened on
    // every directoryChanged -- a copy, a delete, a rename, a refresh, anywhere.
    // With fifty reports of a large tree that is megabytes of JSON on the thread
    // that draws, per bus event. Counted by watching the files: a read touches
    // them, and a cache does not.
    const QString media = m_tree->rootUri().child(QStringLiteral("media")).toString();

    AnalysisTabController* tab = openAnalysis({ media });
    QVERIFY(tab);
    waitForReports(tab);

    ReportsController* library = openLibrary();
    QVERIFY(library);
    QCOMPARE(library->folderCount(), 1);

    // Make the store's own files unreadable-looking by taking them away. A read
    // would now find nothing; a cache still answers. Cruder than counting opens
    // and it needs no instrumentation in shipped code.
    const QString analysisDir = QString::fromLocal8Bit(qgetenv("MOLE_ANALYSIS_PATH"));
    QVERIFY(!analysisDir.isEmpty());
    QVERIFY(QDir(analysisDir).removeRecursively());

    // Fifty directory changes, which used to be fifty full re-reads.
    for (int i = 0; i < 50; ++i)
        m_app->services().events->postDirectoryChanged(m_tree->rootUri());
    drainEvents();

    QVERIFY2(library->folderCount() == 1, "the library went back to disk on a directory change");

    // And pressing refresh does go back to disk, which is the escape hatch.
    library->refresh();
    QCOMPARE(library->folderCount(), 0);
}

void TestAnalysisTab::theGuardItselfNoticesADirectRead()
{
    // A guard that cannot fire is not a guard, so this is the case that proves
    // the one below is worth reading. The same shape tst_IndexOffTheDrawingThread
    // opens with. See MOLE-380.
    AnalysisStore* store = m_app->services().reports;
    QVERIFY(store);

    const QString media = m_tree->rootUri().child(QStringLiteral("media")).toString();
    AnalysisReport report;
    report.id = QStringLiteral("20260101-000000-000");
    report.rootUri = media;
    report.createdAt = QDateTime::currentDateTime();
    QVERIFY(store->save(report));
    // save() invalidated what was kept, so the next ask goes to disk.

    CapturedWarnings warnings;
    (void)store->history(media);
    QVERIFY2(warnings.contains(QStringLiteral("Report store read on the thread that draws")),
        qPrintable(QStringLiteral("the guard said nothing: %1").arg(warnings.joined())));
}

void TestAnalysisTab::nothingTheInterfaceDoesReadsTheStoreOnTheDrawingThread()
{
    // The claim the whole change rests on. Three callers parsed every saved
    // report on this thread: the Reports tab on every EventBus::directoryChanged,
    // the browser's folder facts on every navigation, every alert-rule write and
    // every index refresh, and the analysis tab at session restore for every
    // remembered folder.
    const QString media = m_tree->rootUri().child(QStringLiteral("media")).toString();
    const QString docs = m_tree->rootUri().child(QStringLiteral("docs")).toString();

    AnalysisTabController* tab = openAnalysis({ media, docs });
    QVERIFY(tab);
    waitForReports(tab);
    ReportsController* library = openLibrary();
    QVERIFY(library);
    QCOMPARE(library->folderCount(), 2);

    // Everything the interface does that used to reach the store, after the
    // priming task has been and gone.
    CapturedWarnings warnings;
    for (int i = 0; i < 20; ++i)
        m_app->services().events->postDirectoryChanged(m_tree->rootUri());
    library->setSelectedRoot(media);
    library->setSelectedRoot(docs);
    library->setFilter(QStringLiteral("media"));
    library->setFilter(QString());
    (void)library->folders();
    (void)library->runs();
    tab->setCurrentIndex(0);
    tab->setCurrentIndex(1);
    drainEvents();

    QVERIFY2(!warnings.contains(QStringLiteral("Report store read on the thread that draws")),
        qPrintable(warnings.joined()));
}

void TestAnalysisTab::forgettingAFolderRemovesItFromTheLibrary()
{
    const QString media = m_tree->rootUri().child(QStringLiteral("media")).toString();

    AnalysisTabController* tab = openAnalysis({ media });
    QVERIFY(tab);
    waitForReports(tab);

    ReportsController* library = openLibrary();
    QVERIFY(library);
    QCOMPARE(library->folderCount(), 1);

    QVERIFY(library->forgetFolder(media));
    QCOMPARE(library->folderCount(), 0);
    // With nothing left there is nothing selected, rather than a selection
    // pointing at a folder that no longer has reports.
    QVERIFY(library->selectedRoot().isEmpty());
    QVERIFY(library->runs().isEmpty());
}

void TestAnalysisTab::openingAReportDoesNotRescan()
{
    const QString media = m_tree->rootUri().child(QStringLiteral("media")).toString();

    // One real run, so there is something saved to open.
    AnalysisTabController* first = openAnalysis({ media });
    QVERIFY(first);
    waitForReports(first);
    const QString savedId
        = first->current()->history().first().toMap().value(QStringLiteral("id")).toString();
    QVERIFY(!savedId.isEmpty());

    // Now open it the way the browser tag and the library do.
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.analysis"));
    QVERIFY(row >= 0);
    auto* opened = qobject_cast<AnalysisTabController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(opened);
    opened->setTargets({ media });

    // The saved numbers are there at once, and nothing is walking. On a large
    // tree a rescan takes minutes, and wanting to look at yesterday's report is
    // not a request for that.
    QVERIFY(opened->current());
    QVERIFY(opened->current()->hasReport());
    QVERIFY2(!opened->current()->isBusy(), "opening a report must not start a scan");
    QCOMPARE(opened->current()->history().size(), 1);

    drainEvents();
    QVERIFY2(!opened->current()->isBusy(), "and must not start one a moment later either");

    // Asking for a fresh one is a separate, explicit act.
    opened->refreshAll();
    QVERIFY(waitFor([opened] { return opened->current()->history().size() == 2; }, 20000));
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestAnalysisTab testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_AnalysisTab.moc"
