#include "support/FaultyFileSystem.h"
#include "support/GitFixture.h"
#include "support/MoleTestMain.h"
#include "support/OfferingFileSystem.h"
#include "support/TestSupport.h"
#include "ui/models/BrowserPaneController.h"

#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vcs/ReadRepositoryTask.h"
#include "core/vcs/ReadStatusTask.h"
#include "core/vcs/Repository.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

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
    void aDragOfATickedRowCarriesTheWholeSelection();
    void aDragOfAnUntickedRowCarriesThatRowAlone();

    void droppedFilesArriveWithTheirContents();
    void aDroppedFolderArrivesWithEverythingUnderneathIt();
    void aDropOfAddressesRatherThanFilesIsRefusedOutLoud();
    void aDropOfWhatIsAlreadyHereDoesNothingAndSaysNothing();
    void aCollidingNameIsReportedBeforeAnythingIsWritten();
    void stopKeepsTheFileThatWasAlreadyThere();
    void skipKeepsTheFileThatWasAlreadyThere();
    void overwriteReplacesIt();
    void aReadOnlyDestinationRefusesAndQueuesNothing();
    void createDirectoryAnnouncesItself();
    void renameMovesTheEntry();
    void deleteRemovesTheTargets();
    void readOnlyDriveIsReported();

    void aFolderInsideACheckoutIsReportedWithItsBranch();
    void aFolderOutsideAnyCheckoutReportsNothing();
    void leavingACheckoutTakesTheAnswerWithIt();
    void aCheckoutReachedOverADriveThatIsNotLocalReportsNothing();
    void twoCheckoutsInOnePaneEachReportTheirOwnBranch();
    void aDirtyCheckoutIsCountedAndACleanOneSaysSo();
    void movingBetweenFoldersInOneCheckoutWalksItOnce();
    void leavingACheckoutTakesTheCountWithIt();
    void walkingAwayFromACheckoutAbandonsItsWalk();

    void everyKindOfChangeCarriesItsOwnMarker();
    void aConflictedFileSaysSoRatherThanModified();
    void aFolderSaysThatSomethingInsideItChanged();
    void anIgnoredRowCarriesNoMarker();
    void aFolderOutsideAnyCheckoutCarriesNoMarkers();
    void markersSurviveSortingAndFiltering();

    void acopyOverATrackedFileMarksItWithoutNavigating();
    void deletingATrackedFileIsNoticed();
    void aCommitMadeOutsideMoleClearsTheMarkers();
    void aBurstOfWritesIsOneWalkRatherThanOnePerFile();
    void aRefreshLeavesTheCursorAndTheTicksAlone();

    void oneQueryMarksTheWholeFolderHoweverManyRowsItHas();
    void aFolderOfFiveThousandIsStillOneQuery();
    void aDriveWithNothingToOfferIsNotAskedAboutTheFolderAtAll();
    void navigatingAwayAbandonsTheFolderQuery();

    void whatTheDriveCanDoFollowsTheCursor();
    void anActionAnsweringWithTextSaysWhenItStopsWorking();
    void anActionAnsweringWithUrisOffersThemToOpen();
    void anActionThatFailsSaysWhichOne();
    void anIdTheDriveNeverOfferedIsNotHandedToIt();

    void openingAFolderAsksTheDriveWhatItCanDo();
    void walkingAroundOneDriveAsksItOnce();
    void aDriveNobodyOpensIsNeverAsked();

    void goingUpLandsOnTheFolderJustLeft();
    void goingBackRestoresTheCursor();
    void aForgottenEntryFallsBackToTheFirstRow();

private:
    BrowserPaneController* makePane();
    /// A pane whose git answer has arrived -- the read is a background task, so
    /// what is waited on is the answer rather than the listing.
    BrowserPaneController* paneOnCheckout(const QString& uri);
    /// Mounts `path` as a local drive and answers with its uri. `writable` false
    /// wraps it so the drive says it cannot be written to, which is what a
    /// mounted archive is.
    QString mountLocal(const QString& path, bool writable = true);
    /// A pane pointed at `uri`, with the listing already loaded.
    BrowserPaneController* paneOn(const QString& uri);
    /// A pane on a drive that contributes actions, with the cursor on the file
    /// that has both a link and two earlier versions.
    BrowserPaneController* paneOnOfferingDrive();
    /// The row `name` sits at, or -1.
    static int rowOf(BrowserPaneController* pane, const QString& name);
    /// Whether the row called `name` carries the drive's mark.
    static bool hasDriveMark(BrowserPaneController* pane, const QString& name);
    /// How many rows carry it.
    static int markedRows(BrowserPaneController* pane);
    static QByteArray contentsOf(const QString& path);
    /// The git letter on the row called `name`, or an empty string when it carries
    /// none. Answers "<no such row>" rather than nothing when the row is absent, so
    /// a fixture that did not do what it claimed cannot pass as a clean row.
    QString markFor(BrowserPaneController* pane, const QString& name) const;
    /// How many tasks have been submitted so far. TaskManager keeps a task after
    /// it has finished -- the strip shows what has just run -- so "queued
    /// nothing" is a number that did not change rather than a number that is zero.
    int queuedSoFar() const { return static_cast<int>(m_tasks->tasks().size()); }
    /// How many drive probes have been queued, by the title ProbeDriveTask gives
    /// itself. Browsing queues a listing and an ask-what-this-drive-can-do for
    /// every step, so a total is not an answer about probes.
    int probeTasksSoFar() const;

    /// Owns every pane this test made, and is destroyed before the services are.
    ///
    /// A pane parented to the test object outlives cleanup(), and a pane is not
    /// inert once it has been left alive: it watches a repository directory and
    /// holds a coalescing timer, either of which can wake it up long after the
    /// TaskManager it would submit to has been deleted. That was an order-dependent
    /// crash rather than a failure, which is the worst kind.
    std::unique_ptr<QObject> m_panes;
    std::unique_ptr<QTemporaryDir> m_dir;
    VfsManager* m_vfs = nullptr;
    TaskManager* m_tasks = nullptr;
    EventBus* m_events = nullptr;
    std::unique_ptr<IndexDatabase> m_index;
    std::shared_ptr<MemoryFileSystem> m_fs;
    std::shared_ptr<OfferingFileSystem> m_offering;
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

    m_panes = std::make_unique<QObject>();
    m_services = PluginServices { m_vfs, m_tasks, m_index.get(), m_events };
}

void TestBrowserPaneController::cleanup()
{
    // Panes first, so nothing can still be holding the services below.
    m_panes.reset();
    delete m_tasks;
    m_tasks = nullptr;
    delete m_vfs;
    m_vfs = nullptr;
    delete m_events;
    m_events = nullptr;
    m_index.reset();
    m_fs.reset();
    m_offering.reset();
    m_dir.reset();
}

BrowserPaneController* TestBrowserPaneController::makePane()
{
    return new BrowserPaneController(m_services, m_panes.get());
}

QString TestBrowserPaneController::mountLocal(const QString& path, bool writable)
{
    Mount mount;
    mount.id = QStringLiteral("disk-%1").arg(path);
    mount.displayName = QStringLiteral("disk");
    mount.root = VfsUri::fromLocalPath(path);

    FileSystemPtr fs = std::make_shared<LocalFileSystem>();
    if (!writable) {
        auto declared = std::make_shared<FaultyFileSystem>(fs);
        declared->readOnly();
        fs = declared;
    }
    mount.fileSystem = std::move(fs);
    m_vfs->addMount(mount);
    return mount.root.toString();
}

