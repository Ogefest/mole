#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/DragSource.h"

#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

using namespace mole;
using namespace mole::test;

/// The drag source is the one place that hands a selection to the desktop, so
/// every test here replaces that final step with a recorder. Nothing is actually
/// dragged: `QDrag::exec()` wants a platform and a pointer, and this binary has
/// neither -- see ADR-0040 for why the seam is where it is.
class TestDragSource : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void localFilesBecomeOneUriList();
    void aDirectoryGoesOutAsItsOwnUrl();
    void theHookIsOfferedCopyAndNothingElse();
    void rowsThatAreNotOnDiskStartNothingAndSayWhy();
    void aMixedSelectionSendsWhatItCanAndSaysHowManyStayed();
    void anEmptySelectionStartsNothing();
    void aRefusedDragSaysSo();

    void aRowOnADriveStartsNoDragAndSaysItIsFetchingIt();
    void theSecondDragCarriesTheFetchedCopyByteForByte();
    void aFolderOnADriveStagesWithEverythingUnderneathIt();
    void theSecondDragOfAnUnchangedFileFetchesNothingAgain();
    void aFetchThatFailsSaysSoAndStartsNoDrag();

private:
    DragSource* makeSource();
    int queuedSoFar() const { return static_cast<int>(m_tasks->tasks().size()); }

    std::unique_ptr<QTemporaryDir> m_dir;
    VfsManager* m_vfs = nullptr;
    TaskManager* m_tasks = nullptr;
    EventBus* m_events = nullptr;
    std::unique_ptr<IndexDatabase> m_index;
    std::shared_ptr<MemoryFileSystem> m_drive;
    PluginServices m_services;

    QList<QUrl> m_urls;
    QStringList m_formats;
    Qt::DropActions m_actions = Qt::IgnoreAction;
    int m_handovers = 0;
    bool m_hookResult = true;
};

void TestDragSource::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());

    m_vfs = new VfsManager(this);
    m_tasks = new TaskManager(this);
    m_events = new EventBus(this);
    m_index = std::make_unique<IndexDatabase>(QDir(m_dir->path()).filePath(QStringLiteral("i.sqlite")));
    QVERIFY(m_index->open().ok());

    // A drive that is not the local disk, which is the whole condition under test:
    // rows on it have no path any other application could open. An in-memory drive
    // rather than a mounted archive, for the same reason the launcher's tests use
    // one -- no plugin, no server, and it is the same question.
    m_drive = std::make_shared<MemoryFileSystem>();
    m_drive->addFile(QStringLiteral("/docs/manual.txt"), QByteArray("inside the drive"));
    m_drive->addFile(QStringLiteral("/docs/deep/notes.md"), QByteArray("further in"));

    Mount mount;
    mount.displayName = QStringLiteral("scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = m_drive;
    m_vfs->addMount(mount);

    m_services = PluginServices { m_vfs, m_tasks, m_index.get(), m_events };

    m_urls.clear();
    m_formats.clear();
    m_actions = Qt::IgnoreAction;
    m_handovers = 0;
    m_hookResult = true;
}

void TestDragSource::cleanup()
{
    delete m_tasks;
    m_tasks = nullptr;
    delete m_vfs;
    m_vfs = nullptr;
    delete m_events;
    m_events = nullptr;
    m_index.reset();
    m_drive.reset();
    m_dir.reset();
}

DragSource* TestDragSource::makeSource()
{
    auto* source = new DragSource(m_services, this);
    source->setStartHook([this](std::unique_ptr<QMimeData> mime, Qt::DropActions actions) {
        ++m_handovers;
        m_urls = mime->urls();
        m_formats = mime->formats();
        m_actions = actions;
        return m_hookResult;
    });
    return source;
}

void TestDragSource::localFilesBecomeOneUriList()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("first.txt")));
    QVERIFY(tree.writeFile(QStringLiteral("second.txt")));
    QVERIFY(tree.writeFile(QStringLiteral("third.txt")));

    DragSource* source = makeSource();
    QSignalSpy started(source, &DragSource::started);

    // The order is the order the rows were given: a receiver that lists what it
    // was handed shows the user's own selection back to them.
    source->start({
        VfsUri::fromLocalPath(tree.absolute(QStringLiteral("first.txt"))),
        VfsUri::fromLocalPath(tree.absolute(QStringLiteral("second.txt"))),
        VfsUri::fromLocalPath(tree.absolute(QStringLiteral("third.txt"))),
    });

    QCOMPARE(m_handovers, 1);
    QVERIFY(m_formats.contains(QStringLiteral("text/uri-list")));
    QCOMPARE(m_urls.size(), 3);
    QVERIFY(m_urls.at(0).isLocalFile());
    QCOMPARE(m_urls.at(0).toLocalFile(), tree.absolute(QStringLiteral("first.txt")));
    QCOMPARE(m_urls.at(1).toLocalFile(), tree.absolute(QStringLiteral("second.txt")));
    QCOMPARE(m_urls.at(2).toLocalFile(), tree.absolute(QStringLiteral("third.txt")));
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.first().first().toInt(), 3);
}

