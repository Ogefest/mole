#include "plugins/builtin/BrowserFeature.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/SearchFeatures.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/SessionStore.h"
#include "ui/models/BrowserPaneController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

using namespace mole;
using namespace mole::test;

/// Closing and reopening the application has to put the user back where they
/// were: same tabs, same folders, same layout.
class TestSession : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // --- the store on its own ---
    void savesAndLoadsARoundTrip();
    void missingFileYieldsAnEmptySession();
    void corruptFileYieldsAnEmptySession();
    void unknownVersionYieldsAnEmptySession();
    void currentIndexIsClampedToTheTabsThatExist();
    void writeIsAtomic();

    // --- the application ---
    void firstRunOpensADefaultTab();
    void tabsComeBackAfterRestart();
    void browserRemembersItsFolder();
    void dualPaneLayoutIsRemembered();
    void gridLayoutIsRemembered();
    void unknownLayoutFallsBackToTheListing();
    void bothPanesAndActivePaneAreRemembered();
    void searchTabRemembersItsQueryButNotItsResults();
    void currentTabIsRemembered();
    void everyCriterionSurvivesARestart();
    void tabsFromAnUninstalledPluginAreSkipped();
    void aTabOfTheRetiredIndexSearchComesBackAsTheOneSearch();
    void unreachableFolderStillRestoresTheTab();

    void windowSizeIsRemembered();
    void maximisedStateIsRemembered();
    void fullScreenKeepsTheSizeItHadBefore();
    void aSessionFromBeforeTheTriStateStillRestoresMaximised();
    void offscreenPositionIsNotRestored();

private:
    QString sessionPath() const;
    void startApp();
    void restartApp();
    BrowserController* browserAt(int row) const;

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
};

void TestSession::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

QString TestSession::sessionPath() const
{
    return QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH"));
}

void TestSession::init()
{
    QFile::remove(sessionPath());

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->makeDirs(QStringLiteral("reports")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("archive")));
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/q1.txt")));
    QVERIFY(m_tree->writeFile(QStringLiteral("notes.txt")));
}

void TestSession::cleanup()
{
    m_app.reset();
    m_tree.reset();
}

void TestSession::startApp()
{
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));

    m_app = std::make_unique<AppController>();
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));
}

void TestSession::restartApp()
{
    // The destructor flushes whatever the debounce had not written yet.
    m_app.reset();
    drainEvents();
    startApp();
}

BrowserController* TestSession::browserAt(int row) const
{
    return qobject_cast<BrowserController*>(m_app->tabs()->controllerAt(row));
}

// ---------------------------------------------------------------- the store

void TestSession::savesAndLoadsARoundTrip()
{
    SessionStore store(sessionPath());

    Session written;
    written.tabs.append(TabSession {
        QStringLiteral("mole.browser"), { { QStringLiteral("left"), QStringLiteral("file:///home") } } });
    written.tabs.append(TabSession {
        QStringLiteral("mole.livesearch"), { { QStringLiteral("query"), QStringLiteral("report") } } });
    written.currentIndex = 1;

    QVERIFY(store.save(written));

    const Session read = store.load();
    QCOMPARE(read.tabs.size(), 2);
    QCOMPARE(read.tabs.at(0).featureId, QStringLiteral("mole.browser"));
    QCOMPARE(read.tabs.at(0).state.value(QStringLiteral("left")).toString(), QStringLiteral("file:///home"));
    QCOMPARE(read.tabs.at(1).state.value(QStringLiteral("query")).toString(), QStringLiteral("report"));
    QCOMPARE(read.currentIndex, 1);
}

void TestSession::missingFileYieldsAnEmptySession()
{
    SessionStore store(m_profile.filePath(QStringLiteral("nothing-here.json")));
    QVERIFY(store.load().isEmpty());
}