bool TestBrowserPaneController::hasDriveMark(BrowserPaneController* pane, const QString& name)
{
    const int row = rowOf(pane, name);
    if (row < 0)
        return false;
    return pane->files()->data(pane->files()->index(row, 0), FileListModel::HasDriveActionRole).toBool();
}

int TestBrowserPaneController::markedRows(BrowserPaneController* pane)
{
    int marked = 0;
    for (int row = 0; row < pane->files()->rowCount(); ++row) {
        if (pane->files()->data(pane->files()->index(row, 0), FileListModel::HasDriveActionRole).toBool())
            ++marked;
    }
    return marked;
}

int TestBrowserPaneController::probeTasksSoFar() const
{
    int probes = 0;
    for (const Task* task : m_tasks->tasks()) {
        if (task->title().startsWith(QStringLiteral("Asking what")))
            ++probes;
    }
    return probes;
}

int TestBrowserPaneController::rowOf(BrowserPaneController* pane, const QString& name)
{
    for (int row = 0; row < pane->files()->rowCount(); ++row) {
        if (pane->files()->nameAt(row) == name)
            return row;
    }
    return -1;
}

BrowserPaneController* TestBrowserPaneController::paneOnOfferingDrive()
{
    m_offering = std::make_shared<OfferingFileSystem>();
    m_offering->memory()->addFile(QStringLiteral("/report.txt"), QByteArray("the third draft"));
    m_offering->memory()->addFile(QStringLiteral("/untouched.txt"), QByteArray("never edited"));
    m_offering->addVersion(QStringLiteral("/report.txt"), QStringLiteral("v1"), QByteArray("the first"));
    m_offering->addVersion(QStringLiteral("/report.txt"), QStringLiteral("v2"), QByteArray("the second"));
    m_offering->setLinkable(QStringLiteral("/untouched.txt"), false);

    Mount mount;
    mount.id = QStringLiteral("offering");
    mount.displayName = QStringLiteral("offering");
    // Its own authority, because the fixture already has a scratch drive at
    // mem:/// and two mounts on one root are one mount as far as resolve() is
    // concerned.
    mount.root = VfsUri::fromString(QStringLiteral("mem://offering/"));
    mount.fileSystem = m_offering;
    if (m_vfs->addMount(mount).isEmpty())
        return nullptr;

    BrowserPaneController* pane = paneOn(QStringLiteral("mem://offering/"));
    if (!pane)
        return nullptr;
    pane->setCurrentIndex(rowOf(pane, QStringLiteral("report.txt")));
    return pane;
}

BrowserPaneController* TestBrowserPaneController::paneOn(const QString& uri)
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(uri);
    if (!waitFor([pane] { return !pane->isLoading(); }))
        return nullptr;
    return pane;
}

QByteArray TestBrowserPaneController::contentsOf(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
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

void TestBrowserPaneController::aDragOfATickedRowCarriesTheWholeSelection()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    pane->files()->selectAll();
    // Starting on either of them takes both: a drag that began inside the
    // selection is a drag of the selection.
    QCOMPARE(pane->dragTargets(0).size(), 2);
    QCOMPARE(pane->dragTargets(1).size(), 2);

    QStringList names;
    for (const QString& uri : pane->dragTargets(0))
        names.append(VfsUri::fromString(uri).fileName());
    names.sort();
    QCOMPARE(names, QStringList({ QStringLiteral("a.txt"), QStringLiteral("deep") }));
}

void TestBrowserPaneController::aDragOfAnUntickedRowCarriesThatRowAlone()
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem:///docs"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 2; }));

    pane->files()->setSelected(0, true);
    pane->setCurrentIndex(0);

    // Row 1 is ticked by nothing and is not where the cursor is, and it is still
    // the whole answer -- otherwise dragging one file out of a ticked set would
    // silently send the set, or worse, send whatever the keyboard was last on.
    const QStringList dragged = pane->dragTargets(1);
    QCOMPARE(dragged.size(), 1);
    QCOMPARE(VfsUri::fromString(dragged.first()).fileName(), pane->files()->nameAt(1));

    // And the drag changed neither of the two things a user can see.
    QCOMPARE(pane->currentIndex(), 0);
    QCOMPARE(pane->files()->selectionCount(), 1);
    QVERIFY(pane->files()->isSelected(0));

    // A row that does not exist is not a row: no payload rather than a guess.
    QVERIFY(pane->dragTargets(-1).isEmpty());
    QVERIFY(pane->dragTargets(99).isEmpty());
}

// ---- what a drop means -----------------------------------------------------
//
// No window and no gesture in any of these: the urls are handed straight to the
// controller, which is where every rule about what a drop means lives. The
// destination is a mounted local folder and the source is a second folder under
// no mount at all -- an ordinary download folder, and the case VfsManager cannot
// answer for.

void TestBrowserPaneController::droppedFilesArriveWithTheirContents()
{
    TempTree here;
    TempTree downloads;
    QVERIFY(here.isValid());
    QVERIFY(downloads.isValid());
    QVERIFY(downloads.writeFile(QStringLiteral("invoice.pdf"), QByteArray("%PDF-1.4 invoice")));
    QVERIFY(downloads.writeFile(QStringLiteral("photo.jpg"), QByteArray("\xff\xd8\xff jpeg")));

    const QString destination = mountLocal(here.path());
    BrowserPaneController* pane = paneOn(destination);
    QVERIFY(pane);

    // The premise of this test: the source really is outside every mount, which
    // is what an ordinary download folder is and the one case resolve() cannot
    // answer for.
    QVERIFY(!m_vfs->resolve(VfsUri::fromLocalPath(downloads.absolute(QStringLiteral("invoice.pdf")))));

    pane->dropHere({ QUrl::fromLocalFile(downloads.absolute(QStringLiteral("invoice.pdf"))).toString(),
        QUrl::fromLocalFile(downloads.absolute(QStringLiteral("photo.jpg"))).toString() });

    QVERIFY(waitFor([&here] {
        return QFileInfo::exists(here.absolute(QStringLiteral("invoice.pdf")))
            && QFileInfo::exists(here.absolute(QStringLiteral("photo.jpg")));
    }));

    // Arrived, and arrived whole: a file that is present and empty is the
    // failure this is really about.
    QCOMPARE(contentsOf(here.absolute(QStringLiteral("invoice.pdf"))), QByteArray("%PDF-1.4 invoice"));
    QCOMPARE(contentsOf(here.absolute(QStringLiteral("photo.jpg"))), QByteArray("\xff\xd8\xff jpeg"));

    // And the source is untouched, because a drop is a copy however the sending
    // application would have preferred it.
    QVERIFY(QFileInfo::exists(downloads.absolute(QStringLiteral("invoice.pdf"))));
}