void TestDragSource::aDirectoryGoesOutAsItsOwnUrl()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.makeDirs(QStringLiteral("photos/2026")));
    QVERIFY(tree.writeFile(QStringLiteral("photos/2026/beach.jpg")));

    DragSource* source = makeSource();
    source->start({ VfsUri::fromLocalPath(tree.absolute(QStringLiteral("photos"))) });

    // One url for the folder, not one per file underneath it. Expanding it here
    // would hand the receiver a flat list of leaves.
    QCOMPARE(m_urls.size(), 1);
    QCOMPARE(m_urls.first().toLocalFile(), tree.absolute(QStringLiteral("photos")));
}

void TestDragSource::theHookIsOfferedCopyAndNothingElse()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("payslip.pdf")));

    DragSource* source = makeSource();
    source->start({ VfsUri::fromLocalPath(tree.absolute(QStringLiteral("payslip.pdf"))) });

    // Not "copy among others" and not "copy by default": copy alone. A receiver
    // that was offered a move would delete the source after taking the bytes.
    QCOMPARE(m_actions, Qt::DropActions(Qt::CopyAction));
    QVERIFY(!m_actions.testFlag(Qt::MoveAction));
    QVERIFY(!m_actions.testFlag(Qt::LinkAction));
}

void TestDragSource::rowsThatAreNotOnDiskStartNothingAndSayWhy()
{
    DragSource* source = makeSource();
    QSignalSpy refused(source, &DragSource::refused);
    QSignalSpy started(source, &DragSource::started);

    // What a selection inside a mounted archive looks like from here: valid
    // rows, on a drive, with no path any other application could open.
    source->start({
        VfsUri::fromString(QStringLiteral("zip:///backup.zip/notes/monday.txt")),
        VfsUri::fromString(QStringLiteral("zip:///backup.zip/notes/tuesday.txt")),
    });

    QCOMPARE(m_handovers, 0);
    QCOMPARE(started.count(), 0);
    QCOMPARE(refused.count(), 1);
    QVERIFY(!refused.first().first().toString().isEmpty());
}

void TestDragSource::aMixedSelectionSendsWhatItCanAndSaysHowManyStayed()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("here.txt")));

    DragSource* source = makeSource();
    QSignalSpy started(source, &DragSource::started);
    QSignalSpy leftBehind(source, &DragSource::leftBehind);

    source->start({
        VfsUri::fromLocalPath(tree.absolute(QStringLiteral("here.txt"))),
        VfsUri::fromString(QStringLiteral("sftp://nas/volume1/away.txt")),
        VfsUri::fromString(QStringLiteral("s3://bucket/reports/away.csv")),
    });

    // The local row goes, and the two that could not are counted out loud. Half
    // a selection leaving in silence is the one outcome this must not have.
    QCOMPARE(m_urls.size(), 1);
    QCOMPARE(m_urls.first().toLocalFile(), tree.absolute(QStringLiteral("here.txt")));
    QCOMPARE(started.count(), 1);
    QCOMPARE(leftBehind.count(), 1);
    QCOMPARE(leftBehind.first().at(0).toInt(), 1);
    QCOMPARE(leftBehind.first().at(1).toInt(), 2);
}

void TestDragSource::anEmptySelectionStartsNothing()
{
    DragSource* source = makeSource();
    QSignalSpy refused(source, &DragSource::refused);

    source->start({});

    QCOMPARE(m_handovers, 0);
    QCOMPARE(refused.count(), 1);
}

void TestDragSource::aRefusedDragSaysSo()
{
    // The platform declining the drag is a normal outcome and has to become a
    // message rather than silence.
    m_hookResult = false;

    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("thing.txt")));

    DragSource* source = makeSource();
    QSignalSpy refused(source, &DragSource::refused);
    QSignalSpy started(source, &DragSource::started);

    source->start({ VfsUri::fromLocalPath(tree.absolute(QStringLiteral("thing.txt"))) });

    QCOMPARE(m_handovers, 1);
    QCOMPARE(started.count(), 0);
    QCOMPARE(refused.count(), 1);
    QVERIFY(!refused.first().first().toString().isEmpty());
}

// ---- rows that are not on disk ----------------------------------------------
//
// A drag cannot wait for a hundred megabytes: the gesture is over before they
// arrive and a QDrag cannot be started once the button is up. So the first drag
// fetches and says so, and the second one carries it.

