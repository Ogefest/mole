#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/diagnostics/LoggingFileSystem.h"
#include "core/tasks/ProbeDriveTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <thread>

using namespace mole;
using namespace mole::test;

namespace {
QString versionsId()
{
    return QStringLiteral("org.mole.test.versions");
}
} // namespace

class TestDriveProbe : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_fs;

    VfsUri root() const { return VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")); }
    void probeAndWait(const FileSystemPtr& fs);

private slots:
    void init();
    void cleanup();

    void aDriveSaysNothingUntilItHasBeenAsked();
    void mountingADriveDoesNotAskIt();
    void theAnswerArrivesAndIsRemembered();
    void aDriveIsAskedOnceHoweverOftenItIsProbed();
    void twoThreadsArrivingAtOnceAskTheDriveOnce();
    void aProbeThatFailsLeavesTheDriveWorking();
    void nothingOfferedIsNotTheSameAsNoAnswer();
    void aProbeInFlightDoesNotHoldUpAListing();
    void aCancelledProbeLeavesTheDriveUnasked();
    void theWrapperEveryMountGoesThroughForwardsBoth();
};

void TestDriveProbe::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<MemoryFileSystem>();
    m_fs->addFile(QStringLiteral("/alpha.txt"), QByteArray("one"));
}

void TestDriveProbe::cleanup()
{
    // See MOLE-273: the destructor cancels and joins the pool, and a task still
    // running when the harness moves on to the next function races it.
    m_tasks.reset();
    m_fs.reset();
}

void TestDriveProbe::probeAndWait(const FileSystemPtr& fs)
{
    auto* task = new ProbeDriveTask(fs, root());
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));
}

/// The state every drive is in until somebody opens a folder on it. Not "offers
/// nothing" -- nothing has been asked, and the two must not read the same.
void TestDriveProbe::aDriveSaysNothingUntilItHasBeenAsked()
{
    m_fs->setOffers({ versionsId() });

    QCOMPARE(m_fs->offers().state, DriveOffers::State::Unasked);
    QVERIFY(!m_fs->offers().isKnown());
    QVERIFY(!m_fs->offers().has(versionsId()));
    QCOMPARE(m_fs->probeCallCount(), 0);
}

/// Mounting must not get slower, or fail, for a capability nobody has asked for
/// yet -- so several drives configured and none opened is no probes at all.
void TestDriveProbe::mountingADriveDoesNotAskIt()
{
    VfsManager vfs;
    QList<std::shared_ptr<MemoryFileSystem>> drives;

    for (int i = 0; i < 4; ++i) {
        auto drive = std::make_shared<MemoryFileSystem>();
        drive->setOffers({ versionsId() });
        drives.append(drive);

        Mount mount;
        mount.id = QStringLiteral("mem-%1").arg(i);
        mount.displayName = QStringLiteral("scratch %1").arg(i);
        mount.root = root();
        mount.fileSystem = drive;
        QVERIFY(!vfs.addMount(mount).isEmpty());
    }

    // Resolving one is not asking it either: the interface does that constantly,
    // from the thread that draws, and it must stay free.
    QVERIFY(vfs.resolve(root()) != nullptr);

    for (const auto& drive : drives) {
        QCOMPARE(drive->probeCallCount(), 0);
        QCOMPARE(drive->offers().state, DriveOffers::State::Unasked);
    }
}

void TestDriveProbe::theAnswerArrivesAndIsRemembered()
{
    m_fs->setOffers({ versionsId() });
    probeAndWait(m_fs);

    QCOMPARE(m_fs->probeCallCount(), 1);
    const DriveOffers offers = m_fs->offers();
    QCOMPARE(offers.state, DriveOffers::State::Answered);
    QVERIFY(offers.isKnown());
    QVERIFY(offers.has(versionsId()));
    QVERIFY(!offers.has(QStringLiteral("org.mole.test.something-else")));
}

/// Every navigation submits one, and all but the first must cost nothing.
void TestDriveProbe::aDriveIsAskedOnceHoweverOftenItIsProbed()
{
    m_fs->setOffers({ versionsId() });
    for (int i = 0; i < 5; ++i)
        probeAndWait(m_fs);

    QCOMPARE(m_fs->probeCallCount(), 1);
    QVERIFY(m_fs->offers().has(versionsId()));
}

/// Two panes opening folders on one drive at the same moment. The second must
/// not ask again, and must not wait for the first either -- it has a listing of
/// its own to be getting on with.
void TestDriveProbe::twoThreadsArrivingAtOnceAskTheDriveOnce()
{
    m_fs->setOffers({ versionsId() });
    m_fs->setProbeDelayMs(2000);

    const CancelToken cancel;
    std::thread first([this, &cancel] { m_fs->probe(root(), cancel); });

    // Waited for rather than slept past: the second caller has to arrive while
    // the first is inside the drive, which is the whole point of the case.
    QVERIFY(waitFor([this] { return m_fs->isProbing(); }));

    std::thread second([this, &cancel] { m_fs->probe(root(), cancel); });
    second.join();

    // The second returned without waiting for the first, which is still inside
    // its two seconds.
    QVERIFY(m_fs->isProbing());
    QCOMPARE(m_fs->probeCallCount(), 1);

    cancel.cancel();
    first.join();
}