void TestBrowserPaneController::aDroppedFolderArrivesWithEverythingUnderneathIt()
{
    TempTree here;
    TempTree downloads;
    QVERIFY(here.isValid());
    QVERIFY(downloads.isValid());
    QVERIFY(downloads.writeFile(QStringLiteral("holiday/beach.jpg"), QByteArray("sand")));
    QVERIFY(downloads.writeFile(QStringLiteral("holiday/raw/beach.dng"), QByteArray("negative")));

    BrowserPaneController* pane = paneOn(mountLocal(here.path()));
    QVERIFY(pane);

    pane->dropHere({ QUrl::fromLocalFile(downloads.absolute(QStringLiteral("holiday"))).toString() });

    QVERIFY(waitFor(
        [&here] { return QFileInfo::exists(here.absolute(QStringLiteral("holiday/raw/beach.dng"))); }));
    QCOMPARE(contentsOf(here.absolute(QStringLiteral("holiday/beach.jpg"))), QByteArray("sand"));
    QCOMPARE(contentsOf(here.absolute(QStringLiteral("holiday/raw/beach.dng"))), QByteArray("negative"));
}

void TestBrowserPaneController::aDropOfAddressesRatherThanFilesIsRefusedOutLoud()
{
    TempTree here;
    QVERIFY(here.isValid());
    BrowserPaneController* pane = paneOn(mountLocal(here.path()));
    QVERIFY(pane);
    QSignalSpy failed(pane, &BrowserPaneController::operationFailed);
    const int queued = queuedSoFar();

    // What a web browser hands over when a picture is dragged out of a page.
    pane->dropHere({ QStringLiteral("https://example.invalid/cat.png"),
        QStringLiteral("https://example.invalid/page.html") });

    QCOMPARE(failed.count(), 1);
    QVERIFY(!failed.first().first().toString().isEmpty());
    QCOMPARE(queuedSoFar(), queued);
    QCOMPARE(QDir(here.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
}

void TestBrowserPaneController::aDropOfWhatIsAlreadyHereDoesNothingAndSaysNothing()
{
    TempTree here;
    QVERIFY(here.isValid());
    QVERIFY(here.writeFile(QStringLiteral("already.txt"), QByteArray("mine")));

    BrowserPaneController* pane = paneOn(mountLocal(here.path()));
    QVERIFY(pane);
    QSignalSpy failed(pane, &BrowserPaneController::operationFailed);
    const int queued = queuedSoFar();

    // A drag that ended over the folder it started in. Asking the user about a
    // collision with itself would be the alternative.
    pane->dropHere({ QUrl::fromLocalFile(here.absolute(QStringLiteral("already.txt"))).toString() });

    QCOMPARE(failed.count(), 0);
    QCOMPARE(queuedSoFar(), queued);
    QCOMPARE(contentsOf(here.absolute(QStringLiteral("already.txt"))), QByteArray("mine"));
}

void TestBrowserPaneController::aCollidingNameIsReportedBeforeAnythingIsWritten()
{
    TempTree here;
    TempTree downloads;
    QVERIFY(here.isValid());
    QVERIFY(downloads.isValid());
    QVERIFY(here.writeFile(QStringLiteral("report.txt"), QByteArray("the one already here")));
    QVERIFY(downloads.writeFile(QStringLiteral("report.txt"), QByteArray("the dropped one")));
    QVERIFY(downloads.writeFile(QStringLiteral("notes.txt"), QByteArray("no clash")));

    BrowserPaneController* pane = paneOn(mountLocal(here.path()));
    QVERIFY(pane);
    const int queued = queuedSoFar();

    const QVariantMap plan
        = pane->dropPlan({ QUrl::fromLocalFile(downloads.absolute(QStringLiteral("report.txt"))).toString(),
            QUrl::fromLocalFile(downloads.absolute(QStringLiteral("notes.txt"))).toString() });

    QCOMPARE(plan.value(QStringLiteral("count")).toInt(), 2);
    QCOMPARE(plan.value(QStringLiteral("collisions")).toStringList(),
        QStringList { QStringLiteral("report.txt") });
    QCOMPARE(plan.value(QStringLiteral("targetPath")).toString(), pane->displayPath());
    QVERIFY(plan.value(QStringLiteral("writable")).toBool());
    // Asked, and nothing written by the asking: a plan is a question.
    QCOMPARE(contentsOf(here.absolute(QStringLiteral("report.txt"))), QByteArray("the one already here"));
    QCOMPARE(queuedSoFar(), queued);
}

void TestBrowserPaneController::stopKeepsTheFileThatWasAlreadyThere()
{
    TempTree here;
    TempTree downloads;
    QVERIFY(here.writeFile(QStringLiteral("report.txt"), QByteArray("the one already here")));
    QVERIFY(downloads.writeFile(QStringLiteral("report.txt"), QByteArray("the dropped one")));

    BrowserPaneController* pane = paneOn(mountLocal(here.path()));
    QVERIFY(pane);
    QSignalSpy failed(pane, &BrowserPaneController::operationFailed);

    pane->dropHere({ QUrl::fromLocalFile(downloads.absolute(QStringLiteral("report.txt"))).toString() },
        QStringLiteral("stop"));

    // Stopped, and it says so: a refusal nobody is told about is the same as a
    // file quietly lost.
    QVERIFY(waitFor([&failed] { return failed.count() > 0; }));
    QCOMPARE(contentsOf(here.absolute(QStringLiteral("report.txt"))), QByteArray("the one already here"));
}

void TestBrowserPaneController::skipKeepsTheFileThatWasAlreadyThere()
{
    TempTree here;
    TempTree downloads;
    QVERIFY(here.writeFile(QStringLiteral("report.txt"), QByteArray("the one already here")));
    QVERIFY(downloads.writeFile(QStringLiteral("report.txt"), QByteArray("the dropped one")));
    QVERIFY(downloads.writeFile(QStringLiteral("notes.txt"), QByteArray("no clash")));

    BrowserPaneController* pane = paneOn(mountLocal(here.path()));
    QVERIFY(pane);

    pane->dropHere({ QUrl::fromLocalFile(downloads.absolute(QStringLiteral("report.txt"))).toString(),
                       QUrl::fromLocalFile(downloads.absolute(QStringLiteral("notes.txt"))).toString() },
        QStringLiteral("skip"));

    // The one that did not clash still arrives: skip is about the collision, not
    // about the drop.
    QVERIFY(waitFor([&here] { return QFileInfo::exists(here.absolute(QStringLiteral("notes.txt"))); }));
    QCOMPARE(contentsOf(here.absolute(QStringLiteral("report.txt"))), QByteArray("the one already here"));
}

void TestBrowserPaneController::overwriteReplacesIt()
{
    TempTree here;
    TempTree downloads;
    QVERIFY(here.writeFile(QStringLiteral("report.txt"), QByteArray("the one already here")));
    QVERIFY(downloads.writeFile(QStringLiteral("report.txt"), QByteArray("the dropped one")));

    BrowserPaneController* pane = paneOn(mountLocal(here.path()));
    QVERIFY(pane);

    pane->dropHere({ QUrl::fromLocalFile(downloads.absolute(QStringLiteral("report.txt"))).toString() },
        QStringLiteral("overwrite"));

    QVERIFY(waitFor([&here] {
        return contentsOf(here.absolute(QStringLiteral("report.txt"))) == QByteArray("the dropped one");
    }));
}

void TestBrowserPaneController::aReadOnlyDestinationRefusesAndQueuesNothing()
{
    TempTree here;
    TempTree downloads;
    QVERIFY(downloads.writeFile(QStringLiteral("invoice.pdf"), QByteArray("%PDF")));

    BrowserPaneController* pane = paneOn(mountLocal(here.path(), false));
    QVERIFY(pane);
    QVERIFY(!pane->isWritable());
    QSignalSpy failed(pane, &BrowserPaneController::operationFailed);
    const int queued = queuedSoFar();

    pane->dropHere({ QUrl::fromLocalFile(downloads.absolute(QStringLiteral("invoice.pdf"))).toString() });

    // Refused before anything is queued, and in the wording a transfer to a
    // read-only destination already uses.
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("read-only")));
    QCOMPARE(queuedSoFar(), queued);
    QVERIFY(!QFileInfo::exists(here.absolute(QStringLiteral("invoice.pdf"))));
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

BrowserPaneController* TestBrowserPaneController::paneOnCheckout(const QString& uri)
{
    BrowserPaneController* pane = makePane();
    pane->navigateTo(uri);
    if (!waitFor([pane] { return !pane->isLoading(); }))
        return nullptr;
    // The git read is a task of its own and finishes after the listing does, so
    // waiting for the listing alone would assert against a band that has not been
    // told anything yet.
    waitFor([pane] { return pane->repository()->isPresent(); });
    return pane;
}

void TestBrowserPaneController::aFolderInsideACheckoutIsReportedWithItsBranch()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("src/a.txt"), "a"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root + QStringLiteral("/src"));
    QVERIFY(pane);

    QVERIFY(pane->repository()->isPresent());
    QCOMPARE(pane->repository()->branch(), QStringLiteral("main"));
    QCOMPARE(pane->repository()->headText(), QStringLiteral("main"));
    QCOMPARE(QFileInfo(pane->repository()->root()).canonicalFilePath(),
        QFileInfo(work.path()).canonicalFilePath());
    RepositoryCache::shared().clear();
}

