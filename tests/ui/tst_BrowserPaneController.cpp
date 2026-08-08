#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/models/BrowserPaneController.h"

#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

class TestBrowserPaneController : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void navigateLoadsTheListing();
    void navigationHappensOffTheUiThread();
    void unmountedLocationReportsAnError();
    void invalidUriReportsAnError();
    void historyGoesBackAndForward();
    void navigatingAfterBackTruncatesForward();
    void goUpWalksTowardsTheRoot();
    void activateEntersDirectoriesAndSignalsFiles();
    void backendFailureSurfacesAsErrorText();
    void rapidNavigationKeepsOnlyTheLastResult();
    void destructionDuringLoadIsSafe();

    void cursorStaysInsideTheListing();
    void cursorResetsWhenTheListingChanges();
    void insertTicksAndAdvances();
    void targetsAreListedByNameAndNotJustCounted();
    void targetDetailsFollowTheCursorWhenNothingIsTicked();
    void createDirectoryAnnouncesItself();
    void renameMovesTheEntry();
    void deleteRemovesTheTargets();
    void readOnlyDriveIsReported();

    void goingUpLandsOnTheFolderJustLeft();
    void goingBackRestoresTheCursor();
    void aForgottenEntryFallsBackToTheFirstRow();

private:
    BrowserPaneController* makePane();

    std::unique_ptr<QTemporaryDir> m_dir;
    VfsManager* m_vfs = nullptr;
    TaskManager* m_tasks = nullptr;
    EventBus* m_events = nullptr;
    std::unique_ptr<IndexDatabase> m_index;
    std::shared_ptr<MemoryFileSystem> m_fs;
    PluginServices m_services;
};

void TestBrowserPaneController::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    m_vfs = new VfsManager(this);
    m_tasks = new TaskManager(this);
    m_events = new EventBus(this);
    m_index = std::make_unique<IndexDatabase>(QDir(m_dir->path()).filePath(QStringLiteral("i.sqlite")));
    QVERIFY(m_index->open().ok());

    m_fs = std::make_shared<MemoryFileSystem>();
    m_fs->addFile(QStringLiteral("/docs/a.txt"), QByteArray("aaa"));
    m_fs->addFile(QStringLiteral("/docs/deep/b.txt"), QByteArray("bb"));
    m_fs->addFile(QStringLiteral("/notes.md"));

    Mount mount;
    mount.displayName = QStringLiteral("scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = m_fs;
    m_vfs->addMount(mount);

    m_services = PluginServices { m_vfs, m_tasks, m_index.get(), m_events };
}

void TestBrowserPaneController::cleanup()
{
    delete m_tasks;
    m_tasks = nullptr;
    delete m_vfs;
    m_vfs = nullptr;
    delete m_events;
    m_events = nullptr;
    m_index.reset();
    m_fs.reset();
    m_dir.reset();
}

BrowserPaneController* TestBrowserPaneController::makePane()
{
    return new BrowserPaneController(m_services, this);
}

void TestBrowserPaneController::navigateLoadsTheListing()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));

    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() > 0; }));
    QCOMPARE(pane->files()->rowCount(), 2); // a.txt and the deep/ directory
    QCOMPARE(pane->currentUri(), QStringLiteral("mem:///docs"));
    QCOMPARE(pane->locationName(), QStringLiteral("docs"));
    QVERIFY(pane->errorText().isEmpty());
}

void TestBrowserPaneController::navigationHappensOffTheUiThread()
{
    // A pane that blocks while listing is the single worst failure mode in a
    // file manager, so assert the loading flag actually goes up.
    m_fs->setListDelayMs(120);

    BrowserPaneController* pane = makePane();
    QSignalSpy loading(pane, &BrowserPaneController::loadingChanged);
    pane->navigateTo(QStringLiteral("mem:///docs"));

    QVERIFY2(pane->isLoading(), "navigation must start asynchronously");
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }, 5000));
    QVERIFY(loading.count() >= 2);
}

void TestBrowserPaneController::unmountedLocationReportsAnError()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("sftp://nowhere/x"));

    QVERIFY(!pane->errorText().isEmpty());
    QVERIFY(pane->errorText().contains(QStringLiteral("No drive")));
    QCOMPARE(pane->files()->rowCount(), 0);
}