void TestSession::corruptFileYieldsAnEmptySession()
{
    // A half-written or hand-edited file must degrade into "start fresh", not
    // into a failure to launch.
    QFile file(sessionPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ this is not json at all");
    file.close();

    QVERIFY(SessionStore(sessionPath()).load().isEmpty());
}

void TestSession::unknownVersionYieldsAnEmptySession()
{
    QFile file(sessionPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version": 999, "currentIndex": 0, "tabs": [{"featureId": "mole.browser"}]})");
    file.close();

    // A newer build wrote this; guessing at a format we do not know is worse
    // than starting over.
    QVERIFY(SessionStore(sessionPath()).load().isEmpty());
}

void TestSession::currentIndexIsClampedToTheTabsThatExist()
{
    QFile file(sessionPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version": 1, "currentIndex": 7,
                   "tabs": [{"featureId": "mole.browser", "state": {}}]})");
    file.close();

    const Session loaded = SessionStore(sessionPath()).load();
    QCOMPARE(loaded.tabs.size(), 1);
    QCOMPARE(loaded.currentIndex, 0);
}

void TestSession::writeIsAtomic()
{
    SessionStore store(sessionPath());

    Session first;
    first.tabs.append(TabSession { QStringLiteral("mole.browser"), {} });
    QVERIFY(store.save(first));

    // Saving again must leave exactly one readable file behind, with no
    // temporary left over next to it.
    Session second;
    second.tabs.append(TabSession { QStringLiteral("mole.commander"), {} });
    QVERIFY(store.save(second));

    QCOMPARE(store.load().tabs.size(), 1);
    QCOMPARE(store.load().tabs.first().featureId, QStringLiteral("mole.commander"));

    const QFileInfo info(sessionPath());
    const QStringList strays = info.dir().entryList({ QStringLiteral("session.json.*") }, QDir::Files);
    QVERIFY2(strays.isEmpty(), qPrintable(strays.join(QLatin1Char(' '))));
}

// ---------------------------------------------------------- the application

void TestSession::firstRunOpensADefaultTab()
{
    startApp();

    QCOMPARE(m_app->tabs()->rowCount(), 1);
    QCOMPARE(
        m_app->tabs()->index(0, 0).data(TabsModel::FeatureIdRole).toString(), QStringLiteral("mole.browser"));
}

void TestSession::tabsComeBackAfterRestart()
{
    startApp();
    m_app->tabs()->openTab(QStringLiteral("mole.commander"));
    m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    QCOMPARE(m_app->tabs()->rowCount(), 3);

    restartApp();

    QCOMPARE(m_app->tabs()->rowCount(), 3);
    QCOMPARE(
        m_app->tabs()->index(0, 0).data(TabsModel::FeatureIdRole).toString(), QStringLiteral("mole.browser"));
    QCOMPARE(m_app->tabs()->index(1, 0).data(TabsModel::FeatureIdRole).toString(),
        QStringLiteral("mole.commander"));
    QCOMPARE(m_app->tabs()->index(2, 0).data(TabsModel::FeatureIdRole).toString(),
        QStringLiteral("mole.livesearch"));
}

void TestSession::browserRemembersItsFolder()
{
    startApp();
    BrowserController* browser = browserAt(0);
    QVERIFY(browser);

    const QString target = m_tree->rootUri().child(QStringLiteral("reports")).toString();
    browser->navigateActive(target);
    QVERIFY(waitFor([browser] { return !browser->activePane()->isLoading(); }));

    restartApp();

    BrowserController* restored = browserAt(0);
    QVERIFY(restored);
    QCOMPARE(restored->activePane()->currentUri(), target);
    QVERIFY(waitFor([restored] {
        return !restored->activePane()->isLoading() && restored->activePane()->files()->rowCount() == 1;
    }));
}

void TestSession::dualPaneLayoutIsRemembered()
{
    startApp();
    BrowserController* browser = browserAt(0);
    QVERIFY(browser);
    QVERIFY(!browser->splitEnabled());

    // Single versus dual is how the tab is being looked at, so it belongs to
    // the tab and has to survive a restart.
    browser->setViewMode(BrowserController::ViewMode::Dual);

    restartApp();

    QVERIFY2(browserAt(0)->splitEnabled(), "the layout must come back as it was left");
}

void TestSession::gridLayoutIsRemembered()
{
    startApp();
    BrowserController* browser = browserAt(0);
    QVERIFY(browser);
    QVERIFY(!browser->gridEnabled());

    browser->setViewMode(BrowserController::ViewMode::Grid);
    QVERIFY(browser->gridEnabled());
    QVERIFY2(!browser->splitEnabled(), "grid is one pane, not two");

    restartApp();

    QVERIFY(browserAt(0)->gridEnabled());
}