void TestBrowserPaneController::aFolderOutsideAnyCheckoutReportsNothing()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("plain")));

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = makePane();
    pane->navigateTo(root + QStringLiteral("/plain"));
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    drainEvents();

    // Nothing, and nothing is what the band binds to: absent rather than an empty
    // strip reserving height above the listing.
    QVERIFY(!pane->repository()->isPresent());
    QVERIFY(pane->repository()->headText().isEmpty());
    RepositoryCache::shared().clear();
}

void TestBrowserPaneController::leavingACheckoutTakesTheAnswerWithIt()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("elsewhere")));
    GitFixture checkout(QDir(work.path()).filePath(QStringLiteral("checkout")));
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("checkout")));
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root + QStringLiteral("/checkout"));
    QVERIFY(pane);
    QVERIFY(pane->repository()->isPresent());

    pane->navigateTo(root + QStringLiteral("/elsewhere"));
    QVERIFY(waitFor([pane] { return !pane->repository()->isPresent(); }));
    QVERIFY(pane->repository()->branch().isEmpty());
    RepositoryCache::shared().clear();
}

void TestBrowserPaneController::aCheckoutReachedOverADriveThatIsNotLocalReportsNothing()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    // A real checkout on disk, reached through a drive that is not a real
    // filesystem: the memory drive is given the same absolute path, so anything
    // that took the uri's path rather than its local path would find the
    // repository and put a band up. libgit2 wants a path a kernel understands,
    // and pulling `.git` across a network drive to decorate a listing is the
    // trade ADR-0041 refused.
    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    m_fs->addFile(work.path() + QStringLiteral("/a.txt"), QByteArray("a"));
    BrowserPaneController* pane = makePane();
    pane->navigateTo(QStringLiteral("mem://") + work.path());
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() > 0; }));
    drainEvents();

    QVERIFY(!pane->repository()->isPresent());
    // And nothing was even asked. A band that stays away because the answer was
    // thrown out on arrival would still have walked a work tree over a network
    // drive to get it.
    for (const Task* task : m_tasks->tasks())
        QVERIFY2(!qobject_cast<const ReadRepositoryTask*>(task),
            "no git read belongs on a drive that is not local");
    RepositoryCache::shared().clear();
}

void TestBrowserPaneController::twoCheckoutsInOnePaneEachReportTheirOwnBranch()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("one")));
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("two")));

    GitFixture one(QDir(work.path()).filePath(QStringLiteral("one")));
    GitFixture two(QDir(work.path()).filePath(QStringLiteral("two")));
    QVERIFY(one.init(QStringLiteral("main")));
    QVERIFY(two.init(QStringLiteral("release")));
    QVERIFY(one.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(two.writeFile(QStringLiteral("b.txt"), "b"));
    QVERIFY(!one.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(!two.commitAll(QStringLiteral("first")).isEmpty());

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root + QStringLiteral("/one"));
    QVERIFY(pane);
    QCOMPARE(pane->repository()->branch(), QStringLiteral("main"));

    pane->navigateTo(root + QStringLiteral("/two"));
    QVERIFY(waitFor([pane] { return pane->repository()->branch() == QStringLiteral("release"); }));
    QVERIFY(pane->repository()->isPresent());
    RepositoryCache::shared().clear();
}

