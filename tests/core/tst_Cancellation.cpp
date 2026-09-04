#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QMutex>
#include <QTest>
#include <QWaitCondition>

using namespace mole;
using namespace mole::test;

namespace {

/// How long a cancelled task is given to notice.
///
/// The contract is a second. This is the margin over it, and the margin exists
/// for a loaded build machine rather than for the code: nothing here waits for
/// the timeout in the ordinary case, because the task stops as soon as it looks.
constexpr int kStopsWithinMs = 2000;

/// A drive that can be stopped at one operation and held there.
///
/// The fault injector holds a *transfer* still at a byte offset, which covers
/// everything that happens while bytes are moving. This holds one of the calls
/// either side of that -- the walk that builds the plan, the listing that checks
/// what arrived, the delete a move ends with -- so a cancel can be aimed at a
/// stage rather than at a moment.
class GatedFileSystem final : public IFileSystem
{
public:
    enum class At { Nothing, List, Remove, Read };

    explicit GatedFileSystem(FileSystemPtr inner)
        : m_inner(std::move(inner))
    {
    }

    void stopAt(At what)
    {
        QMutexLocker lock(&m_mutex);
        m_stopAt = what;
    }

    /// Whether a call is being held right now. Wait for this, never for a delay.
    bool isWaiting() const
    {
        QMutexLocker lock(&m_mutex);
        return m_waiting > 0;
    }

    void release()
    {
        QMutexLocker lock(&m_mutex);
        m_released = true;
        m_wake.wakeAll();
    }

    int reads() const
    {
        QMutexLocker lock(&m_mutex);
        return m_reads;
    }
    int removes() const
    {
        QMutexLocker lock(&m_mutex);
        return m_removes;
    }

    QString scheme() const override { return m_inner->scheme(); }
    VfsCapabilities capabilities() const override { return m_inner->capabilities(); }

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override
    {
        hold(At::List);
        return m_inner->list(dir, cancel);
    }
    Result<FileEntry> stat(const VfsUri& target) override { return m_inner->stat(target); }
    Result<void> makeDirectory(const VfsUri& target) override { return m_inner->makeDirectory(target); }
    Result<void> remove(const VfsUri& target, bool recursive, const CancelToken&) override
    {
        {
            QMutexLocker lock(&m_mutex);
            ++m_removes;
        }
        hold(At::Remove);
        return m_inner->remove(target, recursive);
    }
    Result<void> rename(const VfsUri& from, const VfsUri& to, const CancelToken&) override
    {
        return m_inner->rename(from, to);
    }
    Result<std::unique_ptr<QIODevice>> openRead(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& = {}) override
    {
        {
            QMutexLocker lock(&m_mutex);
            ++m_reads;
        }
        hold(At::Read);
        return m_inner->openRead(target, expectedSize);
    }
    Result<std::unique_ptr<QIODevice>> openWrite(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& = {}) override
    {
        return m_inner->openWrite(target, expectedSize);
    }

private:
    void hold(At what)
    {
        QMutexLocker lock(&m_mutex);
        if (m_stopAt != what || m_released)
            return;
        ++m_waiting;
        while (!m_released)
            m_wake.wait(&m_mutex);
        --m_waiting;
    }

    FileSystemPtr m_inner;
    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    At m_stopAt = At::Nothing;
    int m_waiting = 0;
    int m_reads = 0;
    int m_removes = 0;
    bool m_released = false;
};

} // namespace

/// Cancellation, at every stage a job has.
///
/// Three things are asked of every one of them: the task stops rather than
/// running to the end, nothing half-finished is left under a name that suggests
/// it finished, and the source is exactly as it was. A cancel that is noticed
/// only between files is a cancel nobody can use on the job that needs it, and a
/// cancel that leaves a partial file behind is worse than no cancel at all.
class TestCancellation : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aTaskCancelledWhileItIsStillQueuedNeverStarts();
    void cancellingDuringTheWalkStopsBeforeAnythingIsCopied();
    void cancellingDuringTheArrivalCheckStopsAndKeepsTheSource();
    void cancellingDuringAMovesDeletePassKeepsWhatIsLeft();
    void cancellingTwiceInQuickSuccessionIsStillOneOutcome();