void TestBrowserPaneController::invalidUriReportsAnError()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("this is not a uri"));

    QVERIFY(!pane->errorText().isEmpty());
    QVERIFY(!pane->canGoBack());
}

void TestBrowserPaneController::historyGoesBackAndForward()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///"));
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QVERIFY(!pane->canGoBack());

    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QVERIFY(pane->canGoBack());
    QVERIFY(!pane->canGoForward());

    pane->goBack();
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QCOMPARE(pane->currentUri(), QStringLiteral("mem:///"));
    QVERIFY(pane->canGoForward());

    pane->goForward();
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QCOMPARE(pane->currentUri(), QStringLiteral("mem:///docs"));
}

void TestBrowserPaneController::navigatingAfterBackTruncatesForward()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///"));
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));

    pane->goBack();
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QVERIFY(pane->canGoForward());

    pane->navigateTo(QStringLiteral("mem:///docs/deep"));
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));

    // Same rule as a web browser: a new destination discards the forward stack.
    QVERIFY2(!pane->canGoForward(), "forward history must be dropped after a new navigation");
    QVERIFY(pane->canGoBack());
}

void TestBrowserPaneController::goUpWalksTowardsTheRoot()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs/deep"));
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QVERIFY(pane->canGoUp());

    pane->goUp();
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QCOMPARE(pane->currentUri(), QStringLiteral("mem:///docs"));

    pane->goUp();
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QCOMPARE(pane->currentUri(), QStringLiteral("mem:///"));
    QVERIFY2(!pane->canGoUp(), "the root has no parent");
}

void TestBrowserPaneController::activateEntersDirectoriesAndSignalsFiles()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    QSignalSpy fileActivated(pane, &BrowserPaneController::fileActivated);

    // Directories sort first, so row 0 is deep/ and row 1 is a.txt.
    QVERIFY(pane->activate(0));
    QVERIFY(waitFor([pane] { return pane->currentUri() == QStringLiteral("mem:///docs/deep"); }));
    QCOMPARE(fileActivated.count(), 0);

    pane->goBack();
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    QVERIFY(!pane->activate(1));
    QCOMPARE(fileActivated.count(), 1);
    QCOMPARE(fileActivated.first().first().toString(), QStringLiteral("mem:///docs/a.txt"));
}

void TestBrowserPaneController::backendFailureSurfacesAsErrorText()
{
    m_fs->setFault(QStringLiteral("/docs"), VfsError::AccessDenied);

    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));

    QVERIFY(waitFor([pane] { return !pane->errorText().isEmpty(); }));
    QVERIFY(!pane->isLoading());
}

void TestBrowserPaneController::rapidNavigationKeepsOnlyTheLastResult()
{
    // Clicking through folders faster than a slow mount can answer must not
    // leave the pane showing an earlier directory's contents.
    m_fs->setListDelayMs(60);

    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///"));
    pane->navigateTo(QStringLiteral("mem:///docs"));
    pane->navigateTo(QStringLiteral("mem:///docs/deep"));

    QVERIFY(waitFor([pane] { return !pane->isLoading(); }, 8000));
    QCOMPARE(pane->currentUri(), QStringLiteral("mem:///docs/deep"));
    QCOMPARE(pane->files()->rowCount(), 1);
    QCOMPARE(pane->files()->uriAt(0), QStringLiteral("mem:///docs/deep/b.txt"));
}

void TestBrowserPaneController::destructionDuringLoadIsSafe()
{
    m_fs->setListDelayMs(200);

    auto* pane = new BrowserPaneController(m_services, nullptr);
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(pane->isLoading());

    // Closing a tab while its listing is still in flight is routine.
    delete pane;
    drainEvents();
    QVERIFY(waitFor([this] { return m_tasks->activeCount() == 0; }, 8000));
}

void TestBrowserPaneController::cursorStaysInsideTheListing()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    QCOMPARE(pane->currentIndex(), 0);

    // Arrow keys hand over whatever number they land on; clamping is here so
    // no caller has to remember to do it.
    pane->moveCursor(-5);
    QCOMPARE(pane->currentIndex(), 0);

    pane->moveCursor(99);
    QCOMPARE(pane->currentIndex(), 1);

    pane->cursorToStart();
    QCOMPARE(pane->currentIndex(), 0);
    pane->cursorToEnd();
    QCOMPARE(pane->currentIndex(), 1);
}