void TestBrowserPaneController::aDirtyCheckoutIsCountedAndACleanOneSaysSo()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("src/a.txt"), "a"));
    QVERIFY(checkout.writeFile(QStringLiteral("src/b.txt"), "b"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    const QString root = mountLocal(work.path());
    BrowserPaneController* clean = paneOnCheckout(root + QStringLiteral("/src"));
    QVERIFY(clean);
    QVERIFY(waitFor([clean] { return clean->repository()->isStatusKnown(); }));
    QCOMPARE(clean->repository()->changedCount(), 0);
    // "clean", not "0 changed": a count of nought is a sentence about arithmetic,
    // and what somebody wants to know is whether there is anything to deal with.
    QCOMPARE(clean->repository()->changesText(), QStringLiteral("clean"));

    QVERIFY(checkout.writeFile(QStringLiteral("src/a.txt"), "edited"));
    QVERIFY(checkout.writeFile(QStringLiteral("src/fresh.txt"), "new"));
    RepositoryStatusCache::shared().clear();

    BrowserPaneController* dirty = paneOnCheckout(root + QStringLiteral("/src"));
    QVERIFY(dirty);
    QVERIFY(waitFor([dirty] { return dirty->repository()->isStatusKnown(); }));
    QCOMPARE(dirty->repository()->changedCount(), 2);
    QCOMPARE(dirty->repository()->changesText(), QStringLiteral("2 changed"));
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::movingBetweenFoldersInOneCheckoutWalksItOnce()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("src/a.txt"), "a"));
    QVERIFY(checkout.writeFile(QStringLiteral("tests/b.txt"), "b"));
    QVERIFY(checkout.writeFile(QStringLiteral("docs/c.txt"), "c"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(checkout.writeFile(QStringLiteral("src/a.txt"), "edited"));

    // Counted as they are submitted rather than by looking at the list afterwards:
    // a task that has finished and been retired would not be in it.
    int walks = 0;
    connect(m_tasks, &TaskManager::taskAppended, this, [&walks](Task* task) {
        if (qobject_cast<ReadStatusTask*>(task))
            ++walks;
    });

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root + QStringLiteral("/src"));
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
    QCOMPARE(walks, 1);
    QCOMPARE(pane->repository()->changedCount(), 1);

    // Two more folders in the same checkout. Each one is a navigation, a listing
    // and a branch read; none of them is a second stat of the whole work tree.
    for (const QString& folder : { QStringLiteral("/tests"), QStringLiteral("/docs") }) {
        pane->navigateTo(root + folder);
        QVERIFY(
            waitFor([pane, folder] { return !pane->isLoading() && pane->currentUri().endsWith(folder); }));
        QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
        QCOMPARE(pane->repository()->changedCount(), 1);
    }

    QCOMPARE(walks, 1);
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::leavingACheckoutTakesTheCountWithIt()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("elsewhere")));
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("checkout")));
    GitFixture checkout(QDir(work.path()).filePath(QStringLiteral("checkout")));
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "edited"));

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root + QStringLiteral("/checkout"));
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
    QCOMPARE(pane->repository()->changesText(), QStringLiteral("1 changed"));

    // Out of the checkout entirely. The count has to go with the branch -- a band
    // that kept saying "1 changed" about a folder in no repository would be the
    // one failure mode this feature has.
    pane->navigateTo(root + QStringLiteral("/elsewhere"));
    QVERIFY(waitFor([pane] { return !pane->repository()->isPresent(); }));
    QVERIFY(!pane->repository()->isStatusKnown());
    QVERIFY(pane->repository()->changesText().isEmpty());
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::walkingAwayFromACheckoutAbandonsItsWalk()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("elsewhere")));
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("checkout")));
    GitFixture checkout(QDir(work.path()).filePath(QStringLiteral("checkout")));
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "edited"));

    QList<ReadStatusTask*> walks;
    connect(m_tasks, &TaskManager::taskAppended, this, [&walks](Task* task) {
        if (auto* walk = qobject_cast<ReadStatusTask*>(task))
            walks.append(walk);
    });

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = makePane();
    pane->navigateTo(root + QStringLiteral("/checkout"));
    QVERIFY(waitFor([&walks] { return !walks.isEmpty(); }));

    // Straight out of the checkout, while its walk is in flight or has just landed.
    pane->navigateTo(root + QStringLiteral("/elsewhere"));
    QVERIFY(waitFor(
        [pane] { return !pane->isLoading() && pane->currentUri().endsWith(QStringLiteral("/elsewhere")); }));

    // The task ends either way -- cancelled if the walk was still going, succeeded
    // if it beat the navigation. Which of the two happened is a race with the disk
    // and asserting one of them would be asserting how fast this machine is.
    QVERIFY(waitFor([&walks] { return walks.constFirst()->isFinished(); }));

    // What is not a race is the answer: whichever way the walk ended, nothing about
    // the checkout may be on a band that is showing a folder outside it.
    //
    // Waited for rather than read once, because what clears the band is a read of
    // the *new* folder -- a task of its own, submitted after that folder's listing
    // lands, and not the walk above. Reading straight after the walk finished
    // asserted the outcome before the event that produces it, which passed on an
    // idle machine and failed on a loaded one.
    QVERIFY2(waitFor([pane] { return !pane->repository()->isPresent(); }),
        "the band went on naming the checkout after the pane had left it");
    QVERIFY(!pane->repository()->isStatusKnown());
    QVERIFY(pane->repository()->changesText().isEmpty());
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

QString TestBrowserPaneController::markFor(BrowserPaneController* pane, const QString& name) const
{
    FileListModel* files = pane->files();
    for (int row = 0; row < files->rowCount(); ++row) {
        const QModelIndex at = files->index(row, 0);
        if (at.data(FileListModel::NameRole).toString() == name)
            return at.data(FileListModel::GitMarkRole).toString();
    }
    return QStringLiteral("<no such row>");
}

