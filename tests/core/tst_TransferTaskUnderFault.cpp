#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/PartialWrite.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>

#include <atomic>

using namespace mole;
using namespace mole::test;

namespace {

/// The whole payload, so a scenario can say "at 30%" and mean a byte.
constexpr qint64 kPayload = 4000;
constexpr qint64 kThirty = 1200;
constexpr qint64 kSixty = 2400;

} // namespace

/// A copy that goes wrong, over and over, on purpose.
///
/// Everything here asserts the same three things, because they are the three
/// that matter when a transfer fails: **no partial file is presented as a
/// finished one**, **the source is still there**, and **the failure says which
/// file and why**. A copy that half-worked and said nothing is worse than one
/// that did not start, since the first one gets trusted.
///
/// The destination is the local disk rather than the memory drive, because the
/// guard being tested lives in a backend: a write goes under a working name and
/// is renamed into place only when it is finished (ADR-0020, ADR-0021). Nothing
/// in this file sleeps -- a fault fires at a byte offset, and a transfer is held
/// still by stalling its read rather than by hoping.
class TestTransferTaskUnderFault : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aSourceRenamedMidCopyFails();
    void aSourceDeletedMidCopyFails();
    void aSourceTruncatedMidCopyCopiesWhatIsLeft();
    void aSourceThatClaimsMoreThanItGivesFails();
    void aSourceThatGrewSinceTheListingIsCopiedAsItNowIs();
    void aSourceWhosePermissionIsWithdrawnMidCopyLeavesNothingBehind();
    void theFileBeingWrittenIsRemovedMidWrite();
    void anUploadWhoseConnectionDiesMidFileLeavesNothingUnderItsName();
    void aDestinationThatFillsUpSaysWhy();
    void aLinkThatKeepsGoingShortStillDeliversEveryByte();

    void cancellingBeforeTheFirstByteLeavesNothing();
    void cancellingMidFileLeavesNothing();
    void cancellingBetweenFilesKeepsWhatLanded();

    void tenConcurrentCopiesOverOneDrive();
    void everyOneOfTenConcurrentCopiesGetsTheFault();

    void aDriveUnmountedWithATransferInFlight();
    void aMoveWhoseSourceDiesMidFileKeepsTheSource();

private:
    /// A copy of `names` from the faulty source into the temporary directory.
    TransferTask::Request request(const QStringList& names = { QStringLiteral("payload.bin") },
        TransferTask::Mode mode = TransferTask::Mode::Copy) const;
    /// Submits and waits for the end.
    TransferTask* run(const TransferTask::Request& request);

    /// What is in the destination directory, working names included -- litter
    /// left behind is part of what a failed copy is judged on.
    QStringList destinationEntries() const;
    qint64 destinationSize(const QString& name) const;

    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::shared_ptr<FaultyFileSystem> m_source;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::shared_ptr<FaultyFileSystem> m_target;
    std::unique_ptr<TempTree> m_tree;
};

void TestTransferTaskUnderFault::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_memory->addFile(QStringLiteral("/src/payload.bin"), QByteArray(kPayload, 'a'));
    m_source = std::make_shared<FaultyFileSystem>(m_memory);
    m_disk = std::make_shared<LocalFileSystem>();
    m_target = std::make_shared<FaultyFileSystem>(m_disk);
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
}

void TestTransferTaskUnderFault::cleanup()
{
    m_tasks.reset();
    m_source.reset();
    m_target.reset();
    m_memory.reset();
    m_disk.reset();
    m_tree.reset();
}

TransferTask::Request TestTransferTaskUnderFault::request(
    const QStringList& names, TransferTask::Mode mode) const
{
    TransferTask::Request request;
    request.sourceFileSystem = m_source;
    request.targetFileSystem = m_target;
    for (const QString& name : names)
        request.sources.append(VfsUri::fromString(QStringLiteral("mem:///src/") + name));
    request.targetDirectory = m_tree->rootUri();
    request.mode = mode;
    return request;
}

TransferTask* TestTransferTaskUnderFault::run(const TransferTask::Request& request)
{
    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    if (!waitForTask(task))
        return nullptr;
    return task;
}

QStringList TestTransferTaskUnderFault::destinationEntries() const
{
    return QDir(m_tree->path()).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}

