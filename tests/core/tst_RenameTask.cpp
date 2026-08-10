#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/rename/RenamePlan.h"
#include "core/rename/RenameTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QFile>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Carrying out a batch of renames, where the order is the whole problem.
///
/// A rename onto a name that is still taken is refused, so a batch whose rows
/// pass names between them -- a swap, a chain of shifts, a photo set being
/// renumbered -- cannot simply be done in the order it was listed. The plan
/// already decides that such a batch is legal, on the grounds that every name is
/// free by the end of it. This is the half that has to make that true.
class TestRenameTask : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aSwapEndsWithBothFilesUnderTheOtherName();
    void aChainShiftsEveryFileAlongWithoutOverwritingOne();
    void aLongerCycleUnwindsToo();
    void aRowThatFailsIsNamedAndTheRestStillGo();
    void nothingIsLeftUnderAWorkingName();

private:
    /// Renames every `from -> to` pair in one batch, over the temp tree.
    RenameTask* rename(const QList<QPair<QString, QString>>& pairs);
    QByteArray contentsOf(const QString& name) const;

    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<TempTree> m_tree;
};

void TestRenameTask::init()
{
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());

    m_vfs = std::make_unique<VfsManager>();
    Mount mount;
    mount.id = QStringLiteral("local");
    mount.root = VfsUri::fromLocalPath(QStringLiteral("/"));
    mount.fileSystem = std::make_shared<LocalFileSystem>();
    m_vfs->addMount(mount);

    m_tasks = std::make_unique<TaskManager>();
}

void TestRenameTask::cleanup()
{
    m_tasks.reset();
    m_vfs.reset();
    m_tree.reset();
}

RenameTask* TestRenameTask::rename(const QList<QPair<QString, QString>>& pairs)
{
    QList<RenamePlan::Entry> entries;
    for (const auto& [from, to] : pairs) {
        RenamePlan::Entry entry;
        entry.source = m_tree->rootUri().child(from);
        entry.originalName = from;
        entry.newName = to;
        entries.append(entry);
    }

    auto* task = new RenameTask(m_vfs.get(), entries);
    m_tasks->submit(task);
    return waitForTask(task, 30000) ? task : nullptr;
}

QByteArray TestRenameTask::contentsOf(const QString& name) const
{
    QFile file(m_tree->absolute(name));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void TestRenameTask::aSwapEndsWithBothFilesUnderTheOtherName()
{
    // Two files trading names. Done in the order they were listed, the first
    // rename is refused because the second file is still standing on the name --
    // and the batch ends with nothing renamed and two failures, for a batch the
    // plan said was fine.
    QVERIFY(m_tree->writeFile(QStringLiteral("a.txt"), QByteArray("was a")));
    QVERIFY(m_tree->writeFile(QStringLiteral("b.txt"), QByteArray("was b")));

    RenameTask* task = rename({ { QStringLiteral("a.txt"), QStringLiteral("b.txt") },
        { QStringLiteral("b.txt"), QStringLiteral("a.txt") } });
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->renamedCount(), 2);

    QCOMPARE(contentsOf(QStringLiteral("a.txt")), QByteArray("was b"));
    QCOMPARE(contentsOf(QStringLiteral("b.txt")), QByteArray("was a"));
}

void TestRenameTask::aChainShiftsEveryFileAlongWithoutOverwritingOne()
{
    // Renumbering: every file takes the next one's name. Done in the listed
    // order each rename lands on an occupied name; done in reverse it works.
    // Nothing may be overwritten on the way, whichever order it picks.
    QVERIFY(m_tree->writeFile(QStringLiteral("1.txt"), QByteArray("one")));
    QVERIFY(m_tree->writeFile(QStringLiteral("2.txt"), QByteArray("two")));
    QVERIFY(m_tree->writeFile(QStringLiteral("3.txt"), QByteArray("three")));

    RenameTask* task = rename({ { QStringLiteral("1.txt"), QStringLiteral("2.txt") },
        { QStringLiteral("2.txt"), QStringLiteral("3.txt") },
        { QStringLiteral("3.txt"), QStringLiteral("4.txt") } });
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->renamedCount(), 3);

    QCOMPARE(contentsOf(QStringLiteral("2.txt")), QByteArray("one"));
    QCOMPARE(contentsOf(QStringLiteral("3.txt")), QByteArray("two"));
    QCOMPARE(contentsOf(QStringLiteral("4.txt")), QByteArray("three"));
    QVERIFY2(!QFile::exists(m_tree->absolute(QStringLiteral("1.txt"))), "the first name is given up");
}