void TestBrowserPaneController::everyKindOfChangeCarriesItsOwnMarker()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("edited.txt"), "before"));
    QVERIFY(checkout.writeFile(QStringLiteral("gone/removed.txt"), "gone soon"));
    QVERIFY(checkout.writeFile(QStringLiteral("gone/kept.txt"), "still here"));
    QVERIFY(checkout.writeFile(QStringLiteral("moved.txt"), "content that stays the same\n"));
    QVERIFY(checkout.writeFile(QStringLiteral("untouched.txt"), "still here"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    QVERIFY(checkout.writeFile(QStringLiteral("edited.txt"), "after"));
    QVERIFY(checkout.removeFile(QStringLiteral("gone/removed.txt")));
    // A rename is only a rename once it is staged as one: git works it out by
    // matching content between the index and the last commit.
    QVERIFY(checkout.removeFile(QStringLiteral("moved.txt")));
    QVERIFY(checkout.writeFile(QStringLiteral("elsewhere.txt"), "content that stays the same\n"));
    QVERIFY(checkout.stageAll());
    // After the staging, so that it stays untracked rather than becoming added --
    // which is the difference between `??` and `A`.
    QVERIFY(checkout.writeFile(QStringLiteral("fresh.txt"), "new and unstaged"));

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root);
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));

    QCOMPARE(markFor(pane, QStringLiteral("edited.txt")), QStringLiteral("M"));
    QCOMPARE(markFor(pane, QStringLiteral("elsewhere.txt")), QStringLiteral("R"));
    QCOMPARE(markFor(pane, QStringLiteral("fresh.txt")), QStringLiteral("??"));
    // Nothing happened to this one, so there is no mark and therefore no column.
    QCOMPARE(markFor(pane, QStringLiteral("untouched.txt")), QString());

    // A deletion is the one state a listing of what is on disk cannot put on its
    // own row: the file is not there, so there is no row. What it can say is that
    // the folder it was in has changed, which is what the roll-up is for -- and the
    // band still counts it. Giving a deleted file a row of its own is a decision
    // about what a listing is, not a marker; it is MOLE-184.
    pane->navigateTo(root + QStringLiteral("/gone"));
    QVERIFY(waitFor(
        [pane] { return !pane->isLoading() && pane->currentUri().endsWith(QStringLiteral("/gone")); }));
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
    QCOMPARE(markFor(pane, QStringLiteral("removed.txt")), QStringLiteral("<no such row>"));
    QCOMPARE(markFor(pane, QStringLiteral("kept.txt")), QString());

    pane->navigateTo(root);
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
    QCOMPARE(markFor(pane, QStringLiteral("gone")), QStringLiteral("•"));
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::aConflictedFileSaysSoRatherThanModified()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("shared.txt"), "base\n"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    // The same file changed two ways, then a rebase walked into it and stopped.
    QVERIFY(checkout.createBranch(QStringLiteral("topic")));
    QVERIFY(checkout.checkoutBranch(QStringLiteral("topic")));
    QVERIFY(checkout.writeFile(QStringLiteral("shared.txt"), "topic wrote this\n"));
    QVERIFY(!checkout.commitAll(QStringLiteral("on topic")).isEmpty());
    QVERIFY(checkout.checkoutBranch(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("shared.txt"), "main wrote this\n"));
    QVERIFY(!checkout.commitAll(QStringLiteral("on main")).isEmpty());
    QVERIFY(checkout.beginRebase(QStringLiteral("topic"), QStringLiteral("main")));

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root);
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));

    // A conflicted path also carries modified bits. "Modified" is the less urgent
    // half of that truth and would send somebody to the wrong tool.
    QCOMPARE(markFor(pane, QStringLiteral("shared.txt")), QStringLiteral("U"));
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::aFolderSaysThatSomethingInsideItChanged()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("src/deep/down/here.txt"), "before"));
    QVERIFY(checkout.writeFile(QStringLiteral("quiet/nothing.txt"), "untouched"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(checkout.writeFile(QStringLiteral("src/deep/down/here.txt"), "after"));

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root);
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));

    // At the top of the checkout the only rows are directories. Without the
    // roll-up this listing would look completely clean over a tree with an edit
    // in it.
    QCOMPARE(markFor(pane, QStringLiteral("src")), QStringLiteral("•"));
    QCOMPARE(markFor(pane, QStringLiteral("quiet")), QString());

    // And at every level down to the file, because any of them can be the folder
    // on screen.
    for (const QString& folder :
        { QStringLiteral("/src"), QStringLiteral("/src/deep"), QStringLiteral("/src/deep/down") }) {
        pane->navigateTo(root + folder);
        QVERIFY(
            waitFor([pane, folder] { return !pane->isLoading() && pane->currentUri().endsWith(folder); }));
        QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
    }
    // The last hop landed in the folder holding the file itself.
    QCOMPARE(markFor(pane, QStringLiteral("here.txt")), QStringLiteral("M"));
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::anIgnoredRowCarriesNoMarker()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral(".gitignore"), "*.log\n"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(checkout.writeFile(QStringLiteral("noise.log"), "ignored"));
    QVERIFY(checkout.writeFile(QStringLiteral("real.txt"), "counted"));

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root);
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));

    QCOMPARE(markFor(pane, QStringLiteral("real.txt")), QStringLiteral("??"));
    // The row is in the listing -- Mole shows it, git does not care about it.
    QCOMPARE(markFor(pane, QStringLiteral("noise.log")), QString());
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::aFolderOutsideAnyCheckoutCarriesNoMarkers()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("plain")));
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("checkout")));
    GitFixture checkout(QDir(work.path()).filePath(QStringLiteral("checkout")));
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "edited"));

    QFile plain(QDir(work.path()).filePath(QStringLiteral("plain/a.txt")));
    QVERIFY(plain.open(QIODevice::WriteOnly));
    plain.write("not in any repository");
    plain.close();

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root + QStringLiteral("/checkout"));
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
    QCOMPARE(markFor(pane, QStringLiteral("a.txt")), QStringLiteral("M"));

    // The same file name, one directory across, in no work tree. The marks have to
    // go with the repository rather than linger on rows that look similar.
    pane->navigateTo(root + QStringLiteral("/plain"));
    QVERIFY(waitFor([pane] { return !pane->repository()->isPresent(); }));
    QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    QCOMPARE(markFor(pane, QStringLiteral("a.txt")), QString());
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::markersSurviveSortingAndFiltering()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    for (const QString& name :
        { QStringLiteral("alpha.txt"), QStringLiteral("beta.txt"), QStringLiteral("gamma.txt") }) {
        QVERIFY(checkout.writeFile(name, "before"));
    }
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(checkout.writeFile(QStringLiteral("beta.txt"), "after"));

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root);
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
    QCOMPARE(markFor(pane, QStringLiteral("beta.txt")), QStringLiteral("M"));

    // Reordered. The annotations are keyed by uri, not by row, so a row that moves
    // takes its mark with it -- which is the whole reason for that shape.
    pane->files()->setSortDescending(true);
    QCOMPARE(markFor(pane, QStringLiteral("beta.txt")), QStringLiteral("M"));
    pane->files()->setSortKey(FileListModel::SortKey::Modified);
    QCOMPARE(markFor(pane, QStringLiteral("beta.txt")), QStringLiteral("M"));

    // And filtered down to the one row.
    pane->files()->setFilterText(QStringLiteral("beta"));
    QCOMPARE(pane->files()->rowCount(), 1);
    QCOMPARE(markFor(pane, QStringLiteral("beta.txt")), QStringLiteral("M"));
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::acopyOverATrackedFileMarksItWithoutNavigating()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("source")));
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("checkout")));
    GitFixture checkout(QDir(work.path()).filePath(QStringLiteral("checkout")));
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("tracked.txt"), "committed contents\n"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    QFile replacement(QDir(work.path()).filePath(QStringLiteral("source/tracked.txt")));
    QVERIFY(replacement.open(QIODevice::WriteOnly));
    replacement.write("something else entirely\n");
    replacement.close();

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root + QStringLiteral("/checkout"));
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
    QCOMPARE(pane->repository()->changesText(), QStringLiteral("clean"));
    QCOMPARE(markFor(pane, QStringLiteral("tracked.txt")), QString());

    // Mole's own copy, over a file git is tracking. Nothing is navigated: a listing
    // that still calls the file unchanged after this is the failure this ticket is
    // about.
    pane->dropHere(
        { QUrl::fromLocalFile(QDir(work.path()).filePath(QStringLiteral("source/tracked.txt"))).toString() },
        QStringLiteral("overwrite"));

    QVERIFY(waitFor(
        [this, pane] { return markFor(pane, QStringLiteral("tracked.txt")) == QStringLiteral("M"); }));
    QCOMPARE(pane->repository()->changesText(), QStringLiteral("1 changed"));
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::deletingATrackedFileIsNoticed()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("doomed.txt"), "here for now\n"));
    QVERIFY(checkout.writeFile(QStringLiteral("kept.txt"), "staying\n"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root);
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));
    QCOMPARE(pane->repository()->changesText(), QStringLiteral("clean"));

    // Delete it through Mole, with the cursor on it.
    for (int row = 0; row < pane->files()->rowCount(); ++row) {
        if (pane->files()->index(row, 0).data(FileListModel::NameRole).toString()
            == QStringLiteral("doomed.txt")) {
            pane->setCurrentIndex(row);
        }
    }
    QCOMPARE(pane->currentName(), QStringLiteral("doomed.txt"));
    pane->deleteTargets();

    // The count has to notice, or the band would go on calling the checkout clean
    // after Mole itself removed a tracked file from it.
    QVERIFY(waitFor([pane] { return pane->repository()->changesText() == QStringLiteral("1 changed"); }));

    // And while the row is still listed -- a pane on its own does not reload itself,
    // BrowserController does that when it hears the event -- the row carries the
    // deletion. Which is worth asserting because it is the one case where `D` does
    // have a row to land on.
    QCOMPARE(markFor(pane, QStringLiteral("doomed.txt")), QStringLiteral("D"));

    // Once the listing is reloaded the row goes, because a listing shows what is on
    // disk. From then on the deletion lives on the folder above and in the count;
    // whether to give it a row of its own is MOLE-184.
    pane->refresh();
    QVERIFY(waitFor([this, pane] {
        return !pane->isLoading()
            && markFor(pane, QStringLiteral("doomed.txt")) == QStringLiteral("<no such row>");
    }));
    QVERIFY(waitFor([pane] { return pane->repository()->changesText() == QStringLiteral("1 changed"); }));
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::aCommitMadeOutsideMoleClearsTheMarkers()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "first"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(checkout.writeFile(QStringLiteral("a.txt"), "edited but not committed"));

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root);
    QVERIFY(pane);
    QVERIFY(waitFor([this, pane] { return markFor(pane, QStringLiteral("a.txt")) == QStringLiteral("M"); }));

    // Committed by something that is not Mole and announces nothing on the event
    // bus, which is what `git commit` in the terminal panel or in another window
    // looks like from here. Only the watcher on the repository directory can see it.
    QVERIFY(!checkout.commitAll(QStringLiteral("committed elsewhere")).isEmpty());

    QVERIFY2(waitFor([this, pane] { return markFor(pane, QStringLiteral("a.txt")).isEmpty(); }),
        "the marker outlived the commit that made it untrue");
    QVERIFY(waitFor([pane] { return pane->repository()->changesText() == QStringLiteral("clean"); }));
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::aBurstOfWritesIsOneWalkRatherThanOnePerFile()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("source")));
    QVERIFY(QDir(work.path()).mkpath(QStringLiteral("checkout")));
    GitFixture checkout(QDir(work.path()).filePath(QStringLiteral("checkout")));
    QVERIFY(checkout.init(QStringLiteral("main")));
    QVERIFY(checkout.writeFile(QStringLiteral("kept.txt"), "x"));
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    // Two hundred files to copy in, which is two hundred finished tasks and two
    // hundred announcements.
    const int files = 200;
    QStringList urls;
    for (int i = 0; i < files; ++i) {
        const QString path
            = QDir(work.path()).filePath(QStringLiteral("source/file%1.txt").arg(i, 3, 10, QLatin1Char('0')));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("payload");
        file.close();
        urls.append(QUrl::fromLocalFile(path).toString());
    }

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root + QStringLiteral("/checkout"));
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));

    int walks = 0;
    connect(m_tasks, &TaskManager::taskAppended, this, [&walks](Task* task) {
        if (qobject_cast<ReadStatusTask*>(task))
            ++walks;
    });

    pane->dropHere(urls, QStringLiteral("overwrite"));
    QVERIFY(waitFor([pane, files] { return pane->repository()->changedCount() == files; }, 30000));

    // Counted, not timed. A walk per finished task would be two hundred; what the
    // floor has to deliver is a handful.
    QVERIFY2(walks > 0, "the copy left the status stale and nothing walked again");
    QVERIFY2(walks <= 10,
        qPrintable(QStringLiteral("%1 walks for %2 files: the floor is not collecting the burst")
                       .arg(walks)
                       .arg(files)));
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

