#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Delete, which is the operation with nothing to compare afterwards.
///
/// Every other operation can be checked against what it produced. A delete
/// produces nothing, so the only question worth asking is what *else* went with
/// it -- and the answers that matter are all about the edges: a link followed
/// out of the tree, a root taken literally, a path that climbs out.
class TestDelete : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aSymlinkToADirectoryIsUnlinkedRatherThanEmptied();
    void aSymlinkInsideADeletedTreeDoesNotTakeItsTargetWithIt();
    void aRefusalPartWayThroughIsReportedAndTheRestStillGoes();
    void aVeryDeepAndVeryWideTreeIsRemovedWholesale();
    void deletingTheDriveRootIsRefused();
    void aPathThatClimbsOutOfTheDriveCannotReachAboveIt();
    void aFileBeingReadWhileItIsDeletedDoesNotTakeTheReaderWithIt();

private:
    DeleteTask* remove(const FileSystemPtr& drive, const QList<VfsUri>& targets);

    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::unique_ptr<TempTree> m_tree;
};

void TestDelete::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_disk = std::make_shared<LocalFileSystem>();
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
}

void TestDelete::cleanup()
{
    m_tasks.reset();
    m_memory.reset();
    m_disk.reset();
    m_tree.reset();
}

DeleteTask* TestDelete::remove(const FileSystemPtr& drive, const QList<VfsUri>& targets)
{
    auto* task = new DeleteTask(drive, targets);
    m_tasks->submit(task);
    return waitForTask(task, 60000) ? task : nullptr;
}

void TestDelete::aSymlinkToADirectoryIsUnlinkedRatherThanEmptied()
{
    // The worst delete there is. A link is a name, and removing the name is the
    // whole job -- but a link to a directory *looks* like a directory to
    // anything that asks, so a recursive delete that does not check walks
    // through it and empties whatever it points at. That is how deleting a
    // scratch folder takes somebody's home directory with it.
    QVERIFY(m_tree->makeDirs(QStringLiteral("elsewhere")));
    QVERIFY(m_tree->writeFile(QStringLiteral("elsewhere/precious.txt"), QByteArray("do not lose me")));
    QVERIFY(QFile::link(
        m_tree->absolute(QStringLiteral("elsewhere")), m_tree->absolute(QStringLiteral("shortcut"))));

    DeleteTask* task = remove(m_disk, { m_tree->rootUri().child(QStringLiteral("shortcut")) });
    QVERIFY(task != nullptr);

    // Asserted before the outcome of the delete, so a failure here says what was
    // lost rather than what was reported.
    QVERIFY2(QFile::exists(m_tree->absolute(QStringLiteral("elsewhere/precious.txt"))),
        "a delete of a link must not reach through it");
    QVERIFY(QFileInfo(m_tree->absolute(QStringLiteral("elsewhere"))).isDir());

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QVERIFY2(!QFileInfo(m_tree->absolute(QStringLiteral("shortcut"))).isSymLink(), "the link is the target");
}

void TestDelete::aSymlinkInsideADeletedTreeDoesNotTakeItsTargetWithIt()
{
    // The same rule one level down, where it is easier to get right by accident
    // and just as expensive to get wrong.
    QVERIFY(m_tree->makeDirs(QStringLiteral("elsewhere")));
    QVERIFY(m_tree->writeFile(QStringLiteral("elsewhere/precious.txt"), QByteArray("do not lose me")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("scratch/inner")));
    QVERIFY(m_tree->writeFile(QStringLiteral("scratch/inner/rubbish.txt"), QByteArray("go ahead")));
    QVERIFY(QFile::link(m_tree->absolute(QStringLiteral("elsewhere")),
        m_tree->absolute(QStringLiteral("scratch/inner/shortcut"))));

    DeleteTask* task = remove(m_disk, { m_tree->rootUri().child(QStringLiteral("scratch")) });
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));

    QVERIFY(!QFile::exists(m_tree->absolute(QStringLiteral("scratch"))));
    QVERIFY2(QFile::exists(m_tree->absolute(QStringLiteral("elsewhere/precious.txt"))),
        "a link inside a deleted tree is a name in that tree, not a door out of it");
}

void TestDelete::aRefusalPartWayThroughIsReportedAndTheRestStillGoes()
{
    // A folder somebody else owns, in the middle of a selection. Stopping at it
    // would leave the job half done with no record of where; carrying on
    // silently would report a delete that did not happen. Both of those are
    // worse than the answer here: everything that could go, went, and the one
    // that could not is named.
    for (int i = 0; i < 5; ++i)
        m_memory->addFile(QStringLiteral("/data/file%1.txt").arg(i), QByteArray("x"));

    auto drive = std::make_shared<FaultyFileSystem>(m_memory);
    drive->removeFails(
        QStringLiteral("/data/file2.txt"), VfsError::AccessDenied, QStringLiteral("permission denied"));

    QList<VfsUri> targets;
    for (int i = 0; i < 5; ++i)
        targets.append(VfsUri::fromString(QStringLiteral("mem:///data/file%1.txt").arg(i)));

    DeleteTask* task = remove(drive, targets);
    QVERIFY(task != nullptr);
    QCOMPARE(task->deletedCount(), 4);
    QCOMPARE(task->failures().size(), 1);
    QVERIFY2(
        task->failures().first().contains(QStringLiteral("file2.txt")), qPrintable(task->failures().first()));

    // And the count is honest about what is actually gone.
    for (int i = 0; i < 5; ++i) {
        const bool there
            = m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///data/file%1.txt").arg(i))).ok();
        QCOMPARE(there, i == 2);
    }
}

