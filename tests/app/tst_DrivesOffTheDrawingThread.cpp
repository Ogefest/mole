#include "plugins/builtin/BrowserFeature.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/BulkRenameFeature.h"
#include "plugins/builtin/PreviewFeature.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/BrowserPaneController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Nothing the interface does may touch a drive on the thread that draws it.
///
/// ARCHITECTURE.md's first rule, and IFileSystem's own header says every method
/// is "only ever called from a worker thread owned by TaskManager". Seven places
/// did it anyway -- F2, F7, F3, an arrow step in a preview, a drag, bulk
/// rename's listing, an archive open and two image headers -- and each one is a
/// window that stops for as long as a stalled mount takes to give up. None of
/// them was visible on a local disk, which is why they lasted. See MOLE-360.
///
/// So the rule is enforced rather than remembered: IFileSystem::doNotCallFrom()
/// names the drawing thread and a call from it warns, the same way
/// IndexDatabase::doNotQueryFrom() has since ADR-0066. These cases walk the
/// routes and assert silence -- and `theGuardItselfNoticesADirectCall` is here
/// because a guard that has stopped working would let every one of them pass
/// without a word.
///
/// The other half is a clock: a drive that takes a second to answer must not
/// make a gesture take a second. Both halves are needed. The guard catches a
/// call from the wrong thread; the delay catches one made from the right thread
/// and *waited for* on this one.
class TestDrivesOffTheDrawingThread : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void theGuardItselfNoticesADirectCall();
    void makingAFolderDoesNotWaitForTheDrive();
    void renamingDoesNotWaitForTheDrive();
    void openingAPreviewDoesNotWaitForTheDrive();
    void aBulkRenameSelectionDoesNotWaitForTheDrive();

private:
    BrowserPaneController* pane();

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
    std::shared_ptr<MemoryFileSystem> m_slow;
};

/// What the guard says. Matched on rather than a whole line, because what
/// matters is that a drive was touched here at all.
static const QString kComplaint = QStringLiteral("Drive call on the thread that draws the window");

/// Long enough that waiting for it would be unmistakable, short enough that a
/// case which does wait still finishes.
static constexpr int kDriveDelayMs = 1500;
/// What a gesture is allowed to cost. Generous on purpose: the claim is "it did
/// not wait for the drive", not a performance figure, and a loaded machine may
/// take a moment over an event loop.
static constexpr int kGestureBudgetMs = 600;

void TestDrivesOffTheDrawingThread::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

void TestDrivesOffTheDrawingThread::init()
{
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));

    // A drive that answers everything, slowly. Contrived, and it has to be: the
    // only honest way to test what the window does while a drive is thinking is
    // to have one that thinks.
    m_slow = std::make_shared<MemoryFileSystem>();
    m_slow->addDirectory(QStringLiteral("/folder"));
    m_slow->addFile(QStringLiteral("/folder/notes.txt"), QByteArray("something to read"));
    m_slow->addFile(QStringLiteral("/folder/other.txt"), QByteArray("something else"));

    Mount mount;
    mount.id = QStringLiteral("slow");
    mount.displayName = QStringLiteral("slow");
    mount.root = VfsUri::fromString(QStringLiteral("mem://slow/"));
    mount.fileSystem = m_slow;
    QVERIFY(!m_app->services().vfs->addMount(mount).isEmpty());
}

void TestDrivesOffTheDrawingThread::cleanup()
{
    m_app.reset();
    m_slow.reset();
    m_tree.reset();
}

BrowserPaneController* TestDrivesOffTheDrawingThread::pane()
{
    auto* browser = qobject_cast<BrowserController*>(m_app->tabs()->currentController());
    return browser ? browser->activePane() : nullptr;
}

/// A guard that has stopped working would make every case below pass in silence.
void TestDrivesOffTheDrawingThread::theGuardItselfNoticesADirectCall()
{
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(kComplaint + QStringLiteral(".*stat")));
    // On purpose, from this thread, which is the one AppController named.
    const Result<FileEntry> asked
        = m_slow->stat(VfsUri::fromString(QStringLiteral("mem://slow/folder/notes.txt")));
    QVERIFY2(asked.ok(), "the call still answers -- the guard warns rather than refusing");
}