void TestBrowserPaneController::aRefreshLeavesTheCursorAndTheTicksAlone()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    QTemporaryDir work;
    QVERIFY(work.isValid());
    GitFixture checkout(work.path());
    QVERIFY(checkout.init(QStringLiteral("main")));
    for (const QString& name : { QStringLiteral("alpha.txt"), QStringLiteral("beta.txt"),
             QStringLiteral("gamma.txt"), QStringLiteral("delta.txt") }) {
        QVERIFY(checkout.writeFile(name, "committed"));
    }
    QVERIFY(!checkout.commitAll(QStringLiteral("first")).isEmpty());

    const QString root = mountLocal(work.path());
    BrowserPaneController* pane = paneOnCheckout(root);
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return pane->repository()->isStatusKnown(); }));

    // Cursor on the third row, and two rows ticked.
    pane->setCurrentIndex(2);
    const QString onCursor = pane->currentName();
    QVERIFY(!onCursor.isEmpty());
    pane->setCurrentIndex(0);
    pane->toggleSelectionAndAdvance();
    pane->toggleSelectionAndAdvance();
    const QStringList ticked = pane->files()->selectedUris();
    QCOMPARE(ticked.size(), 2);
    pane->setCurrentIndex(2);

    // Something changes the tree from outside, so a walk lands while the user is
    // sitting on a row with a selection made. Staged as well as written, because
    // staging is what touches the repository directory -- and the watcher on that
    // directory is the only thing here that notices a change Mole did not make.
    QVERIFY(checkout.writeFile(QStringLiteral("alpha.txt"), "edited from elsewhere"));
    QVERIFY(checkout.stageAll());
    QVERIFY(
        waitFor([this, pane] { return markFor(pane, QStringLiteral("alpha.txt")) == QStringLiteral("M"); }));

    // The annotations changed; the rows did not. A refresh that reset the model would
    // drop both of these, and losing a selection somebody built by hand is worse than
    // a stale marker.
    QCOMPARE(pane->currentIndex(), 2);
    QCOMPARE(pane->currentName(), onCursor);
    QCOMPARE(pane->files()->selectedUris(), ticked);
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
}

/// One query for the folder, never one per row: a folder of five thousand files
/// must not become five thousand lookups on the path that draws.
void TestBrowserPaneController::oneQueryMarksTheWholeFolderHoweverManyRowsItHas()
{
    BrowserPaneController* pane = paneOnOfferingDrive();
    QVERIFY(pane);

    QVERIFY(waitFor([this] { return m_offering->folderQueryCallCount() == 1; }));
    QVERIFY(waitFor([pane] { return markedRows(pane) == 1; }));

    // report.txt has versions and a link; untouched.txt was told it has neither.
    QVERIFY(hasDriveMark(pane, QStringLiteral("report.txt")));
    QVERIFY(!hasDriveMark(pane, QStringLiteral("untouched.txt")));
    QCOMPARE(m_offering->folderQueryCallCount(), 1);
}

void TestBrowserPaneController::aFolderOfFiveThousandIsStillOneQuery()
{
    BrowserPaneController* pane = paneOnOfferingDrive();
    QVERIFY(pane);
    QVERIFY(waitFor([this] { return m_offering->folderQueryCallCount() == 1; }));

    for (int i = 0; i < 5000; ++i) {
        const QString name = QStringLiteral("/deep/f%1.txt").arg(i);
        m_offering->memory()->addFile(name, QByteArray("x"));
        m_offering->setLinkable(name, i % 2 == 0);
    }

    const int before = m_offering->folderQueryCallCount();
    pane->navigateTo(QStringLiteral("mem://offering/deep"));
    QVERIFY(waitFor([pane] { return !pane->isLoading() && pane->files()->rowCount() == 5000; }));
    QVERIFY(waitFor([pane] { return markedRows(pane) == 2500; }));

    // One. Not one per row, not one per screenful.
    QCOMPARE(m_offering->folderQueryCallCount() - before, 1);
}

/// No task, no query, no work: a drive with nothing to offer browses exactly as
/// it did before any of this existed.
void TestBrowserPaneController::aDriveWithNothingToOfferIsNotAskedAboutTheFolderAtAll()
{
    BrowserPaneController* pane = paneOn(QStringLiteral("mem:///docs"));
    QVERIFY(pane);
    QVERIFY(waitFor([this] { return m_fs->offers().isKnown(); }));
    drainEvents();

    QCOMPARE(markedRows(pane), 0);
    QVERIFY2(m_fs->offers().ids.isEmpty(), "the fixture drive offers nothing");
}

/// A result for a folder nobody is looking at any more is discarded, not drawn.
void TestBrowserPaneController::navigatingAwayAbandonsTheFolderQuery()
{
    BrowserPaneController* pane = paneOnOfferingDrive();
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return markedRows(pane) == 1; }));

    m_offering->memory()->addFile(QStringLiteral("/deep/slow.txt"), QByteArray("x"));
    m_offering->setActionDelayMs(60000);

    pane->navigateTo(QStringLiteral("mem://offering/deep"));
    // Waited on the condition: the query has to be inside the drive before
    // navigating away means anything.
    QVERIFY(waitFor([this] { return m_offering->isWorking(); }));

    pane->navigateTo(QStringLiteral("mem://offering/"));
    QVERIFY2(waitFor([this] { return !m_offering->isWorking(); }),
        "leaving the folder has to call the query off rather than wait it out");
}