void TestSession::unknownLayoutFallsBackToTheListing()
{
    // A newer build might write a layout this one has never heard of. Falling
    // back to the plain listing beats refusing to restore the tab.
    QFile file(sessionPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version": 1, "currentIndex": 0, "tabs": [
                    {"featureId": "mole.browser", "state": {"viewMode": "hologram"}}]})");
    file.close();

    startApp();

    QCOMPARE(m_app->tabs()->rowCount(), 1);
    BrowserController* browser = browserAt(0);
    QVERIFY(browser);
    QVERIFY(!browser->gridEnabled());
    QVERIFY(!browser->splitEnabled());
}

void TestSession::bothPanesAndActivePaneAreRemembered()
{
    startApp();
    BrowserController* browser = browserAt(0);
    browser->setViewMode(BrowserController::ViewMode::Dual);

    const QString left = m_tree->rootUri().child(QStringLiteral("reports")).toString();
    const QString right = m_tree->rootUri().child(QStringLiteral("archive")).toString();
    browser->left()->navigateTo(left);
    browser->right()->navigateTo(right);
    browser->setActivePaneIndex(1);
    QVERIFY(waitFor([browser] { return !browser->left()->isLoading() && !browser->right()->isLoading(); }));

    restartApp();

    BrowserController* restored = browserAt(0);
    QCOMPARE(restored->left()->currentUri(), left);
    QCOMPARE(restored->right()->currentUri(), right);
    QCOMPARE(restored->activePaneIndex(), 1);
}

void TestSession::searchTabRemembersItsQueryButNotItsResults()
{
    startApp();
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(search);

    search->setRootUri(m_tree->rootUri().toString());
    search->setQueryText(QStringLiteral("q1"));
    search->start();
    QVERIFY(waitFor([search] { return !search->isRunning(); }, 10000));
    QCOMPARE(search->results()->rowCount(), 1);

    restartApp();

    auto* restored = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(1));
    QVERIFY(restored);
    QCOMPARE(restored->queryText(), QStringLiteral("q1"));
    QCOMPARE(restored->rootUri(), m_tree->rootUri().toString());
    // Re-walking the disk on startup would be a surprise, and showing stale
    // hits would be worse than showing none.
    QCOMPARE(restored->results()->rowCount(), 0);
    QVERIFY(!restored->isRunning());
}

/// A query built out of nine fields is not something to make somebody build
/// again because they restarted.
void TestSession::everyCriterionSurvivesARestart()
{
    startApp();
    const int row = m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(search);

    search->setRootUri(m_tree->rootUri().toString());
    search->setQueryText(QStringLiteral("report-*.pdf"));
    search->setNameMode(1);
    search->setWholeWord(true);
    search->setExcludeName(true);
    search->setPathText(QStringLiteral("invoices"));
    search->setExcludePath(true);
    search->setExtension(QStringLiteral("jpg, jpeg"));
    search->setTypeClasses({ QStringLiteral("image"), QStringLiteral("document") });
    search->setModifiedFrom(QStringLiteral("last 7 days"));
    search->setModifiedTo(QStringLiteral("today"));
    search->setCreatedFrom(QStringLiteral("2026-01-01"));
    search->setAccessedFrom(QStringLiteral("30d"));
    search->setKindMode(1);
    search->setEmptyOnly(true);
    search->setIncludeHidden(false);
    search->setMaxDepth(0);
    search->setExcluded(QStringLiteral("node_modules, .git"));
    search->setSizeRange(QStringLiteral("10k"), QStringLiteral("2M"));

    restartApp();

    auto* restored = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(restored);
    QCOMPARE(restored->queryText(), QStringLiteral("report-*.pdf"));
    QCOMPARE(restored->nameMode(), 1);
    QVERIFY(restored->wholeWord());
    QVERIFY(restored->excludeName());
    QCOMPARE(restored->pathText(), QStringLiteral("invoices"));
    QVERIFY(restored->excludePath());
    QCOMPARE(restored->extension(), QStringLiteral("jpg, jpeg"));
    QCOMPARE(restored->typeClasses(), QStringList({ QStringLiteral("image"), QStringLiteral("document") }));
    QCOMPARE(restored->modifiedFrom(), QStringLiteral("last 7 days"));
    QCOMPARE(restored->modifiedTo(), QStringLiteral("today"));
    QCOMPARE(restored->createdFrom(), QStringLiteral("2026-01-01"));
    QCOMPARE(restored->accessedFrom(), QStringLiteral("30d"));
    QCOMPARE(restored->kindMode(), 1);
    QVERIFY(restored->emptyOnly());
    QVERIFY(!restored->includeHidden());
    QCOMPARE(restored->maxDepth(), 0);
    QCOMPARE(restored->excluded(), QStringLiteral("node_modules, .git"));
    QCOMPARE(restored->minSize(), 10 * 1024);
    QCOMPARE(restored->maxSize(), 2 * 1024 * 1024);
}

