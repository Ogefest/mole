#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/FolderSizesTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QFile>

using namespace mole;
using namespace mole::test;

/// Totalling up folders, which is the answer to "which of these is the big one"
/// without opening an analysis and reading a report.
class TestFolderSizesTask : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void totalsEachFolderIncludingWhatIsNested();
    void answersArriveOneFolderAtATime();
    void anEmptyFolderIsZeroRatherThanSilence();
    void countsFilesButNotTheFoldersThemselves();
    void cancellingStopsWithoutReportingAHalfTotal();
    void reportsWhatItCanReadOfAnUnreadableTree();

private:
    struct Sized
    {
        QString uri;
        qint64 bytes = 0;
        qint64 files = 0;
    };

    /// Runs the task to completion on the pool, collecting what it announced.
    QList<Sized> measure(const QStringList& relativeFolders, bool cancelImmediately = false);

    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<TaskManager> m_tasks;
    FileSystemPtr m_fs;
};

void TestFolderSizesTask::init()
{
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());

    // Known sizes, and a nested one, because the whole point is that a total
    // includes what is underneath.
    QVERIFY(m_tree->makeDirs(QStringLiteral("small")));
    QVERIFY(m_tree->writeFile(QStringLiteral("small/a.bin"), QByteArray(100, 'a')));

    QVERIFY(m_tree->makeDirs(QStringLiteral("big/deeper/deepest")));
    QVERIFY(m_tree->writeFile(QStringLiteral("big/b.bin"), QByteArray(1000, 'b')));
    QVERIFY(m_tree->writeFile(QStringLiteral("big/deeper/c.bin"), QByteArray(2000, 'c')));
    QVERIFY(m_tree->writeFile(QStringLiteral("big/deeper/deepest/d.bin"), QByteArray(4000, 'd')));

    QVERIFY(m_tree->makeDirs(QStringLiteral("empty")));

    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<LocalFileSystem>();
}

void TestFolderSizesTask::cleanup()
{
    m_tasks.reset();
    m_tree.reset();
}

QList<TestFolderSizesTask::Sized> TestFolderSizesTask::measure(
    const QStringList& relativeFolders, bool cancelImmediately)
{
    QList<VfsUri> folders;
    for (const QString& relative : relativeFolders)
        folders.append(m_tree->rootUri().child(relative));

    auto* task = new FolderSizesTask(m_fs, folders);
    QList<Sized> seen;
    connect(
        task, &FolderSizesTask::folderSized, task, [&seen](const VfsUri& folder, qint64 bytes, qint64 files) {
            seen.append(Sized { folder.toString(), bytes, files });
        });

    m_tasks->submit(task);
    if (cancelImmediately)
        task->requestCancel();
    waitFor([task] { return task->isFinished(); }, 15000);
    return seen;
}

void TestFolderSizesTask::totalsEachFolderIncludingWhatIsNested()
{
    const QList<Sized> seen = measure({ QStringLiteral("small"), QStringLiteral("big") });
    QCOMPARE(seen.size(), 2);

    QCOMPARE(seen.at(0).bytes, 100);
    // 1000 at the top, 2000 one down, 4000 two down. A total that stopped at the
    // first level would say 1000 and look entirely plausible.
    QCOMPARE(seen.at(1).bytes, 7000);
}

void TestFolderSizesTask::answersArriveOneFolderAtATime()
{
    const QList<Sized> seen = measure({ QStringLiteral("big"), QStringLiteral("small") });

    // In the order asked for, one signal each, so a listing fills in as the walk
    // goes rather than all at once when everything is done.
    QCOMPARE(seen.size(), 2);
    QVERIFY(seen.at(0).uri.endsWith(QStringLiteral("/big")));
    QVERIFY(seen.at(1).uri.endsWith(QStringLiteral("/small")));
}

void TestFolderSizesTask::anEmptyFolderIsZeroRatherThanSilence()
{
    const QList<Sized> seen = measure({ QStringLiteral("empty") });

    // Zero is an answer. Saying nothing would leave the row looking unmeasured
    // for ever, which is the one thing the row must not do.
    QCOMPARE(seen.size(), 1);
    QCOMPARE(seen.first().bytes, 0);
    QCOMPARE(seen.first().files, 0);
}

void TestFolderSizesTask::countsFilesButNotTheFoldersThemselves()
{
    const QList<Sized> seen = measure({ QStringLiteral("big") });
    QCOMPARE(seen.size(), 1);

    // Three files under big/, and the directories in between are not stock to be
    // counted -- on most backends they have a size of their own, and adding it
    // would inflate every total by a few kilobytes per folder.
    QCOMPARE(seen.first().files, 3);
    QCOMPARE(seen.first().bytes, 7000);
}

void TestFolderSizesTask::cancellingStopsWithoutReportingAHalfTotal()
{
    // On a local disk this cannot be tested honestly: folderSized is queued to
    // this thread, and by the time the cancel is sent the worker has already
    // finished the next folder -- both answers arrive and the test proves
    // nothing. So the folders are put on a drive that takes its time listing
    // them, which is what makes the cancel land mid-walk.
    auto slow = std::make_shared<MemoryFileSystem>();
    slow->addFile(QStringLiteral("/first/a.bin"), QByteArray(100, 'a'));
    slow->addFile(QStringLiteral("/second/b.bin"), QByteArray(200, 'b'));
    slow->setListDelayMs(300);

    const QList<VfsUri> folders { VfsUri::fromString(QStringLiteral("mem://slow/first")),
        VfsUri::fromString(QStringLiteral("mem://slow/second")) };

    auto* task = new FolderSizesTask(slow, folders);
    QList<Sized> seen;
    connect(task, &FolderSizesTask::folderSized, task,
        [&seen, task](const VfsUri& folder, qint64 bytes, qint64 files) {
            seen.append(Sized { folder.toString(), bytes, files });
            task->requestCancel();
        });

    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 15000));

    // One answer, and a whole one. A folder counted half way and announced as a
    // total would put a wrong number in a listing, which is worse than none.
    QCOMPARE(seen.size(), 1);
    QVERIFY(seen.first().uri.endsWith(QStringLiteral("/first")));
    QCOMPARE(seen.first().bytes, 100);
}

void TestFolderSizesTask::reportsWhatItCanReadOfAnUnreadableTree()
{
    // A directory nobody may enter, beside one that can be read. The walker
    // records the error and carries on, and "at least this much" is more use in a
    // listing than a blank.
    QVERIFY(m_tree->makeDirs(QStringLiteral("mixed/open")));
    QVERIFY(m_tree->writeFile(QStringLiteral("mixed/open/e.bin"), QByteArray(500, 'e')));
    QVERIFY(m_tree->makeDirs(QStringLiteral("mixed/shut")));
    QVERIFY(m_tree->writeFile(QStringLiteral("mixed/shut/f.bin"), QByteArray(9000, 'f')));

    const QString shut = m_tree->path() + QStringLiteral("/mixed/shut");
    if (!QFile::setPermissions(shut, QFileDevice::ReadOwner))
        QSKIP("cannot make a directory unreadable here");

    const QList<Sized> seen = measure({ QStringLiteral("mixed") });
    QFile::setPermissions(shut, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QCOMPARE(seen.size(), 1);
    QVERIFY2(seen.first().bytes >= 500, "what could be read is still reported");
}

MOLE_TEST_MAIN(TestFolderSizesTask)
#include "tst_FolderSizesTask.moc"