void TestBrowserPaneController::cursorResetsWhenTheListingChanges()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));
    pane->cursorToEnd();
    QCOMPARE(pane->currentIndex(), 1);

    pane->navigateTo(QStringLiteral("mem:///docs/deep"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 1; }));

    // Landing in a new folder with the cursor pointing at row 1 of the old one
    // would be arbitrary; start at the top.
    QCOMPARE(pane->currentIndex(), 0);
}

void TestBrowserPaneController::insertTicksAndAdvances()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    pane->toggleSelectionAndAdvance();
    QCOMPARE(pane->files()->selectionCount(), 1);
    QCOMPARE(pane->currentIndex(), 1);
    QCOMPARE(pane->targetCount(), 1);

    pane->toggleSelectionAndAdvance();
    QCOMPARE(pane->files()->selectionCount(), 2);
    QCOMPARE(pane->targetSummary(), QStringLiteral("2 items"));
}

// A count answers "how many". Before something is deleted the question is "which
// ones", and until this landed there was no way for a dialog to ask it.
void TestBrowserPaneController::targetsAreListedByNameAndNotJustCounted()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    pane->files()->selectAll();
    const QVariantList details = pane->targetDetails();
    QCOMPARE(details.size(), 2);

    QStringList named;
    for (const QVariant& row : details)
        named.append(row.toMap().value(QStringLiteral("name")).toString());
    named.sort();
    QCOMPARE(named, QStringList({ QStringLiteral("a.txt"), QStringLiteral("deep") }));

    // Which of them is a folder, because a folder goes with everything inside it
    // and that is the difference between deleting one file and deleting a tree.
    QVariantMap folder;
    QVariantMap file;
    for (const QVariant& row : details) {
        if (row.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("deep"))
            folder = row.toMap();
        else
            file = row.toMap();
    }
    QVERIFY2(folder.value(QStringLiteral("isDir")).toBool(), "a folder says so");
    QVERIFY2(!file.value(QStringLiteral("isDir")).toBool(), "a file says so");

    QCOMPARE(file.value(QStringLiteral("detail")).toString(), QStringLiteral("3 B"));
    // A folder's own size says nothing about what is inside it, so it claims nothing.
    QCOMPARE(folder.value(QStringLiteral("detail")).toString(), QString());

    // The list and the deletion read the same rule, rather than being two opinions
    // about what is selected that could drift apart.
    QCOMPARE(details.size(), pane->targetCount());
}

void TestBrowserPaneController::targetDetailsFollowTheCursorWhenNothingIsTicked()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    QCOMPARE(pane->files()->selectionCount(), 0);
    const QVariantList details = pane->targetDetails();
    QCOMPARE(details.size(), 1);
    QCOMPARE(details.first().toMap().value(QStringLiteral("name")).toString(),
        pane->files()->nameAt(pane->currentIndex()));
}

void TestBrowserPaneController::createDirectoryAnnouncesItself()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));

    QSignalSpy created(m_events, &EventBus::entryCreated);
    pane->createDirectory(QStringLiteral("brand-new"));

    QVERIFY(m_fs->stat(VfsUri::fromString(QStringLiteral("mem:///docs/brand-new"))).ok());
    // Announcing rather than refreshing directly is what makes a second pane
    // on the same folder notice.
    QCOMPARE(created.count(), 1);

    QSignalSpy failed(pane, &BrowserPaneController::operationFailed);
    pane->createDirectory(QStringLiteral("brand-new"));
    QCOMPARE(failed.count(), 1);

    pane->createDirectory(QStringLiteral("   "));
    QCOMPARE(failed.count(), 1); // blank names are ignored, not an error
}