qint64 TestTransferTaskUnderFault::destinationSize(const QString& name) const
{
    return QFileInfo(m_tree->absolute(name)).size();
}

void TestTransferTaskUnderFault::aSourceRenamedMidCopyFails()
{
    m_source->fileIsRenamedAt(kThirty, QStringLiteral("renamed.bin"));

    TransferTask* task = run(request());
    QVERIFY(task);

    QCOMPARE(task->copiedCount(), 0);
    QCOMPARE(task->failedCount(), 1);
    const QString failure = task->failures().first();
    QVERIFY2(failure.contains(QStringLiteral("payload.bin")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("stopped after 1200 bytes")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("renamed")), qPrintable(failure));

    // Not even under a working name: an abandoned write is not a result.
    QVERIFY2(destinationEntries().isEmpty(), qPrintable(destinationEntries().join(QLatin1Char(' '))));
    // The bytes are still on the source drive, under the name they now have.
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///src/renamed.bin"))).ok());
}

void TestTransferTaskUnderFault::aSourceDeletedMidCopyFails()
{
    m_source->fileVanishesAt(kThirty);

    TransferTask* task = run(request());
    QVERIFY(task);

    QCOMPARE(task->copiedCount(), 0);
    QCOMPARE(task->failedCount(), 1);
    const QString failure = task->failures().first();
    QVERIFY2(failure.contains(QStringLiteral("payload.bin")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("went away")), qPrintable(failure));
    QVERIFY(destinationEntries().isEmpty());
}