void TestSession::currentTabIsRemembered()
{
    startApp();
    m_app->tabs()->openTab(QStringLiteral("mole.commander"));
    m_app->tabs()->openTab(QStringLiteral("mole.livesearch"));
    m_app->tabs()->setCurrentIndex(1);

    restartApp();

    QCOMPARE(m_app->tabs()->currentIndex(), 1);
    QCOMPARE(m_app->tabs()->index(1, 0).data(TabsModel::FeatureIdRole).toString(),
        QStringLiteral("mole.commander"));
}

void TestSession::tabsFromAnUninstalledPluginAreSkipped()
{
    // Hand-write a session referring to a feature nobody provides, the way an
    // uninstalled plugin would leave things.
    QFile file(sessionPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version": 1, "currentIndex": 0, "tabs": [
                    {"featureId": "org.gone.missing", "state": {}},
                    {"featureId": "mole.browser", "state": {}}]})");
    file.close();

    startApp();

    // The surviving tab comes back; the missing one is dropped rather than
    // taking the whole restore down with it.
    QCOMPARE(m_app->tabs()->rowCount(), 1);
    QCOMPARE(
        m_app->tabs()->index(0, 0).data(TabsModel::FeatureIdRole).toString(), QStringLiteral("mole.browser"));
}

/// A feature merged into another is not a feature nobody provides.
///
/// The indexed search was its own tab until the two became one, and its id is
/// written into every session saved before that. Dropping those tabs would be
/// the answer an uninstalled plugin deserves; what they meant is this search,
/// asked of everywhere indexed, and that is where they land.
void TestSession::aTabOfTheRetiredIndexSearchComesBackAsTheOneSearch()
{
    QFile file(sessionPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version": 1, "currentIndex": 0, "tabs": [
                    {"featureId": "mole.indexsearch",
                     "state": {"query": "quarterly", "volumeIndex": 0}}]})");
    file.close();

    startApp();

    QCOMPARE(m_app->tabs()->rowCount(), 1);
    QCOMPARE(m_app->tabs()->index(0, 0).data(TabsModel::FeatureIdRole).toString(),
        QStringLiteral("mole.livesearch"));

    auto* search = qobject_cast<LiveSearchController*>(m_app->tabs()->controllerAt(0));
    QVERIFY(search);
    QCOMPARE(search->queryText(), QStringLiteral("quarterly"));
    QVERIFY2(search->everywhere(), "the tab came back scoped to one folder, which is not what it asked");
}

void TestSession::unreachableFolderStillRestoresTheTab()
{
    startApp();
    BrowserController* browser = browserAt(0);
    const QString doomed = m_tree->rootUri().child(QStringLiteral("archive")).toString();
    browser->navigateActive(doomed);
    QVERIFY(waitFor([browser] { return !browser->activePane()->isLoading(); }));

    m_app->saveSessionNow();
    m_app.reset();
    drainEvents();

    // The folder disappears while the application is closed.
    QVERIFY(QDir(m_tree->absolute(QStringLiteral("archive"))).removeRecursively());

    startApp();

    // The tab must still be there, showing the problem, rather than the whole
    // restore being abandoned.
    QCOMPARE(m_app->tabs()->rowCount(), 1);
    BrowserController* restored = browserAt(0);
    QVERIFY(restored);
    QVERIFY(waitFor([restored] { return !restored->activePane()->errorText().isEmpty(); }, 10000));
}

void TestSession::windowSizeIsRemembered()
{
    startApp();
    m_app->rememberWindowGeometry(120, 80, 1000, 700, QWindow::Windowed);
    m_app->saveSessionNow();

    restartApp();

    const QVariantMap geometry = m_app->savedWindowGeometry();
    QCOMPARE(geometry.value(QStringLiteral("width")).toInt(), 1000);
    QCOMPARE(geometry.value(QStringLiteral("height")).toInt(), 700);
    QCOMPARE(geometry.value(QStringLiteral("windowState")).toString(), QStringLiteral("normal"));
}