private:
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::unique_ptr<TempTree> m_tree;
    std::shared_ptr<LocalFileSystem> m_disk;
};

void TestCancellation::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_disk = std::make_shared<LocalFileSystem>();
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
}

void TestCancellation::cleanup()
{
    m_tasks.reset();
    m_memory.reset();
    m_disk.reset();
    m_tree.reset();
}

void TestCancellation::aTaskCancelledWhileItIsStillQueuedNeverStarts()
{
    // The queue is where a mistaken job is most often caught: somebody drops a
    // folder on the wrong pane and reaches for the cross before anything has
    // begun. Nothing should be opened at all.
    m_tasks->setMaxThreadCount(1);
    m_memory->addFile(QStringLiteral("/source/first.bin"), QByteArray(64 * 1024, 'a'));
    m_memory->addFile(QStringLiteral("/source/second.bin"), QByteArray(64 * 1024, 'b'));
    m_memory->addDirectory(QStringLiteral("/arrived"));

    auto blocking = std::make_shared<FaultyFileSystem>(m_memory);
    blocking->readStallsAt(1024);

    TransferTask::Request occupied;
    occupied.sourceFileSystem = blocking;
    occupied.targetFileSystem = m_memory;
    occupied.sources = { VfsUri::fromString(QStringLiteral("mem:///source/first.bin")) };
    occupied.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));
    auto* holder = new TransferTask(occupied);
    m_tasks->submit(holder);
    QVERIFY(waitFor([&] { return blocking->isStalled(); }));

    // The only worker is busy, so this one is still in the queue.
    auto queuedSource = std::make_shared<GatedFileSystem>(m_memory);
    TransferTask::Request queued;
    queued.sourceFileSystem = queuedSource;
    queued.targetFileSystem = m_memory;
    queued.sources = { VfsUri::fromString(QStringLiteral("mem:///source/second.bin")) };
    queued.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));
    auto* waiting = new TransferTask(queued);
    m_tasks->submit(waiting);

    QCOMPARE(waiting->state(), Task::State::Pending);
    waiting->requestCancel();

    blocking->release();
    QVERIFY(waitForTask(holder, kStopsWithinMs));
    QVERIFY(waitForTask(waiting, kStopsWithinMs));

    QCOMPARE(waiting->state(), Task::State::Cancelled);
    QCOMPARE(waiting->copiedCount(), 0);
    QCOMPARE(queuedSource->reads(), 0);
    QVERIFY(!m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///arrived/second.bin"))).ok());
}