void TestTransferTaskUnderFault::aSourceTruncatedMidCopyCopiesWhatIsLeft()
{
    // The file really did shrink, so what arrives is the file as it now is --
    // 1200 bytes copied, 1200 bytes on the source, and nothing to report. This
    // is the case the guard in aSourceThatClaimsMoreThanItGivesFails() must let
    // through, and it is why that guard asks the source rather than the plan.
    m_source->fileChangesSizeAt(kThirty, kThirty);

    TransferTask* task = run(request());
    QVERIFY(task);

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
    QCOMPARE(task->copiedCount(), 1);
    QCOMPARE(destinationEntries(), QStringList { QStringLiteral("payload.bin") });
    QCOMPARE(destinationSize(QStringLiteral("payload.bin")), kThirty);
    QCOMPARE(
        m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///src/payload.bin"))).value().size, kThirty);
}

void TestTransferTaskUnderFault::aSourceThatClaimsMoreThanItGivesFails()
{
    // The drive says 4500 bytes, hands over 4000, and still says 4500 when
    // asked again. Nothing shrank: the stream ended early and called it the end
    // of the file. Accepting that is how a move deletes the only full copy.
    m_source->listingOverstatesSizeBy(500);

    TransferTask* task = run(request());
    QVERIFY(task);

    QCOMPARE(task->copiedCount(), 0);
    QCOMPARE(task->failedCount(), 1);
    const QString failure = task->failures().first();
    QVERIFY2(failure.contains(QStringLiteral("payload.bin")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("4500")) && failure.contains(QStringLiteral("4000")),
        qPrintable(failure));
    QVERIFY2(destinationEntries().isEmpty(), qPrintable(destinationEntries().join(QLatin1Char(' '))));
}

void TestTransferTaskUnderFault::aSourceThatGrewSinceTheListingIsCopiedAsItNowIs()
{
    // A log being written to, or a download finishing, between the listing and
    // the copy. The plan says one size and the file gives more -- and the guard
    // that catches a *short* read must not turn this into a failure, because
    // nothing is wrong: everything the file had when it was opened arrived.
    //
    // A listing that understates is how the copy sees it, whichever way round it
    // actually happened.
    m_source->listingOverstatesSizeBy(-1000);

    TransferTask* task = run(request());
    QVERIFY(task);

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
    QCOMPARE(task->copiedCount(), 1);
    // All of it, not the 3000 the plan expected -- and the arrival check, which
    // weighs what was sent rather than what was planned, is content with that.
    QCOMPARE(destinationSize(QStringLiteral("payload.bin")), kPayload);
}

void TestTransferTaskUnderFault::aSourceWhosePermissionIsWithdrawnMidCopyLeavesNothingBehind()
{
    // Somebody changes the mode bits, or a credential is re-locked, while the
    // copy is running. From here it is a read that fails and a drive that
    // answers nothing afterwards -- including the question the short-read guard
    // wants to ask it.
    m_source->accessRevokedAt(kThirty);

    TransferTask* task = run(request());
    QVERIFY(task);

    QCOMPARE(task->copiedCount(), 0);
    QCOMPARE(task->failedCount(), 1);
    QVERIFY2(destinationEntries().isEmpty(), qPrintable(destinationEntries().join(QLatin1Char(' '))));
    // The source is untouched, which the drive underneath can still say even
    // though the wrapper over it is refusing everything.
    QCOMPARE(
        m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///src/payload.bin"))).value().size, kPayload);
}

void TestTransferTaskUnderFault::theFileBeingWrittenIsRemovedMidWrite()
{
    // Something removes the file being written while it is being written. The
    // bytes go on arriving -- the stream is open on an inode that no longer has
    // a name -- and the failure only shows up when the result is put in place.
    const VfsUri target = m_tree->rootUri().child(QStringLiteral("payload.bin"));
    const QString staging = partialWriteOf(target).toLocalPath();
    m_target->whenWriteReaches(kThirty, [staging] { QFile::remove(staging); });

    TransferTask* task = run(request());
    QVERIFY(task);

    QCOMPARE(task->copiedCount(), 0);
    QCOMPARE(task->failedCount(), 1);
    QVERIFY2(task->failures().first().contains(QStringLiteral("payload.bin")),
        qPrintable(task->failures().first()));
    QVERIFY(destinationEntries().isEmpty());
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///src/payload.bin"))).ok());
}

/// The connection dying part way through an **upload**, cheaply.
///
/// The interference tier kills the connection at a byte offset in each
/// direction, and only one of those directions had a mirror here: a read that
/// dies is `aReadThatStopsHalfWayIsNotAnEndOfFile` and a source that dies is
/// several cases above, while a *write* that dies mid-file was covered nowhere
/// outside a move and a dry run. The two directions fail through different code
/// -- one gives up on a stream it is reading, the other on one it is writing --
/// so one of them proves nothing about the other.
void TestTransferTaskUnderFault::anUploadWhoseConnectionDiesMidFileLeavesNothingUnderItsName()
{
    m_target->writeFailsAt(kSixty);

    TransferTask* task = run(request());
    QVERIFY(task);

    QCOMPARE(task->copiedCount(), 0);
    QCOMPARE(task->failedCount(), 1);

    // Which file and why, the same standard the disk-full case is held to. The
    // bytes before the offset really were written, so "it failed" without a
    // name would leave somebody looking for a file that is genuinely part there.
    const QString failure = task->failures().first();
    QVERIFY2(failure.contains(QStringLiteral("payload.bin")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("refused")), qPrintable(failure));

    // Nothing under the final name and no working file either. This is the one
    // that matters: a half-uploaded file left under the name it was aiming at is
    // indistinguishable from a finished one to everything that looks later.
    QVERIFY2(destinationEntries().isEmpty(), qPrintable(destinationEntries().join(QLatin1Char(' '))));
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///src/payload.bin"))).ok());
}

void TestTransferTaskUnderFault::aDestinationThatFillsUpSaysWhy()
{
    m_target->destinationFillsAt(kSixty);

    TransferTask* task = run(request());
    QVERIFY(task);

    QCOMPARE(task->copiedCount(), 0);
    QCOMPARE(task->failedCount(), 1);

    // Which file, and why. "short write" on its own is true and useless: a disk
    // that filled up and a server that hung up read exactly alike.
    const QString failure = task->failures().first();
    QVERIFY2(failure.contains(QStringLiteral("payload.bin")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("no space left")), qPrintable(failure));
    QVERIFY2(failure.contains(QStringLiteral("2400")), qPrintable(failure));

    QVERIFY(destinationEntries().isEmpty());
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///src/payload.bin"))).ok());
}

/// A link that keeps handing back less than it was asked for, cheaply.
///
/// The mirror of the interference tier's lossy-link case, where a quarter of a
/// gigabyte crosses a link carrying 200 ms of latency and 1% and then 5% packet
/// loss, and every byte is verified at the far end. What that exercises, once
/// the packets are out of it, is a read returning fewer bytes than it was asked
/// for over and over -- ordinary streaming behaviour that a caller may not read
/// as the end of the file. That live case needs a server and four minutes; this
/// one needs neither and runs on every change.
void TestTransferTaskUnderFault::aLinkThatKeepsGoingShortStillDeliversEveryByte()
{
    // Patterned, not four thousand copies of one letter like the fixture's own
    // payload. A comparison against a run of identical bytes passes just as
    // happily on a file that was truncated and padded back out, or one whose
    // blocks arrived in the wrong order -- which is exactly what this case is
    // supposed to be able to see.
    QByteArray patterned(kPayload, Qt::Uninitialized);
    for (qint64 i = 0; i < kPayload; ++i)
        patterned[static_cast<int>(i)] = static_cast<char>((i * 31 + i / 251) & 0xff);
    m_memory->addFile(QStringLiteral("/src/lossy.bin"), patterned);

    // Seven of them, and deliberately not on any boundary a buffer is likely to
    // use. The one-byte read is the smallest a stream can go short by without
    // being an end of file, and it is the one a caller checking `got < asked`
    // gets wrong.
    const QList<qint64> offsets = { 1, 499, 1201, 2399, 2400, 3001, kPayload - 1 };
    const QList<qint64> chunks = { 1, 3, 17, 1, 250, 7, 1 };
    for (int i = 0; i < offsets.size(); ++i)
        m_source->readGoesShortAt(offsets.at(i), chunks.at(i));

    // And a counter at the same offsets, because this case asserts that a copy
    // *succeeded* -- which it would do just as cheerfully if not one of the
    // faults above had fired. Three of the interference tier's cases were green
    // and checking nothing in exactly this way, so a case whose instrument might
    // silently miss says out loud that it landed.
    std::atomic<int> reached { 0 };
    for (const qint64 offset : offsets)
        m_source->whenReadReaches(offset, [&reached] { ++reached; });

    TransferTask* task = run(request({ QStringLiteral("lossy.bin") }));
    QVERIFY(task);

    // It succeeds. Nothing here is a fault the copy has to report -- a short
    // read is what a network does, not what a broken one does.
    QCOMPARE(task->failedCount(), 0);
    QCOMPARE(task->copiedCount(), 1);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));

    // And byte for byte, which is the whole point: a file of the right length
    // is not the same claim as a file of the right contents, and a boundary that
    // is off by one produces the first without the second.
    QCOMPARE(reached.load(), static_cast<int>(offsets.size()));

    QCOMPARE(destinationSize(QStringLiteral("lossy.bin")), kPayload);
    QFile landed(QDir(m_tree->path()).filePath(QStringLiteral("lossy.bin")));
    QVERIFY(landed.open(QIODevice::ReadOnly));
    QCOMPARE(landed.readAll(), patterned);
}