void TestDelete::aVeryDeepAndVeryWideTreeIsRemovedWholesale()
{
    // Depth for the recursion and width for the bookkeeping. Five hundred levels
    // is past anything a person makes and short of any path limit, which makes
    // it a test of the code rather than of the filesystem.
    QString deep;
    for (int level = 0; level < 500; ++level) {
        deep += QStringLiteral("/level%1").arg(level);
        m_memory->addFile(QStringLiteral("/tree%1/leaf.txt").arg(deep), QByteArray("x"));
    }
    for (int i = 0; i < 5000; ++i)
        m_memory->addFile(QStringLiteral("/tree/wide/file%1.txt").arg(i), QByteArray("x"));

    DeleteTask* task = remove(m_memory, { VfsUri::fromString(QStringLiteral("mem:///tree")) });
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->deletedCount(), 1);

    QVERIFY(!m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///tree"))).ok());
    QVERIFY(!m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///tree/wide/file4999.txt"))).ok());
}

void TestDelete::deletingTheDriveRootIsRefused()
{
    // Not a thing anybody types on purpose: it is what a selection of everything
    // collapses to, or what an empty path resolves to. There is no undo, and the
    // drive is somebody's whole disk.
    m_memory->addFile(QStringLiteral("/data/keep.txt"), QByteArray("still here"));

    DeleteTask* task = remove(m_memory, { VfsUri::fromString(QStringLiteral("mem:///")) });
    QVERIFY(task != nullptr);
    QCOMPARE(task->deletedCount(), 0);
    QCOMPARE(task->failures().size(), 1);
    QVERIFY2(task->failures().first().contains(QStringLiteral("root")), qPrintable(task->failures().first()));
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///data/keep.txt"))).ok());
}

void TestDelete::aPathThatClimbsOutOfTheDriveCannotReachAboveIt()
{
    // ".." in a path is resolved when the uri is built, and it stops at the
    // root rather than climbing past it. That is what makes a drive rooted at a
    // directory a boundary rather than a suggestion -- a delete is where the
    // difference costs something.
    m_memory->addFile(QStringLiteral("/data/inner/gone.txt"), QByteArray("x"));
    m_memory->addFile(QStringLiteral("/data/keep.txt"), QByteArray("x"));

    const VfsUri climbing = VfsUri::fromString(QStringLiteral("mem:///data/inner/../../../../etc"));
    QCOMPARE(climbing.path(), QStringLiteral("/etc"));

    DeleteTask* task = remove(m_memory, { climbing });
    QVERIFY(task != nullptr);
    // Nothing called /etc exists on this drive, so the delete finds nothing --
    // and, crucially, nothing above the drive root was ever addressed.
    QCOMPARE(task->deletedCount(), 0);
    QCOMPARE(task->failures().size(), 1);
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///data/keep.txt"))).ok());
}

void TestDelete::aFileBeingReadWhileItIsDeletedDoesNotTakeTheReaderWithIt()
{
    // Two tasks over one file: a preview open on it and a delete going through.
    // Either order is a legitimate outcome -- the read finishes from an open
    // handle, or it fails because the file went away. A crash is not, and
    // neither is a read that returns rubbish.
    QVERIFY(m_tree->makeDirs(QStringLiteral("data")));
    const QByteArray payload(64 * 1024, 'z');
    QVERIFY(m_tree->writeFile(QStringLiteral("data/report.bin"), payload));

    const VfsUri target = m_tree->rootUri().child(QStringLiteral("data")).child(QStringLiteral("report.bin"));
    Result<std::unique_ptr<QIODevice>> reader = m_disk->openRead(target);
    QVERIFY2(reader.ok(), qPrintable(reader.error().message));
    QCOMPARE(reader.value()->read(1024).size(), 1024);

    DeleteTask* task = remove(m_disk, { target });
    QVERIFY(task != nullptr);
    QCOMPARE(task->deletedCount(), 1);
    QVERIFY(!QFile::exists(m_tree->absolute(QStringLiteral("data/report.bin"))));

    // On a filesystem with open-unlink semantics the rest still arrives; on one
    // without, the read fails. Both are answers. The assertion is that whatever
    // comes back is either the file's own bytes or a failure.
    const QByteArray rest = reader.value()->readAll();
    QVERIFY(rest.isEmpty() || rest == payload.mid(1024));
}

MOLE_TEST_MAIN(TestDelete)

#include "tst_Delete.moc"