void TestDragSource::aRowOnADriveStartsNoDragAndSaysItIsFetchingIt()
{
    DragSource* source = makeSource();
    QSignalSpy staging(source, &DragSource::staging);
    QSignalSpy started(source, &DragSource::started);
    const int queued = queuedSoFar();

    source->start({ VfsUri::fromString(QStringLiteral("mem:///docs/manual.txt")) });

    // Nothing dragged, something started, and the user told which of the two
    // happened. A gesture that appears to have done nothing is the failure here.
    QCOMPARE(m_handovers, 0);
    QCOMPARE(started.count(), 0);
    QCOMPARE(staging.count(), 1);
    QCOMPARE(staging.first().first().toInt(), 1);
    QCOMPARE(queuedSoFar(), queued + 1);
}

void TestDragSource::theSecondDragCarriesTheFetchedCopyByteForByte()
{
    DragSource* source = makeSource();
    const VfsUri row = VfsUri::fromString(QStringLiteral("mem:///docs/manual.txt"));

    source->start({ row });
    QVERIFY(waitFor([this] { return m_tasks->activeCount() == 0; }));

    QSignalSpy staging(source, &DragSource::staging);
    source->start({ row });

    QCOMPARE(staging.count(), 0);
    QCOMPARE(m_handovers, 1);
    QCOMPARE(m_urls.size(), 1);
    QVERIFY(m_urls.first().isLocalFile());

    const QString staged = m_urls.first().toLocalFile();
    // The name survives, or the receiver picks its handler by the wrong extension.
    QVERIFY2(staged.endsWith(QStringLiteral("manual.txt")), qPrintable(staged));

    QFile copy(staged);
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), QByteArray("inside the drive"));
}

void TestDragSource::aFolderOnADriveStagesWithEverythingUnderneathIt()
{
    DragSource* source = makeSource();
    const VfsUri folder = VfsUri::fromString(QStringLiteral("mem:///docs"));

    source->start({ folder });
    QVERIFY(waitFor([this] { return m_tasks->activeCount() == 0; }));

    source->start({ folder });
    QCOMPARE(m_handovers, 1);
    QCOMPARE(m_urls.size(), 1);

    // One url for the folder, and the tree really underneath it: a folder that
    // arrives empty is worse than one that could not be dragged.
    const QString staged = m_urls.first().toLocalFile();
    QVERIFY2(staged.endsWith(QStringLiteral("docs")), qPrintable(staged));
    QVERIFY(QFileInfo(staged).isDir());
    QCOMPARE(QFileInfo(QDir(staged).filePath(QStringLiteral("manual.txt"))).size(), 16);
    QVERIFY(QFileInfo::exists(QDir(staged).filePath(QStringLiteral("deep/notes.md"))));
}

void TestDragSource::theSecondDragOfAnUnchangedFileFetchesNothingAgain()
{
    DragSource* source = makeSource();
    const VfsUri row = VfsUri::fromString(QStringLiteral("mem:///docs/manual.txt"));

    source->start({ row });
    QVERIFY(waitFor([this] { return m_tasks->activeCount() == 0; }));
    const int queued = queuedSoFar();

    source->start({ row });
    QCOMPARE(m_handovers, 1);
    source->start({ row });
    QCOMPARE(m_handovers, 2);

    // Dragging the same file twice fetches it once. Nothing else in here would
    // notice a second fetch -- the drag would still work -- which is exactly why
    // it is asserted.
    QCOMPARE(queuedSoFar(), queued);

    // Until the source moves on, and then the copy is not the file any more.
    m_drive->addFile(QStringLiteral("/docs/manual.txt"), QByteArray("rewritten, and longer than before"));
    QSignalSpy staging(source, &DragSource::staging);
    source->start({ row });
    QCOMPARE(staging.count(), 1);
    QCOMPARE(m_handovers, 2);
    QCOMPARE(queuedSoFar(), queued + 1);

    QVERIFY(waitFor([this] { return m_tasks->activeCount() == 0; }));
    source->start({ row });
    QCOMPARE(m_handovers, 3);
    QFile copy(m_urls.first().toLocalFile());
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), QByteArray("rewritten, and longer than before"));
}

void TestDragSource::aFetchThatFailsSaysSoAndStartsNoDrag()
{
    // The drive goes away half way through, which is what a NAS does.
    m_drive->setFault(QStringLiteral("/docs/manual.txt"), VfsError::NetworkError);

    DragSource* source = makeSource();
    QSignalSpy refused(source, &DragSource::refused);
    source->start({ VfsUri::fromString(QStringLiteral("mem:///docs/manual.txt")) });

    QVERIFY(waitFor([&refused] { return refused.count() > 0; }));
    QVERIFY(!refused.first().first().toString().isEmpty());
    QCOMPARE(m_handovers, 0);

    // And nothing was remembered as staged, so trying again really tries again
    // rather than handing over an empty file.
    QSignalSpy staging(source, &DragSource::staging);
    source->start({ VfsUri::fromString(QStringLiteral("mem:///docs/manual.txt")) });
    QCOMPARE(staging.count(), 1);
    QCOMPARE(m_handovers, 0);
}

MOLE_TEST_MAIN(TestDragSource)
#include "tst_DragSource.moc"
