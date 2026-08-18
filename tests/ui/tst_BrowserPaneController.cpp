#include "support/FaultyFileSystem.h"
#include "support/GitFixture.h"
#include "support/MoleTestMain.h"
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
    static QByteArray contentsOf(const QString& path);
    /// How many tasks have been submitted so far. TaskManager keeps a task after
    /// it has finished -- the strip shows what has just run -- so "queued
    /// nothing" is a number that did not change rather than a number that is zero.
    int queuedSoFar() const { return static_cast<int>(m_tasks->tasks().size()); }

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
    QVERIFY(!pane->repository()->isPresent());
    QVERIFY(!pane->repository()->isStatusKnown());
    QVERIFY(pane->repository()->changesText().isEmpty());
    RepositoryCache::shared().clear();
    RepositoryStatusCache::shared().clear();
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