void TestTransferTaskUnderFault::cancellingBeforeTheFirstByteLeavesNothing()
{
    m_source->readStallsAt(0);

    auto* task = new TransferTask(request());
    m_tasks->submit(task);
    QVERIFY(waitFor([this] { return m_source->isStalled(); }));

    task->requestCancel();
    m_source->release();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Cancelled);
    QCOMPARE(task->copiedCount(), 0);
    QVERIFY2(destinationEntries().isEmpty(), qPrintable(destinationEntries().join(QLatin1Char(' '))));
    QCOMPARE(
        m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///src/payload.bin"))).value().size, kPayload);
}

void TestTransferTaskUnderFault::cancellingMidFileLeavesNothing()
{
    m_source->readStallsAt(kSixty);

    auto* task = new TransferTask(request());
    m_tasks->submit(task);
    QVERIFY(waitFor([this] { return m_source->isStalled(); }));

    task->requestCancel();
    m_source->release();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Cancelled);
    QCOMPARE(task->copiedCount(), 0);
    // Half a file under the name somebody asked for would be indistinguishable
    // from a file that is simply that size.
    QVERIFY2(destinationEntries().isEmpty(), qPrintable(destinationEntries().join(QLatin1Char(' '))));
}

void TestTransferTaskUnderFault::cancellingBetweenFilesKeepsWhatLanded()
{
    m_memory->addFile(QStringLiteral("/src/second.bin"), QByteArray(kPayload, 'b'));
    m_source->readStallsAt(0, QStringLiteral("/src/second.bin"));

    auto* task = new TransferTask(request({ QStringLiteral("payload.bin"), QStringLiteral("second.bin") }));
    m_tasks->submit(task);
    QVERIFY(waitFor([this] { return m_source->isStalled(); }));

    task->requestCancel();
    m_source->release();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Cancelled);
    // The one that finished before the cancel stays, whole. The one that had
    // not started leaves nothing.
    QCOMPARE(destinationEntries(), QStringList { QStringLiteral("payload.bin") });
    QCOMPARE(destinationSize(QStringLiteral("payload.bin")), kPayload);
}