void TestDrivesOffTheDrawingThread::makingAFolderDoesNotWaitForTheDrive()
{
    BrowserPaneController* browsing = pane();
    QVERIFY(browsing);
    browsing->navigateTo(QStringLiteral("mem://slow/folder"));
    QVERIFY(waitFor([browsing] { return !browsing->isLoading(); }));

    m_slow->setOperationDelayMs(kDriveDelayMs);

    QElapsedTimer clock;
    clock.start();
    browsing->createDirectory(QStringLiteral("brand new"));
    const qint64 spent = clock.elapsed();

    QVERIFY2(spent < kGestureBudgetMs,
        qPrintable(
            QStringLiteral("F7 waited %1 ms for a drive that takes %2").arg(spent).arg(kDriveDelayMs)));
    // And it really did happen, rather than being skipped.
    QVERIFY(waitFor(
        [this] {
            return m_slow->stat(VfsUri::fromString(QStringLiteral("mem://slow/folder/brand new"))).ok();
        },
        30000));
}

void TestDrivesOffTheDrawingThread::renamingDoesNotWaitForTheDrive()
{
    BrowserPaneController* browsing = pane();
    QVERIFY(browsing);
    browsing->navigateTo(QStringLiteral("mem://slow/folder"));
    QVERIFY(waitFor([browsing] { return !browsing->isLoading() && browsing->files()->rowCount() == 2; }));

    const int row = browsing->files()->rowOfUri(QStringLiteral("mem://slow/folder/notes.txt"));
    QVERIFY(row >= 0);
    browsing->setCurrentIndex(row);

    m_slow->setOperationDelayMs(kDriveDelayMs);

    QElapsedTimer clock;
    clock.start();
    browsing->renameCurrent(QStringLiteral("renamed.txt"));
    const qint64 spent = clock.elapsed();

    QVERIFY2(spent < kGestureBudgetMs,
        qPrintable(
            QStringLiteral("F2 waited %1 ms for a drive that takes %2").arg(spent).arg(kDriveDelayMs)));
    QVERIFY(waitFor(
        [this] {
            return m_slow->stat(VfsUri::fromString(QStringLiteral("mem://slow/folder/renamed.txt"))).ok();
        },
        30000));
}

void TestDrivesOffTheDrawingThread::openingAPreviewDoesNotWaitForTheDrive()
{
    m_slow->setOperationDelayMs(kDriveDelayMs);

    QElapsedTimer clock;
    clock.start();
    m_app->previewFile(QStringLiteral("mem://slow/folder/notes.txt"));
    const qint64 spent = clock.elapsed();

    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    QVERIFY2(preview, "F3 opens a preview tab");
    QVERIFY2(spent < kGestureBudgetMs,
        qPrintable(
            QStringLiteral("F3 waited %1 ms for a drive that takes %2").arg(spent).arg(kDriveDelayMs)));

    // And the tab does settle on the file, once the drive has answered.
    QVERIFY(waitFor(
        [preview] { return preview->currentUri() == QStringLiteral("mem://slow/folder/notes.txt"); }, 30000));
}

void TestDrivesOffTheDrawingThread::aBulkRenameSelectionDoesNotWaitForTheDrive()
{
    const int row = m_app->openFeatureTab(QStringLiteral("core.bulkrename"));
    auto* rename = qobject_cast<BulkRenameController*>(m_app->tabs()->controllerAt(row));
    QVERIFY(rename);

    m_slow->setOperationDelayMs(kDriveDelayMs);

    // The listing is per distinct parent folder, and it used to be made from
    // here every time the selection changed.
    QElapsedTimer clock;
    clock.start();
    rename->setTargets(
        { QStringLiteral("mem://slow/folder/notes.txt"), QStringLiteral("mem://slow/folder/other.txt") });
    const qint64 spent = clock.elapsed();

    QVERIFY2(spent < kGestureBudgetMs,
        qPrintable(QStringLiteral("setting the targets waited %1 ms for a drive that takes %2")
                       .arg(spent)
                       .arg(kDriveDelayMs)));
    QCOMPARE(rename->sourceUris().size(), 2);
}

MOLE_TEST_MAIN(TestDrivesOffTheDrawingThread)
#include "tst_DrivesOffTheDrawingThread.moc"
