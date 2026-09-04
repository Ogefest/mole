#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/ReadRangeTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QFile>

using namespace mole;
using namespace mole::test;

/// Reading a window out of a file, which is what makes previewing something
/// far larger than memory possible at all.
class TestReadRangeTask : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void readsTheHeadOfAFile();
    void seeksPastTheStart();
    void snapsTheWindowToWholeLines();
    void reportsWhenMoreFollows();
    void theLastWindowSaysSo();
    void snappingTheStartDoesNotShortenTheWindow();
    void reportsTheWholeFileSize();
    void neverReadsMoreThanAskedFor();
    void refusesToSeekOnAStreamOnlyDrive();
    void failsOnAFileThatIsNotThere();

private:
    /// Runs a task to completion on the pool, the way the application does.
    ReadRangeTask* readRange(qint64 offset, qint64 length, bool align = true);

    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<TaskManager> m_tasks;
    FileSystemPtr m_fs;
    VfsUri m_file;
};

void TestReadRangeTask::init()
{
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());

    // Fixed-width lines make every offset arithmetic in the assertions exact.
    QByteArray contents;
    for (int line = 0; line < 1000; ++line)
        contents += QStringLiteral("line %1\n").arg(line, 6, 10, QLatin1Char('0')).toLatin1();
    QCOMPARE(contents.size(), 1000 * 12); // "line 000000\n" is 12 bytes

    QVERIFY(m_tree->writeFile(QStringLiteral("big.log"), contents));

    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<LocalFileSystem>();
    m_file = m_tree->rootUri().child(QStringLiteral("big.log"));
}

void TestReadRangeTask::cleanup()
{
    m_tasks.reset();
    m_fs.reset();
    m_tree.reset();
}

ReadRangeTask* TestReadRangeTask::readRange(qint64 offset, qint64 length, bool align)
{
    auto* task = new ReadRangeTask(m_fs, m_file, offset, length);
    task->setAlignToLines(align);
    m_tasks->submit(task);
    if (!waitFor([task] { return task->isFinished(); }, 10000))
        return nullptr;
    return task;
}

void TestReadRangeTask::readsTheHeadOfAFile()
{
    ReadRangeTask* task = readRange(0, 120);
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->actualOffset(), 0);
    QVERIFY(task->contents().startsWith("line 000000\n"));
    QCOMPARE(task->contents().size(), 120); // exactly ten lines
}

void TestReadRangeTask::seeksPastTheStart()
{
    // Line 500 begins at byte 6000. Nothing before it should be transferred,
    // which is the entire point -- at 100 GB, reading and discarding is not an
    // option.
    ReadRangeTask* task = readRange(6000, 24);
    QVERIFY(task);
    QCOMPARE(task->actualOffset(), 6000);
    QVERIFY(task->contents().startsWith("line 000500\n"));
}

void TestReadRangeTask::snapsTheWindowToWholeLines()
{
    // Asking from the middle of line 500 must back up to its start, or paging
    // through a log shows a severed line at every step.
    ReadRangeTask* task = readRange(6005, 100);
    QVERIFY(task);
    QCOMPARE(task->actualOffset(), 6000);
    QVERIFY(task->contents().startsWith("line 000500\n"));

    // And with snapping off, the caller gets exactly what it asked for.
    ReadRangeTask* raw = readRange(6005, 100, false);
    QVERIFY(raw);
    QCOMPARE(raw->actualOffset(), 6005);
    QVERIFY(raw->contents().startsWith("000500\n"));
}

void TestReadRangeTask::reportsWhenMoreFollows()
{
    ReadRangeTask* task = readRange(0, 120);
    QVERIFY(task);
    QVERIFY2(task->hasMore(), "the other 11880 bytes follow a 120 byte window");
}

void TestReadRangeTask::theLastWindowSaysSo()
{
    ReadRangeTask* task = readRange(11880, 1000); // the final ten lines
    QVERIFY(task);
    QVERIFY2(!task->hasMore(), "nothing follows the end of the file");
    QVERIFY(task->contents().endsWith("line 000999\n"));
}

void TestReadRangeTask::snappingTheStartDoesNotShortenTheWindow()
{
    // Asking from mid-line snaps the start backwards. If that shortened the
    // window, the last window of a file could never reach its end and paging
    // forward would stop one step early, for ever.
    ReadRangeTask* task = readRange(11885, 115);
    QVERIFY(task);
    QCOMPARE(task->actualOffset(), 11880);
    QVERIFY2(!task->hasMore(), "the window still covers the byte it was asked to end on");
    QVERIFY(task->contents().endsWith("line 000999\n"));
}

void TestReadRangeTask::reportsTheWholeFileSize()
{
    ReadRangeTask* task = readRange(0, 10);
    QVERIFY(task);
    // The window is tiny; the size reported is the file's, which is what the
    // position bar needs to mean anything.
    QCOMPARE(task->fileSize(), 12000);
}

void TestReadRangeTask::neverReadsMoreThanAskedFor()
{
    ReadRangeTask* task = readRange(0, 500);
    QVERIFY(task);
    QVERIFY2(task->contents().size() <= 500, "the window is a ceiling, not a hint");
}

void TestReadRangeTask::refusesToSeekOnAStreamOnlyDrive()
{
    // **This case had never executed.** It built a MemoryFileSystem and then
    // skipped when that drive could seek -- which it always can, so the guard was
    // always true and the seven lines after it had never run once. The claim they
    // hold is the one ADR-0027 and the span loop exist for: a ranged read on a
    // drive that cannot seek is refused rather than answered from offset 0.
    //
    // A drive that really cannot: FaultyFileSystem::cannotSeek() drops the
    // capability, which is what a backend streaming over a socket is, and is the
    // flag ReadRangeTask actually asks about. See MOLE-399.
    auto memory = std::make_shared<MemoryFileSystem>();
    const VfsUri root = VfsUri::fromString(QStringLiteral("mem://test/"));
    const VfsUri file = root.child(QStringLiteral("a.txt"));
    memory->addFile(QStringLiteral("/a.txt"), QByteArray("hello there"));

    auto streamOnly = std::make_shared<FaultyFileSystem>(memory);
    streamOnly->cannotSeek();
    QVERIFY(streamOnly->capabilities().testFlag(VfsCapability::Read));
    QVERIFY2(!streamOnly->capabilities().testFlag(VfsCapability::RandomAccessRead),
        "the fixture still advertises seeking, so there is nothing to refuse");

    auto* task = new ReadRangeTask(streamOnly, file, 4, 10);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 10000));

    // Better to fail than to quietly read and discard four gigabytes to
    // simulate a seek the drive cannot do.
    QCOMPARE(task->state(), Task::State::Failed);
    QCOMPARE(task->error().code, VfsError::NotSupported);

    // And a read from the beginning still works on the same drive, so what is
    // being refused is the seek rather than the drive.
    auto* fromTheStart = new ReadRangeTask(streamOnly, file, 0, 5);
    m_tasks->submit(fromTheStart);
    QVERIFY(waitFor([fromTheStart] { return fromTheStart->isFinished(); }, 10000));
    QCOMPARE(fromTheStart->state(), Task::State::Succeeded);
}

void TestReadRangeTask::failsOnAFileThatIsNotThere()
{
    auto* task = new ReadRangeTask(m_fs, m_tree->rootUri().child(QStringLiteral("nope.log")), 0, 10);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 10000));
    QCOMPARE(task->state(), Task::State::Failed);
}

MOLE_TEST_MAIN(TestReadRangeTask)
#include "tst_ReadRangeTask.moc"