void TestSession::maximisedStateIsRemembered()
{
    startApp();
    m_app->rememberWindowGeometry(0, 0, 900, 600, QWindow::Windowed);
    m_app->rememberWindowGeometry(0, 0, 1920, 1080, QWindow::Maximized);
    m_app->saveSessionNow();

    restartApp();

    const QVariantMap geometry = m_app->savedWindowGeometry();
    QCOMPARE(geometry.value(QStringLiteral("windowState")).toString(), QStringLiteral("maximized"));
    // The size from before maximising is kept, so un-maximising lands
    // somewhere sensible instead of filling the screen twice.
    QCOMPARE(geometry.value(QStringLiteral("width")).toInt(), 900);
    QCOMPARE(geometry.value(QStringLiteral("height")).toInt(), 600);
}

/// The same claim for full-screen, which is the one that was broken. The state
/// used to reach the application as a single boolean -- "is it maximised" --
/// and a full-screen window is not maximised, so its screen-sized metrics were
/// taken for the size somebody had chosen and written over the real one. The
/// next start then opened a plain window the size of the display.
void TestSession::fullScreenKeepsTheSizeItHadBefore()
{
    startApp();
    m_app->rememberWindowGeometry(40, 30, 900, 600, QWindow::Windowed);
    m_app->rememberWindowGeometry(0, 0, 1920, 1080, QWindow::FullScreen);
    m_app->saveSessionNow();

    restartApp();

    const QVariantMap geometry = m_app->savedWindowGeometry();
    QCOMPARE(geometry.value(QStringLiteral("windowState")).toString(), QStringLiteral("fullscreen"));
    QCOMPARE(geometry.value(QStringLiteral("width")).toInt(), 900);
    QCOMPARE(geometry.value(QStringLiteral("height")).toInt(), 600);
    QCOMPARE(geometry.value(QStringLiteral("x")).toInt(), 40);
    QCOMPARE(geometry.value(QStringLiteral("y")).toInt(), 30);
}

/// Somebody upgrading has a session file with a "maximized" boolean in it, and
/// should not have to maximise the window again to teach the new build what it
/// already knew.
void TestSession::aSessionFromBeforeTheTriStateStillRestoresMaximised()
{
    startApp();
    m_app->rememberWindowGeometry(10, 20, 800, 640, QWindow::Windowed);
    m_app->saveSessionNow();
    // Gone before the file is edited, not restarted around it: the destructor
    // flushes what it still holds, which would write straight over the edit.
    m_app.reset();
    drainEvents();

    // The file as the previous version wrote it: no windowState, a boolean
    // instead.
    const QString path = QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    QJsonObject window = root.value(QStringLiteral("window")).toObject();
    window.remove(QStringLiteral("windowState"));
    window[QStringLiteral("maximized")] = true;
    root[QStringLiteral("window")] = window;

    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(root).toJson());
    file.close();

    startApp();

    const QVariantMap geometry = m_app->savedWindowGeometry();
    QCOMPARE(geometry.value(QStringLiteral("windowState")).toString(), QStringLiteral("maximized"));
    QCOMPARE(geometry.value(QStringLiteral("width")).toInt(), 800);
}

void TestSession::offscreenPositionIsNotRestored()
{
    // A monitor that is no longer plugged in would put the window out of
    // reach, so the size comes back but the position does not.
    QVERIFY(!AppController::geometryIsOnScreen(-9000, -9000, 800, 600));
    QVERIFY(AppController::geometryIsOnScreen(0, 0, 800, 600));
    QVERIFY(!AppController::geometryIsOnScreen(0, 0, 0, 0));

    startApp();
    m_app->rememberWindowGeometry(-9000, -9000, 800, 600, QWindow::Windowed);
    m_app->saveSessionNow();
    restartApp();

    const QVariantMap geometry = m_app->savedWindowGeometry();
    QCOMPARE(geometry.value(QStringLiteral("width")).toInt(), 800);
    QVERIFY2(!geometry.contains(QStringLiteral("x")), "an unreachable position must be dropped");
}

// A GUI application, not a console one: the window-geometry tests need screens
// to exist, and QCoreApplication has none.
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestSession testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}
#include "tst_Session.moc"