/// The row under the cursor decides what is on offer, so moving the cursor to a
/// file the drive has nothing for empties the list rather than leaving the last
/// file's actions behind.
void TestBrowserPaneController::whatTheDriveCanDoFollowsTheCursor()
{
    BrowserPaneController* pane = paneOnOfferingDrive();
    QVERIFY(pane);

    QVERIFY(waitFor([pane] { return pane->driveActions().size() == 2; }));
    const QVariantList offered = pane->driveActions();
    QCOMPARE(
        offered.first().toMap().value(QStringLiteral("id")).toString(), OfferingFileSystem::linkAction());
    QCOMPARE(
        offered.last().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Earlier versions"));

    pane->setCurrentIndex(rowOf(pane, QStringLiteral("untouched.txt")));
    QVERIFY(waitFor([pane] { return pane->driveActions().isEmpty(); }));
}

void TestBrowserPaneController::anActionAnsweringWithTextSaysWhenItStopsWorking()
{
    BrowserPaneController* pane = paneOnOfferingDrive();
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return !pane->driveActions().isEmpty(); }));

    QSignalSpy answered(pane, &BrowserPaneController::driveActionText);
    pane->invokeDriveAction(OfferingFileSystem::linkAction());
    QVERIFY(waitFor([&answered] { return answered.count() == 1; }));

    // The drive's own title, so somebody who picked one of two entries knows
    // which answered.
    QCOMPARE(answered.first().at(0).toString(), QStringLiteral("Copy a temporary link"));
    QVERIFY(answered.first().at(1).toString().contains(QStringLiteral("report.txt")));
    QVERIFY2(!answered.first().at(2).toString().isEmpty(), "a link that expires has to say when");
}

/// Each entry opens as an ordinary file, which is what an earlier version of one
/// is -- so it goes out the same way activating a row does.
void TestBrowserPaneController::anActionAnsweringWithUrisOffersThemToOpen()
{
    BrowserPaneController* pane = paneOnOfferingDrive();
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return !pane->driveActions().isEmpty(); }));

    QSignalSpy answered(pane, &BrowserPaneController::driveActionUris);
    pane->invokeDriveAction(OfferingFileSystem::versionsAction());
    QVERIFY(waitFor([&answered] { return answered.count() == 1; }));

    const QVariantList choices = answered.first().at(1).toList();
    QCOMPARE(choices.size(), 2);
    // Labelled by the drive's own token: it is the only thing that tells one
    // state of a file from another, and this layer must not invent a prettier
    // one it cannot know is true.
    QCOMPARE(choices.first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("v1"));

    QSignalSpy opened(pane, &BrowserPaneController::fileActivated);
    pane->openUri(choices.first().toMap().value(QStringLiteral("uri")).toString());
    QCOMPARE(opened.count(), 1);
    QCOMPARE(VfsUri::fromString(opened.first().at(0).toString()).version(), QStringLiteral("v1"));
}

void TestBrowserPaneController::anActionThatFailsSaysWhichOne()
{
    BrowserPaneController* pane = paneOnOfferingDrive();
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return !pane->driveActions().isEmpty(); }));

    m_offering->setActionFault(VfsError::NetworkError);

    QSignalSpy failed(pane, &BrowserPaneController::operationFailed);
    pane->invokeDriveAction(OfferingFileSystem::linkAction());
    QVERIFY(waitFor([&failed] { return failed.count() == 1; }));

    const QString message = failed.first().at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("Copy a temporary link")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("would not answer")), qPrintable(message));
}

/// A menu that has gone stale must not be able to reach the drive with an id it
/// never offered for the row the cursor is actually on.
void TestBrowserPaneController::anIdTheDriveNeverOfferedIsNotHandedToIt()
{
    BrowserPaneController* pane = paneOnOfferingDrive();
    QVERIFY(pane);
    QVERIFY(waitFor([pane] { return !pane->driveActions().isEmpty(); }));

    const int before = m_offering->invokeCallCount();
    pane->invokeDriveAction(QStringLiteral("org.mole.test.never-offered"));
    drainEvents();
    QCOMPARE(m_offering->invokeCallCount(), before);
}

/// A drive's extra capabilities depend on what it was pointed at, so they are
/// discovered rather than compiled in -- and this is the moment they are needed
/// and the moment the drive is already being called anyway. See ADR-0076.
void TestBrowserPaneController::openingAFolderAsksTheDriveWhatItCanDo()
{
    m_fs->setOffers({ QStringLiteral("org.mole.test.versions") });
    QCOMPARE(m_fs->probeCallCount(), 0);

    QVERIFY(paneOn(QStringLiteral("mem:///docs")));

    // The probe is a task of its own and the listing does not wait for it, so
    // what is waited on is the answer rather than the listing.
    QVERIFY(waitFor([this] { return m_fs->offers().isKnown(); }));
    QCOMPARE(m_fs->probeCallCount(), 1);
    QVERIFY(m_fs->offers().has(QStringLiteral("org.mole.test.versions")));
}

void TestBrowserPaneController::walkingAroundOneDriveAsksItOnce()
{
    m_fs->setOffers({ QStringLiteral("org.mole.test.versions") });
    BrowserPaneController* pane = paneOn(QStringLiteral("mem:///docs"));
    QVERIFY(pane);
    QVERIFY(waitFor([this] { return m_fs->offers().isKnown(); }));

    const int probesAfterFirst = probeTasksSoFar();
    for (const QString& folder :
        { QStringLiteral("mem:///docs/deep"), QStringLiteral("mem:///"), QStringLiteral("mem:///docs") }) {
        pane->navigateTo(folder);
        QVERIFY(waitFor([pane] { return !pane->isLoading(); }));
    }

    QCOMPARE(m_fs->probeCallCount(), 1);
    // And no task queued for it either: a drive that has already answered is not
    // worth a job per navigation, which is what browsing is made of. Counted by
    // title rather than by how many tasks there are in total, because a
    // navigation queues other things as well.
    QCOMPARE(probeTasksSoFar(), probesAfterFirst);
}

/// Open Mole with several drives configured and browse one: there is one probe
/// rather than one per drive, and a drive nobody opens costs nothing, ever.
void TestBrowserPaneController::aDriveNobodyOpensIsNeverAsked()
{
    auto other = std::make_shared<MemoryFileSystem>();
    other->addFile(QStringLiteral("/elsewhere.txt"), QByteArray("x"));
    other->setOffers({ QStringLiteral("org.mole.test.versions") });

    Mount mount;
    mount.id = QStringLiteral("other");
    mount.displayName = QStringLiteral("other");
    mount.root = VfsUri::fromString(QStringLiteral("other:///"));
    mount.fileSystem = other;
    QVERIFY(!m_vfs->addMount(mount).isEmpty());

    m_fs->setOffers({ QStringLiteral("org.mole.test.versions") });
    QVERIFY(paneOn(QStringLiteral("mem:///docs")));
    QVERIFY(waitFor([this] { return m_fs->offers().isKnown(); }));

    QCOMPARE(m_fs->probeCallCount(), 1);
    QCOMPARE(other->probeCallCount(), 0);
    QCOMPARE(other->offers().state, DriveOffers::State::Unasked);
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
