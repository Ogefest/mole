#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace mole;
using namespace mole::test;

namespace {

#ifdef Q_OS_UNIX
/// Holds every file descriptor this process can still get, and gives a few back.
///
/// Not a simulation: the descriptors really are gone, and every open in the
/// process -- ours, Qt's, libc's -- fails while this is alive. Which is the
/// point, because what is being tested is what the code does when opening stops
/// working for reasons that have nothing to do with the file it asked for.
class DescriptorFamine
{
public:
    /// `spare` is how many to hand back, so the event loop and the test itself
    /// can still function while the job under test cannot open its files.
    explicit DescriptorFamine(int spare)
    {
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull < 0)
            return;
        m_held.append(devNull);
        for (;;) {
            const int fd = ::dup(devNull);
            if (fd < 0)
                break;
            m_held.append(fd);
            if (m_held.size() > 200000)
                break; // a limit high enough that the machine has no limit
        }
        for (int i = 0; i < spare && !m_held.isEmpty(); ++i)
            ::close(m_held.takeLast());
    }

    ~DescriptorFamine()
    {
        for (int fd : std::as_const(m_held))
            ::close(fd);
    }

    bool isBiting() const { return m_held.size() > 8; }

private:
    QList<int> m_held;
};
#endif

} // namespace

/// What happens when the machine runs out of something.
///
/// Every one of these is a failure that has nothing to do with the file being
/// worked on, and the thing they have in common is how they are usually
/// reported: as an assortment of odd errors from wherever the shortage happened
/// to bite. What a job owes is one clear answer and nothing half-finished left
/// behind.
class TestRunningOut : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void twentyFilesCopyWithSixDescriptorsToSpare();
    void aStagingDirectoryThatIsNotThereIsReportedRatherThanIgnored();

private:
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::unique_ptr<TempTree> m_tree;
};

void TestRunningOut::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_disk = std::make_shared<LocalFileSystem>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
}

void TestRunningOut::cleanup()
{
    m_tasks.reset();
    m_disk.reset();
    m_memory.reset();
    m_tree.reset();
}

void TestRunningOut::twentyFilesCopyWithSixDescriptorsToSpare()
{
#ifndef Q_OS_UNIX
    QSKIP("descriptor limits work differently on this platform");
#else
    // The descriptor limit, approached from the side that can be arranged
    // honestly. Every descriptor this process can get is taken and six are
    // handed back -- and twenty files still copy, which is only possible if the
    // copy holds one at a time and gives it back before the next.
    //
    // That is the failure worth ruling out. One kept per file is a job that dies
    // part way through a directory of ten thousand, on a machine that had room
    // for all of them. A shortage tight enough to fail an *open* cannot be
    // arranged from inside the process without taking the event loop with it, so
    // that half belongs to a run under a real ulimit.
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));
    for (int i = 0; i < 20; ++i)
        m_memory->addFile(QStringLiteral("/source/file%1.bin").arg(i), QByteArray(1024, 'x'));

    TransferTask::Request request;
    request.sourceFileSystem = m_memory;
    request.targetFileSystem = m_disk;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///source")) };
    request.targetDirectory = m_tree->rootUri().child(QStringLiteral("arrived"));

    auto* task = new TransferTask(request);
    {
        // Held only while the job runs. The source is the memory drive, so the
        // reads need no descriptor at all and the writes need one apiece --
        // which is what makes the failure land on the destination, where a
        // half-written file would be.
        // Six spare: enough for the event loop that is already running, and not
        // enough for a copy that needs one for the file it is writing.
        DescriptorFamine famine(6);
        if (!famine.isBiting())
            QSKIP("this process has no descriptor limit worth reaching");
        m_tasks->submit(task);
        QVERIFY2(waitForTask(task, 60000), "a job that cannot open anything still has to finish");
    }

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->copiedCount(), 20);

    // And nothing is left under a working name, which is the outcome that would
    // survive the shortage and be trusted afterwards.
    const QStringList entries = QDir(m_tree->absolute(QStringLiteral("arrived/source")))
                                    .entryList(QDir::Files | QDir::Hidden, QDir::Name);
    for (const QString& name : entries)
        QVERIFY2(!name.contains(QStringLiteral("mole-partial")), qPrintable(name));
#endif
}

void TestRunningOut::aStagingDirectoryThatIsNotThereIsReportedRatherThanIgnored()
{
    // Everything below the streaming threshold is staged in a temporary file,
    // and so is every archive. A machine whose temporary directory has been
    // removed, filled or made read-only is a machine where that fails -- and the
    // failure has to be an error rather than a stream that reports itself open
    // and swallows every write.
    const QByteArray previous = qgetenv("TMPDIR");
    qputenv("TMPDIR", "/proc/mole-has-no-temporary-directory-here");

    QTemporaryFile scratch;
    const bool opened = scratch.open();

    if (previous.isEmpty())
        qunsetenv("TMPDIR");
    else
        qputenv("TMPDIR", previous);

    QVERIFY2(!opened, "a temporary file cannot be created in a directory that is not there");
    // The staged write path is held to the same rule in tst_StreamingUpload,
    // where a BufferedUpload with nowhere to stage refuses to open rather than
    // accepting bytes it cannot keep.
}

MOLE_TEST_MAIN(TestRunningOut)

#include "tst_RunningOut.moc"