void TestCancellation::cancellingDuringTheWalkStopsBeforeAnythingIsCopied()
{
    // A recursive copy spends its first seconds walking, and on a slow drive
    // that is where a mistake is noticed. Nothing has been written yet, so the
    // only thing that can go wrong is the job carrying on regardless.
    for (int i = 0; i < 20; ++i)
        m_memory->addFile(QStringLiteral("/source/deep/file%1.bin").arg(i), QByteArray(1024, 'x'));
    m_memory->addDirectory(QStringLiteral("/arrived"));

    auto source = std::make_shared<GatedFileSystem>(m_memory);
    source->stopAt(GatedFileSystem::At::List);

    TransferTask::Request request;
    request.sourceFileSystem = source;
    request.targetFileSystem = m_memory;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///source")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitFor([&] { return source->isWaiting(); }));

    task->requestCancel();
    source->release();
    QVERIFY2(waitForTask(task, kStopsWithinMs), "a cancel during the walk has to be noticed within a second");

    QCOMPARE(task->state(), Task::State::Cancelled);
    QCOMPARE(task->copiedCount(), 0);
    QVERIFY(!m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///arrived/source"))).ok());
}

void TestCancellation::cancellingDuringTheArrivalCheckStopsAndKeepsTheSource()
{
    // The check runs after the last byte and before a move deletes anything,
    // which makes it the most dangerous moment to interrupt: the copy looks
    // finished from every angle, and the source is still the only whole copy.
    m_memory->addFile(QStringLiteral("/source/report.bin"), QByteArray(16 * 1024, 'r'));
    m_memory->addDirectory(QStringLiteral("/arrived"));

    auto target = std::make_shared<GatedFileSystem>(m_memory);

    TransferTask::Request request;
    request.mode = TransferTask::Mode::Move;
    request.sourceFileSystem = m_memory;
    request.targetFileSystem = target;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///source/report.bin")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));

    auto* task = new TransferTask(request);
    // The listing of the destination is the arrival check; nothing else in this
    // job lists anything.
    target->stopAt(GatedFileSystem::At::List);
    m_tasks->submit(task);
    QVERIFY(waitFor([&] { return target->isWaiting(); }));

    task->requestCancel();
    target->release();
    QVERIFY(waitForTask(task, kStopsWithinMs));

    QCOMPARE(task->state(), Task::State::Cancelled);
    QVERIFY2(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///source/report.bin"))).ok(),
        "a move interrupted before its delete pass has to leave the source alone");
}

void TestCancellation::cancellingDuringAMovesDeletePassKeepsWhatIsLeft()
{
    // Half way through deleting the sources of a move. What has already been
    // deleted arrived at the destination first, so nothing is lost -- and what
    // has not been deleted must stay, rather than being swept up on the way out.
    for (int i = 0; i < 4; ++i)
        m_memory->addFile(QStringLiteral("/source/file%1.bin").arg(i), QByteArray(1024, 'm'));
    m_memory->addDirectory(QStringLiteral("/arrived"));

    auto source = std::make_shared<GatedFileSystem>(m_memory);

    TransferTask::Request request;
    request.mode = TransferTask::Mode::Move;
    request.sourceFileSystem = source;
    request.targetFileSystem = m_memory;
    for (int i = 0; i < 4; ++i)
        request.sources.append(VfsUri::fromString(QStringLiteral("mem:///source/file%1.bin").arg(i)));
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));

    auto* task = new TransferTask(request);
    source->stopAt(GatedFileSystem::At::Remove);
    m_tasks->submit(task);
    QVERIFY(waitFor([&] { return source->isWaiting(); }));

    task->requestCancel();
    source->release();
    QVERIFY(waitForTask(task, kStopsWithinMs));

    // Every file arrived before any was deleted, which is what makes stopping
    // here safe at all.
    for (int i = 0; i < 4; ++i) {
        QVERIFY2(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///arrived/file%1.bin").arg(i))).ok(),
            "the copies were finished before the delete pass began");
    }
    // And the delete pass stopped rather than running to the end.
    QVERIFY2(source->removes() < 4, "a cancelled delete pass must not delete everything anyway");
}

void TestCancellation::cancellingTwiceInQuickSuccessionIsStillOneOutcome()
{
    // Somebody clicks the cross twice, or a shortcut and a button both fire. The
    // second cancel arrives while the first is being acted on.
    m_memory->addFile(QStringLiteral("/source/report.bin"), QByteArray(64 * 1024, 'r'));
    m_memory->addDirectory(QStringLiteral("/arrived"));

    auto source = std::make_shared<FaultyFileSystem>(m_memory);
    source->readStallsAt(8 * 1024);

    TransferTask::Request request;
    request.sourceFileSystem = source;
    request.targetFileSystem = m_memory;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///source/report.bin")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitFor([&] { return source->isStalled(); }));

    task->requestCancel();
    task->requestCancel();
    source->release();
    QVERIFY(waitForTask(task, kStopsWithinMs));
    task->requestCancel(); // and once more, after it is over

    QCOMPARE(task->state(), Task::State::Cancelled);
    QCOMPARE(task->copiedCount(), 0);
    QVERIFY(!m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///arrived/report.bin"))).ok());
}

MOLE_TEST_MAIN(TestCancellation)

#include "tst_Cancellation.moc"