void TestRenameTask::aLongerCycleUnwindsToo()
{
    // Three files going round: a to b, b to c, c to a. There is no order that
    // avoids an occupied name, so one of them has to stand somewhere else for a
    // moment.
    QVERIFY(m_tree->writeFile(QStringLiteral("a.txt"), QByteArray("was a")));
    QVERIFY(m_tree->writeFile(QStringLiteral("b.txt"), QByteArray("was b")));
    QVERIFY(m_tree->writeFile(QStringLiteral("c.txt"), QByteArray("was c")));

    RenameTask* task = rename({ { QStringLiteral("a.txt"), QStringLiteral("b.txt") },
        { QStringLiteral("b.txt"), QStringLiteral("c.txt") },
        { QStringLiteral("c.txt"), QStringLiteral("a.txt") } });
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->renamedCount(), 3);

    QCOMPARE(contentsOf(QStringLiteral("b.txt")), QByteArray("was a"));
    QCOMPARE(contentsOf(QStringLiteral("c.txt")), QByteArray("was b"));
    QCOMPARE(contentsOf(QStringLiteral("a.txt")), QByteArray("was c"));
}

void TestRenameTask::aRowThatFailsIsNamedAndTheRestStillGo()
{
    // A file that went away between the preview and the commit. Stopping there
    // would leave the batch half applied with no record of where; the row is
    // named and the others are carried out.
    QVERIFY(m_tree->writeFile(QStringLiteral("here.txt"), QByteArray("here")));
    QVERIFY(m_tree->writeFile(QStringLiteral("also.txt"), QByteArray("also")));

    QList<RenamePlan::Entry> entries;
    for (const auto& pair : { std::pair { QStringLiteral("gone.txt"), QStringLiteral("gone-new.txt") },
             std::pair { QStringLiteral("here.txt"), QStringLiteral("here-new.txt") },
             std::pair { QStringLiteral("also.txt"), QStringLiteral("also-new.txt") } }) {
        RenamePlan::Entry entry;
        entry.source = m_tree->rootUri().child(pair.first);
        entry.originalName = pair.first;
        entry.newName = pair.second;
        entries.append(entry);
    }

    auto* task = new RenameTask(m_vfs.get(), entries);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    QCOMPARE(task->renamedCount(), 2);
    QCOMPARE(task->failures().size(), 1);
    QVERIFY2(
        task->failures().first().contains(QStringLiteral("gone.txt")), qPrintable(task->failures().first()));
    QCOMPARE(contentsOf(QStringLiteral("here-new.txt")), QByteArray("here"));
    QCOMPARE(contentsOf(QStringLiteral("also-new.txt")), QByteArray("also"));
}

void TestRenameTask::nothingIsLeftUnderAWorkingName()
{
    // The temporary name a cycle needs is a real file for a moment. If a batch
    // ever ended with one still on disk, the user would be left with a file
    // called something they did not ask for and no way to know which.
    QVERIFY(m_tree->writeFile(QStringLiteral("a.txt"), QByteArray("was a")));
    QVERIFY(m_tree->writeFile(QStringLiteral("b.txt"), QByteArray("was b")));

    QVERIFY(rename({ { QStringLiteral("a.txt"), QStringLiteral("b.txt") },
        { QStringLiteral("b.txt"), QStringLiteral("a.txt") } }));

    const QStringList entries = QDir(m_tree->path()).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    QCOMPARE(entries, QStringList({ QStringLiteral("a.txt"), QStringLiteral("b.txt") }));
}

MOLE_TEST_MAIN(TestRenameTask)

#include "tst_RenameTask.moc"