/// A drive that cannot say goes on working. The failure belongs to the probe and
/// reaches nobody: whoever opened the folder asked for a listing.
void TestDriveProbe::aProbeThatFailsLeavesTheDriveWorking()
{
    m_fs->setOffers({ versionsId() });
    m_fs->setProbeFault(VfsError::NetworkError);

    auto* task = new ProbeDriveTask(m_fs, root());
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));
    QVERIFY2(task->state() == Task::State::Succeeded,
        "a drive that cannot say what it offers is not a failed job");

    QCOMPARE(m_fs->offers().state, DriveOffers::State::Failed);
    QVERIFY(!m_fs->offers().isKnown());
    QVERIFY(!m_fs->offers().has(versionsId()));

    QVERIFY2(m_fs->list(root(), CancelToken()).ok(), "the drive still lists");
    QVERIFY2(m_fs->stat(root().child(QStringLiteral("alpha.txt"))).ok(), "and still answers about a file");
}

/// Three states, and each of the three has to be tellable from the other two.
/// A drive that answered "nothing" has said something; one whose probe failed
/// has not; one nobody asked has not been asked.
void TestDriveProbe::nothingOfferedIsNotTheSameAsNoAnswer()
{
    auto silent = std::make_shared<MemoryFileSystem>();
    probeAndWait(silent);
    QCOMPARE(silent->offers().state, DriveOffers::State::Answered);
    QVERIFY(silent->offers().isKnown());
    QVERIFY(silent->offers().ids.isEmpty());

    auto broken = std::make_shared<MemoryFileSystem>();
    broken->setProbeFault(VfsError::AccessDenied);
    probeAndWait(broken);
    QCOMPARE(broken->offers().state, DriveOffers::State::Failed);
    QVERIFY(!broken->offers().isKnown());

    auto untouched = std::make_shared<MemoryFileSystem>();
    QCOMPARE(untouched->offers().state, DriveOffers::State::Unasked);
    QVERIFY(!untouched->offers().isKnown());
}

/// The reason a probe is a task of its own rather than a step inside the
/// listing. A drive that never answers what it offers is still a drive somebody
/// is browsing, and the folder has to open.
void TestDriveProbe::aProbeInFlightDoesNotHoldUpAListing()
{
    m_fs->setProbeDelayMs(60000);

    const CancelToken probeCancel;
    std::thread probing([this, &probeCancel] { m_fs->probe(root(), probeCancel); });
    QVERIFY(waitFor([this] { return m_fs->isProbing(); }));

    // Waited on the condition, not on a clock: the listing has to finish while
    // the probe is still inside the drive, and that is what is asserted.
    const Result<FileEntryList> listing = m_fs->list(root(), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 1);
    QVERIFY2(m_fs->isProbing(), "the listing must have come back while the probe was still out");

    probeCancel.cancel();
    probing.join();
}

/// A probe that was called off found nothing out, so the drive has not said it
/// cannot do the thing -- and the next folder opened here asks again.
void TestDriveProbe::aCancelledProbeLeavesTheDriveUnasked()
{
    m_fs->setOffers({ versionsId() });
    m_fs->setProbeDelayMs(60000);

    CancelToken cancel;
    std::thread probing([this, &cancel] { m_fs->probe(root(), cancel); });
    QVERIFY(waitFor([this] { return m_fs->isProbing(); }));
    cancel.cancel();
    probing.join();

    QCOMPARE(m_fs->offers().state, DriveOffers::State::Unasked);
    QCOMPARE(m_fs->probeCallCount(), 1);

    m_fs->setProbeDelayMs(0);
    probeAndWait(m_fs);
    QCOMPARE(m_fs->probeCallCount(), 2);
    QVERIFY(m_fs->offers().has(versionsId()));
}

/// VfsManager puts every drive behind the log wrapper, so a wrapper that kept an
/// answer of its own -- or none -- would make this discoverable in a test and
/// absent in the running application.
void TestDriveProbe::theWrapperEveryMountGoesThroughForwardsBoth()
{
    m_fs->setOffers({ versionsId() });
    const FileSystemPtr wrapped = withLogging(m_fs, QStringLiteral("scratch"));
    QVERIFY(wrapped != m_fs);

    QCOMPARE(wrapped->offers().state, DriveOffers::State::Unasked);
    probeAndWait(wrapped);

    QCOMPARE(m_fs->probeCallCount(), 1);
    QVERIFY2(wrapped->offers().has(versionsId()), "the wrapper must report what the drive found");
    QVERIFY2(m_fs->offers().has(versionsId()), "and the drive must be what holds it");
}

MOLE_TEST_MAIN(TestDriveProbe)
#include "tst_DriveProbe.moc"