void TestBrowserPaneController::renameMovesTheEntry()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    // Row 0 is the "deep" directory, row 1 is a.txt.
    pane->setCurrentIndex(1);
    QCOMPARE(pane->currentName(), QStringLiteral("a.txt"));

    QSignalSpy renamed(m_events, &EventBus::entryRenamed);
    pane->renameCurrent(QStringLiteral("renamed.txt"));

    QCOMPARE(renamed.count(), 1);
    QVERIFY(m_fs->stat(VfsUri::fromString(QStringLiteral("mem:///docs/renamed.txt"))).ok());
    QVERIFY(!m_fs->stat(VfsUri::fromString(QStringLiteral("mem:///docs/a.txt"))).ok());

    // A pane refreshes when its owner tells it to -- BrowserController does
    // that off the event bus -- so bring the listing up to date before acting
    // on it again.
    pane->refresh();
    QVERIFY(waitFor([pane] {
        return !pane->isLoading() && pane->files()->rowOfUri(QStringLiteral("mem:///docs/renamed.txt")) >= 0;
    }));
    pane->setCurrentIndex(pane->files()->rowOfUri(QStringLiteral("mem:///docs/renamed.txt")));

    // Renaming to the name it already has is a no-op, not an AlreadyExists error.
    QSignalSpy failed(pane, &BrowserPaneController::operationFailed);
    pane->renameCurrent(QStringLiteral("renamed.txt"));
    QCOMPARE(failed.count(), 0);
    QCOMPARE(renamed.count(), 1);
}

void TestBrowserPaneController::deleteRemovesTheTargets()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    pane->setCurrentIndex(1); // a.txt, nothing ticked
    pane->deleteTargets();

    QVERIFY(waitFor(
        [this] { return !m_fs->stat(VfsUri::fromString(QStringLiteral("mem:///docs/a.txt"))).ok(); }));
    // The directory survives: delete acts on the cursor, not the whole folder.
    QVERIFY(m_fs->stat(VfsUri::fromString(QStringLiteral("mem:///docs/deep"))).ok());
}

void TestBrowserPaneController::readOnlyDriveIsReported()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));

    // The in-memory drive is writable; the UI greys out actions based on this.
    QVERIFY(pane->isWritable());
}

void TestBrowserPaneController::goingUpLandsOnTheFolderJustLeft()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    const int row = pane->files()->rowOfUri(QStringLiteral("mem:///docs/deep"));
    QVERIFY(row >= 0);
    pane->setCurrentIndex(row);
    QVERIFY(pane->activate(row));
    QVERIFY(waitFor([pane] { return pane->currentUri() == QStringLiteral("mem:///docs/deep"); }));

    pane->goUp();
    QVERIFY(waitFor([pane] { return pane->currentUri() == QStringLiteral("mem:///docs"); }));

    // Walking a tree with the keyboard should feel like walking, not like
    // restarting at the top of every level.
    QVERIFY(waitFor([pane, row] { return pane->currentIndex() == row; }));
    QCOMPARE(pane->files()->uriAt(pane->currentIndex()), QStringLiteral("mem:///docs/deep"));
}

void TestBrowserPaneController::goingBackRestoresTheCursor()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    // Park the cursor somewhere that is not the first row, then leave and come
    // back the way the back button does.
    pane->setCurrentIndex(1);
    const QString parked = pane->files()->uriAt(1);
    QVERIFY(!parked.isEmpty());

    pane->navigateTo(QStringLiteral("mem:///"));
    QVERIFY(waitFor([pane] { return pane->currentUri() == QStringLiteral("mem:///"); }));

    pane->goBack();
    QVERIFY(waitFor([pane] { return pane->currentUri() == QStringLiteral("mem:///docs"); }));
    QVERIFY(waitFor([pane, parked] { return pane->files()->uriAt(pane->currentIndex()) == parked; }));
}

void TestBrowserPaneController::aForgottenEntryFallsBackToTheFirstRow()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    const int row = pane->files()->rowOfUri(QStringLiteral("mem:///docs/a.txt"));
    QVERIFY(row >= 0);
    pane->setCurrentIndex(row);

    pane->navigateTo(QStringLiteral("mem:///"));
    QVERIFY(waitFor([pane] { return pane->currentUri() == QStringLiteral("mem:///"); }));

    // The entry the cursor was on is gone by the time we come back. Landing on
    // a stale index, or on nothing, would be worse than landing on the top.
    QVERIFY(m_fs->remove(VfsUri::fromString(QStringLiteral("mem:///docs/a.txt")), false).ok());

    pane->goBack();
    QVERIFY(waitFor([pane] { return pane->currentUri() == QStringLiteral("mem:///docs"); }));
    QVERIFY(waitFor([pane] { return pane->files()->rowCount() == 1; }));
    QCOMPARE(pane->currentIndex(), 0);
}

MOLE_TEST_MAIN(TestBrowserPaneController)
#include "tst_BrowserPaneController.moc"