void TestTransferTaskUnderFault::tenConcurrentCopiesOverOneDrive()
{
    QList<TransferTask*> tasks;
    for (int i = 0; i < 10; ++i) {
        const QString name = QStringLiteral("bulk%1.bin").arg(i);
        m_memory->addFile(QStringLiteral("/src/") + name, QByteArray(kPayload, char('a' + i)));

        auto* task = new TransferTask(request({ name }));
        m_tasks->submit(task);
        tasks.append(task);
    }

    for (TransferTask* task : std::as_const(tasks)) {
        QVERIFY(waitForTask(task));
        QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
    }

    // Ten transfers through one drive, one temporary directory and one working
    // name each: nothing may collide and nothing may be left over.
    QCOMPARE(destinationEntries().size(), 10);
    for (int i = 0; i < 10; ++i)
        QCOMPARE(destinationSize(QStringLiteral("bulk%1.bin").arg(i)), kPayload);
}

void TestTransferTaskUnderFault::everyOneOfTenConcurrentCopiesGetsTheFault()
{
    // A fault consumed by whichever stream reached it first would turn nine of
    // these into successes, and the suite would report that a dropped
    // connection is survivable.
    m_source->readFailsAt(kThirty);

    QList<TransferTask*> tasks;
    for (int i = 0; i < 10; ++i) {
        const QString name = QStringLiteral("bulk%1.bin").arg(i);
        m_memory->addFile(QStringLiteral("/src/") + name, QByteArray(kPayload, char('a' + i)));

        auto* task = new TransferTask(request({ name }));
        m_tasks->submit(task);
        tasks.append(task);
    }

    for (TransferTask* task : std::as_const(tasks)) {
        QVERIFY(waitForTask(task));
        QCOMPARE(task->copiedCount(), 0);
        QCOMPARE(task->failedCount(), 1);
    }
    QVERIFY2(destinationEntries().isEmpty(), qPrintable(destinationEntries().join(QLatin1Char(' '))));
}

void TestTransferTaskUnderFault::aDriveUnmountedWithATransferInFlight()
{
    VfsManager drives;
    Mount mount;
    mount.displayName = QStringLiteral("Scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = m_source;
    const QString id = drives.addMount(mount);
    QVERIFY(!id.isEmpty());

    m_source->readStallsAt(kSixty);
    auto* task = new TransferTask(request());
    m_tasks->submit(task);
    QVERIFY(waitFor([this] { return m_source->isStalled(); }));

    // The drive goes away while the bytes are in flight. A task holds its
    // backend for the length of its run, so the transfer finishes against the
    // drive it was given rather than losing it half way -- what disappears is
    // the mount, which is a thing the interface resolves through.
    drives.removeMount(id);
    m_source->release();
    QVERIFY(waitForTask(task));

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
    QCOMPARE(destinationSize(QStringLiteral("payload.bin")), kPayload);
    QVERIFY(!drives.resolve(VfsUri::fromString(QStringLiteral("mem:///src/payload.bin"))));
}

void TestTransferTaskUnderFault::aMoveWhoseSourceDiesMidFileKeepsTheSource()
{
    m_source->fileIsRenamedAt(kThirty, QStringLiteral("renamed.bin"));

    TransferTask* task = run(request({ QStringLiteral("payload.bin") }, TransferTask::Mode::Move));
    QVERIFY(task);

    QCOMPARE(task->failedCount(), 1);
    QVERIFY(destinationEntries().isEmpty());
    // A move deletes nothing while anything failed, so the bytes still exist --
    // under the name the fault gave them, which is where they went.
    const Result<FileEntry> left
        = m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///src/renamed.bin")));
    QVERIFY2(left.ok(), "a move whose copy failed deleted the source");
    QCOMPARE(left.value().size, kPayload);
}

MOLE_TEST_MAIN(TestTransferTaskUnderFault)
#include "tst_TransferTaskUnderFault.moc"
