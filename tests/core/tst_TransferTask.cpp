#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/NameRules.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>

using namespace mole;
using namespace mole::test;

class TestTransferTask : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void copiesASingleFile();
    void copiesADirectoryTreeRecursively();
    void copiesAcrossDifferentBackends();
    void moveWithinOneBackendIsARename();
    void moveAcrossBackendsCopiesThenDeletes();
    void moveKeepsTheSourceWhenSomethingFailed();
    void conflictFailsByDefault();
    void conflictCanSkip();
    void conflictCanOverwrite();
    void mergesIntoAnExistingDirectory();
    void missingSourceIsRecordedNotFatal();
    void aNameTheDestinationRefusesFailsOneFileAndNotTheRun();
    void aRefusedNameInsideATreeIsOneFailureAndTheRestArrives();
    void readOnlyTargetFails();
    void aWriteThatFailsOnlyWhenClosedIsStillAFailure();
    void aReadThatStopsHalfWayIsNotAnEndOfFile();
    void aFileThatLandedShortIsCaughtAfterwards();
    void aMoveKeepsTheSourceWhenTheCheckFails();
    void cancellationStopsMidway();
    void emptyRequestSucceeds();

    void aLinkedDirectoryIsCopiedAsALinkAndNotAsAnEmptyFolder();
    void aLinkedFileIsCopiedAsALinkAndNotAsACopyOfItsTarget();
    void aRelativeLinkArrivesStillRelative();
    void aDriveThatCannotHoldALinkRefusesItByName();
    void aBrokenLinkSelectedOnItsOwnIsStillCopied();

    void deleteRemovesFilesAndTrees();
    void deleteReportsFailures();

private:
    QString readLocal(const QString& relative) const;

    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_mem;
    std::shared_ptr<LocalFileSystem> m_local;
    std::unique_ptr<TempTree> m_tree;
};

void TestTransferTask::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_mem = std::make_shared<MemoryFileSystem>();
    m_local = std::make_shared<LocalFileSystem>();
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
}

void TestTransferTask::cleanup()
{
    m_tasks.reset();
    m_mem.reset();
    m_local.reset();
    m_tree.reset();
}

QString TestTransferTask::readLocal(const QString& relative) const
{
    QFile file(m_tree->absolute(relative));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

void TestTransferTask::copiesASingleFile()
{
    m_mem->addFile(QStringLiteral("/src/note.txt"), QByteArray("payload"));
    m_mem->addDirectory(QStringLiteral("/dst"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///src/note.txt")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///dst"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->copiedCount(), 1);
    QVERIFY(task->failures().isEmpty());

    Result<std::unique_ptr<QIODevice>> copy
        = m_mem->openRead(VfsUri::fromString(QStringLiteral("mem:///dst/note.txt")));
    QVERIFY2(copy.ok(), qPrintable(copy.error().message));
    QCOMPARE(copy.value()->readAll(), QByteArray("payload"));

    // A copy leaves the original alone.
    QVERIFY(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///src/note.txt"))).ok());
}

void TestTransferTask::copiesADirectoryTreeRecursively()
{
    m_mem->addFile(QStringLiteral("/src/top.txt"), QByteArray("1"));
    m_mem->addFile(QStringLiteral("/src/deep/inner.txt"), QByteArray("22"));
    m_mem->addFile(QStringLiteral("/src/deep/deeper/leaf.txt"), QByteArray("333"));
    m_mem->addDirectory(QStringLiteral("/dst"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///src")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///dst"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->copiedCount(), 3);

    // The whole shape has to survive, directories included.
    QVERIFY(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///dst/src/deep/deeper"))).ok());
    Result<FileEntry> leaf
        = m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///dst/src/deep/deeper/leaf.txt")));
    QVERIFY2(leaf.ok(), qPrintable(leaf.error().message));
    QCOMPARE(leaf.value().size, 3);
}

void TestTransferTask::copiesAcrossDifferentBackends()
{
    // The whole point of the VFS layer: mem -> disk is the same code path as
    // disk -> disk, and later NAS -> S3 will be too.
    m_mem->addFile(QStringLiteral("/export/report.txt"), QByteArray("from memory"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_local;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///export/report.txt")) };
    request.targetDirectory = m_tree->rootUri();

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(readLocal(QStringLiteral("report.txt")), QStringLiteral("from memory"));
}

void TestTransferTask::moveWithinOneBackendIsARename()
{
    m_mem->addFile(QStringLiteral("/a/big.bin"), QByteArray(4096, 'x'));
    m_mem->addDirectory(QStringLiteral("/b"));

    const int listCallsBefore = m_mem->listCallCount();

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/big.bin")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///b"));
    request.mode = TransferTask::Mode::Move;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(!m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///a/big.bin"))).ok());
    QVERIFY(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///b/big.bin"))).ok());

    // Renaming must not walk or stream anything.
    QCOMPARE(m_mem->listCallCount(), listCallsBefore);
}

void TestTransferTask::moveAcrossBackendsCopiesThenDeletes()
{
    m_mem->addFile(QStringLiteral("/out/data.txt"), QByteArray("moving"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_local;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///out/data.txt")) };
    request.targetDirectory = m_tree->rootUri();
    request.mode = TransferTask::Mode::Move;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(readLocal(QStringLiteral("data.txt")), QStringLiteral("moving"));
    QVERIFY2(!m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///out/data.txt"))).ok(),
        "the source must be gone after a successful move");
}

void TestTransferTask::moveKeepsTheSourceWhenSomethingFailed()
{
    m_mem->addFile(QStringLiteral("/out/good.txt"), QByteArray("ok"));
    m_mem->addFile(QStringLiteral("/out/bad.txt"), QByteArray("nope"));
    m_mem->setFault(QStringLiteral("/out/bad.txt"), VfsError::IoError);

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_local;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///out/good.txt")),
        VfsUri::fromString(QStringLiteral("mem:///out/bad.txt")) };
    request.targetDirectory = m_tree->rootUri();
    request.mode = TransferTask::Mode::Move;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QVERIFY(!task->failures().isEmpty());
    // Losing data to tidy up after a half-failed copy is never an acceptable
    // trade, so nothing is deleted when anything went wrong.
    QVERIFY2(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///out/good.txt"))).ok(),
        "a partly failed move must not delete any source");
}

void TestTransferTask::conflictFailsByDefault()
{
    m_mem->addFile(QStringLiteral("/a/same.txt"), QByteArray("new"));
    m_mem->addFile(QStringLiteral("/b/same.txt"), QByteArray("existing"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/same.txt")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///b"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->copiedCount(), 0);
    QCOMPARE(task->failedCount(), 1);

    Result<std::unique_ptr<QIODevice>> kept
        = m_mem->openRead(VfsUri::fromString(QStringLiteral("mem:///b/same.txt")));
    QVERIFY(kept.ok());
    QCOMPARE(kept.value()->readAll(), QByteArray("existing"));
}

void TestTransferTask::conflictCanSkip()
{
    m_mem->addFile(QStringLiteral("/a/same.txt"), QByteArray("new"));
    m_mem->addFile(QStringLiteral("/a/fresh.txt"), QByteArray("fresh"));
    m_mem->addFile(QStringLiteral("/b/same.txt"), QByteArray("existing"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/same.txt")),
        VfsUri::fromString(QStringLiteral("mem:///a/fresh.txt")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///b"));
    request.onConflict = TransferTask::Conflict::Skip;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->skippedCount(), 1);
    QCOMPARE(task->copiedCount(), 1);
    QVERIFY(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///b/fresh.txt"))).ok());
}

void TestTransferTask::conflictCanOverwrite()
{
    m_mem->addFile(QStringLiteral("/a/same.txt"), QByteArray("new"));
    m_mem->addFile(QStringLiteral("/b/same.txt"), QByteArray("existing"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/same.txt")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///b"));
    request.onConflict = TransferTask::Conflict::Overwrite;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    Result<std::unique_ptr<QIODevice>> replaced
        = m_mem->openRead(VfsUri::fromString(QStringLiteral("mem:///b/same.txt")));
    QVERIFY(replaced.ok());
    QCOMPARE(replaced.value()->readAll(), QByteArray("new"));
}

void TestTransferTask::mergesIntoAnExistingDirectory()
{
    m_mem->addFile(QStringLiteral("/a/shared/one.txt"), QByteArray("1"));
    m_mem->addFile(QStringLiteral("/b/shared/two.txt"), QByteArray("2"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/shared")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///b"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    // A directory that already exists is a merge, not a clash.
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->failures().isEmpty());
    QVERIFY(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///b/shared/one.txt"))).ok());
    QVERIFY(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///b/shared/two.txt"))).ok());
}

void TestTransferTask::missingSourceIsRecordedNotFatal()
{
    m_mem->addFile(QStringLiteral("/a/real.txt"), QByteArray("here"));
    m_mem->addDirectory(QStringLiteral("/b"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/ghost.txt")),
        VfsUri::fromString(QStringLiteral("mem:///a/real.txt")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///b"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    // One bad entry must not abandon the rest of the batch.
    QCOMPARE(task->failedCount(), 1);
    QCOMPARE(task->copiedCount(), 1);
    QVERIFY(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///b/real.txt"))).ok());
}

void TestTransferTask::aWriteThatFailsOnlyWhenClosedIsStillAFailure()
{
    // Regression. The copy loop called close() on the target and ignored it, and
    // every remote backend commits in close() -- so an upload that failed was
    // reported as a file successfully copied. A copy that silently did not happen
    // is the worst outcome available, because nothing later looks wrong.
    m_mem->addFile(QStringLiteral("/a/file.txt"), QByteArray("payload"));

    auto inner = std::make_shared<MemoryFileSystem>();
    inner->addDirectory(QStringLiteral("/target"));
    auto target = std::make_shared<FaultyFileSystem>(inner);
    target->writeFailsOnClose();

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = target;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/file.txt")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///target"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->failedCount(), 1);
    QCOMPARE(task->copiedCount(), 0);
}

void TestTransferTask::aNameTheDestinationRefusesFailsOneFileAndNotTheRun()
{
    // The names that arrive off a remote drive. Every one of these is a
    // perfectly legal name where it came from, and on Windows the copy used to
    // stop part way through with an IoError carrying the path and no reason.
    auto windows = std::make_shared<MemoryFileSystem>();
    windows->setNameRules(NameRules::forPlatform(HostPlatform::Windows));
    windows->addDirectory(QStringLiteral("/arrived"));

    m_mem->addFile(QStringLiteral("/nas/really?.txt"), QByteArray("one"));
    m_mem->addFile(QStringLiteral("/nas/a:b.txt"), QByteArray("two"));
    m_mem->addFile(QStringLiteral("/nas/nul.txt"), QByteArray("three"));
    m_mem->addFile(QStringLiteral("/nas/fine.txt"), QByteArray("four"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = windows;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///nas/really?.txt")),
        VfsUri::fromString(QStringLiteral("mem:///nas/a:b.txt")),
        VfsUri::fromString(QStringLiteral("mem:///nas/nul.txt")),
        VfsUri::fromString(QStringLiteral("mem:///nas/fine.txt")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    // Three refused, one copied, and the run finished.
    QCOMPARE(task->failedCount(), 3);
    QCOMPARE(task->copiedCount(), 1);
    QVERIFY(windows->stat(VfsUri::fromString(QStringLiteral("mem:///arrived/fine.txt"))).ok());

    // The reason names the character, and offers a name that would work --
    // never applies it, because a file under a name nobody chose is harder to
    // find later than one that did not arrive.
    const QString failures = task->failures().join(QStringLiteral(" | "));
    QVERIFY2(failures.contains(QLatin1Char('?')), qPrintable(failures));
    QVERIFY2(failures.contains(QLatin1Char(':')), qPrintable(failures));
    QVERIFY2(failures.contains(QStringLiteral("nul")), qPrintable(failures));
    QVERIFY2(failures.contains(QStringLiteral("would be accepted")), qPrintable(failures));

    // Nothing arrived under a sanitised name.
    Result<FileEntryList> arrived
        = windows->list(VfsUri::fromString(QStringLiteral("mem:///arrived")), CancelToken());
    QVERIFY(arrived.ok());
    QCOMPARE(arrived.value().size(), 1);
}

void TestTransferTask::aRefusedNameInsideATreeIsOneFailureAndTheRestArrives()
{
    // The case the ticket describes: a folder copied off a NAS, with the
    // offending file somewhere inside it rather than at the top.
    auto windows = std::make_shared<MemoryFileSystem>();
    windows->setNameRules(NameRules::forPlatform(HostPlatform::Windows));
    windows->addDirectory(QStringLiteral("/arrived"));

    m_mem->addFile(QStringLiteral("/nas/photos/holiday.jpg"), QByteArray("keep"));
    m_mem->addFile(QStringLiteral("/nas/photos/really?.jpg"), QByteArray("refused"));
    m_mem->addFile(QStringLiteral("/nas/photos/more/later.jpg"), QByteArray("keep too"));

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = windows;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///nas/photos")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->failedCount(), 1);
    QVERIFY(windows->stat(VfsUri::fromString(QStringLiteral("mem:///arrived/photos/holiday.jpg"))).ok());
    QVERIFY(windows->stat(VfsUri::fromString(QStringLiteral("mem:///arrived/photos/more/later.jpg"))).ok());
    QVERIFY(!windows->stat(VfsUri::fromString(QStringLiteral("mem:///arrived/photos/really?.jpg"))).ok());
}

void TestTransferTask::readOnlyTargetFails()
{
    m_mem->addFile(QStringLiteral("/a/file.txt"), QByteArray("x"));

    // A second memory filesystem that was never given to a shared_ptr owner
    // cannot accept streamed writes, standing in for a read-only drive.
    auto readOnly = std::make_shared<MemoryFileSystem>();
    readOnly->addDirectory(QStringLiteral("/target"));
    readOnly->setFault(QStringLiteral("/target/file.txt"), VfsError::AccessDenied);

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = readOnly;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/file.txt")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///target"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->failedCount(), 1);
    QCOMPARE(task->copiedCount(), 0);
}

void TestTransferTask::aReadThatStopsHalfWayIsNotAnEndOfFile()
{
    // Regression, and the mirror image of the one above. The copy loop stopped
    // as soon as a read returned nothing and then reported success, so a source
    // that died half way left a truncated file behind and said the copy was
    // fine. Truncated and reported as good is worse than not copied at all: the
    // original then gets deleted by a move, or trusted by whoever reads it.
    m_mem->addFile(QStringLiteral("/a/big.bin"), QByteArray("head and then some more"));

    auto source = std::make_shared<FaultyFileSystem>(m_mem);
    source->readFailsAt(4);
    auto target = std::make_shared<MemoryFileSystem>();
    target->addDirectory(QStringLiteral("/target"));

    TransferTask::Request request;
    request.sourceFileSystem = source;
    request.targetFileSystem = target;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/big.bin")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///target"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->failedCount(), 1);
    QCOMPARE(task->copiedCount(), 0);
    QVERIFY(task->failures().first().contains(QStringLiteral("stopped after 4 bytes")));
}

void TestTransferTask::aFileThatLandedShortIsCaughtAfterwards()
{
    // Every byte was accepted, every close succeeded, and the file on the other
    // side is a quarter of the size. Nothing in the copy itself can know that,
    // which is the whole reason for weighing the result afterwards.
    m_mem->addFile(QStringLiteral("/a/backup.bin"), QByteArray(4000, 'x'));

    auto inner = std::make_shared<MemoryFileSystem>();
    inner->addDirectory(QStringLiteral("/target"));
    auto target = std::make_shared<FaultyFileSystem>(inner);
    target->writeKeepsEveryNth(4);

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = target;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/backup.bin")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///target"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->failedCount(), 1);
    QVERIFY2(task->failures().first().contains(QStringLiteral("4000 bytes were sent but 1000 arrived")),
        qPrintable(task->failures().first()));
}

void TestTransferTask::aMoveKeepsTheSourceWhenTheCheckFails()
{
    // The check has to happen before the move deletes anything, or a move to a
    // destination that lost half the file destroys the only good copy.
    m_mem->addFile(QStringLiteral("/a/backup.bin"), QByteArray(4000, 'x'));

    auto inner = std::make_shared<MemoryFileSystem>();
    inner->addDirectory(QStringLiteral("/target"));
    auto target = std::make_shared<FaultyFileSystem>(inner);
    target->writeKeepsEveryNth(4);

    TransferTask::Request request;
    request.mode = TransferTask::Mode::Move;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = target;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///a/backup.bin")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///target"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->failedCount(), 1);
    QVERIFY2(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///a/backup.bin"))).ok(),
        "the original was deleted after a move whose destination lost bytes");
}

void TestTransferTask::cancellationStopsMidway()
{
    for (int i = 0; i < 60; ++i)
        m_mem->addFile(QStringLiteral("/bulk/f%1.bin").arg(i), QByteArray(1024, 'x'));
    m_mem->addDirectory(QStringLiteral("/dst"));
    m_mem->setListDelayMs(40);

    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///bulk")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///dst"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->state() == Task::State::Running; }));
    task->requestCancel();

    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Cancelled);
}

void TestTransferTask::emptyRequestSucceeds()
{
    TransferTask::Request request;
    request.sourceFileSystem = m_mem;
    request.targetFileSystem = m_mem;
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->copiedCount(), 0);
}

// ------------------------------------------------------------------ links

void TestTransferTask::aLinkedDirectoryIsCopiedAsALinkAndNotAsAnEmptyFolder()
{
    // Qt answers isDir() through the link, so a link to a directory used to be
    // planned as a directory job: the far end got makeDirectory() and nothing
    // inside it, counted as transferred -- and a move then deleted the link. See
    // ADR-0092.
    QVERIFY(m_tree->makeDirs(QStringLiteral("real/inside")));
    QVERIFY(m_tree->writeFile(QStringLiteral("real/inside/leaf.txt"), QByteArray("a leaf")));
    QVERIFY(
        QFile::link(m_tree->absolute(QStringLiteral("real")), m_tree->absolute(QStringLiteral("pointer"))));

    TransferTask::Request request;
    request.sourceFileSystem = m_local;
    request.targetFileSystem = m_mem;
    request.sources = { m_tree->rootUri().child(QStringLiteral("pointer")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));
    m_mem->addDirectory(QStringLiteral("/arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    const VfsUri arrival = VfsUri::fromString(QStringLiteral("mem:///arrived/pointer"));
    const Result<FileEntry> stat = m_mem->stat(arrival);
    QVERIFY(stat.ok());
    QVERIFY2(stat.value().isSymlink, "a link must arrive as a link, not as an empty folder");
    QVERIFY(!stat.value().isDir);
    const Result<QString> points = m_mem->readLink(arrival);
    QVERIFY(points.ok());
    QCOMPARE(points.value(), m_tree->absolute(QStringLiteral("real")));
}

void TestTransferTask::aLinkedFileIsCopiedAsALinkAndNotAsACopyOfItsTarget()
{
    // The other half of the same fault: the link was opened, the target's bytes
    // were read through it, and a reference became a duplicate -- on a large
    // target, a full disk.
    QVERIFY(m_tree->writeFile(QStringLiteral("real.bin"), QByteArray("the real thing")));
    QVERIFY(QFile::link(
        m_tree->absolute(QStringLiteral("real.bin")), m_tree->absolute(QStringLiteral("pointer.bin"))));

    TransferTask::Request request;
    request.sourceFileSystem = m_local;
    request.targetFileSystem = m_mem;
    request.sources = { m_tree->rootUri().child(QStringLiteral("pointer.bin")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));
    m_mem->addDirectory(QStringLiteral("/arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    const VfsUri arrival = VfsUri::fromString(QStringLiteral("mem:///arrived/pointer.bin"));
    const Result<FileEntry> stat = m_mem->stat(arrival);
    QVERIFY(stat.ok());
    QVERIFY2(stat.value().isSymlink, "the link itself must arrive, not the bytes behind it");
    QCOMPARE(stat.value().size, 0);
    const Result<QString> points = m_mem->readLink(arrival);
    QVERIFY(points.ok());
    QCOMPARE(points.value(), m_tree->absolute(QStringLiteral("real.bin")));
}

void TestTransferTask::aRelativeLinkArrivesStillRelative()
{
    // A relative link is relative on purpose: it keeps pointing at its
    // neighbour wherever the tree is copied to. Resolving it on the way would
    // pin the copy to the machine it came from.
    QVERIFY(m_tree->makeDirs(QStringLiteral("tree")));
    QVERIFY(m_tree->writeFile(QStringLiteral("tree/real.bin"), QByteArray("the real thing")));
    QVERIFY(QFile::link(QStringLiteral("real.bin"), m_tree->absolute(QStringLiteral("tree/near.bin"))));

    TransferTask::Request request;
    request.sourceFileSystem = m_local;
    request.targetFileSystem = m_mem;
    request.sources = { m_tree->rootUri().child(QStringLiteral("tree")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));
    m_mem->addDirectory(QStringLiteral("/arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    const Result<QString> points
        = m_mem->readLink(VfsUri::fromString(QStringLiteral("mem:///arrived/tree/near.bin")));
    QVERIFY(points.ok());
    QCOMPARE(points.value(), QStringLiteral("real.bin"));
}

void TestTransferTask::aDriveThatCannotHoldALinkRefusesItByName()
{
    // Most drives have no links at all. The refusal has to name the file and say
    // why, rather than the link arriving as something else -- which is the whole
    // reason this went unnoticed for so long.
    QVERIFY(m_tree->writeFile(QStringLiteral("real.bin"), QByteArray("the real thing")));
    QVERIFY(QFile::link(
        m_tree->absolute(QStringLiteral("real.bin")), m_tree->absolute(QStringLiteral("pointer.bin"))));

    auto flat = std::make_shared<FaultyFileSystem>(m_mem);
    flat->holdsNoLinks();
    TransferTask::Request request;
    request.sourceFileSystem = m_local;
    request.targetFileSystem = flat;
    request.sources = { m_tree->rootUri().child(QStringLiteral("pointer.bin")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));
    m_mem->addDirectory(QStringLiteral("/arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->failedCount(), 1);
    QVERIFY2(task->failures().first().contains(QStringLiteral("pointer.bin")),
        qPrintable(task->failures().first()));
    QVERIFY2(task->failures().first().contains(QStringLiteral("symbolic link")),
        qPrintable(task->failures().first()));
    QVERIFY(!m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///arrived/pointer.bin"))).ok());
}

void TestTransferTask::aBrokenLinkSelectedOnItsOwnIsStillCopied()
{
    // The plan stats what it was handed, and QFileInfo::exists() resolves a
    // link: a link to nothing was "no such file" when selected on its own, while
    // the same link one level inside a copied tree went through the listing and
    // arrived. The two answers were about the same name. See ADR-0092.
    QVERIFY(QFile::link(
        m_tree->absolute(QStringLiteral("not-there.bin")), m_tree->absolute(QStringLiteral("dangling.bin"))));

    TransferTask::Request request;
    request.sourceFileSystem = m_local;
    request.targetFileSystem = m_mem;
    request.sources = { m_tree->rootUri().child(QStringLiteral("dangling.bin")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///arrived"));
    m_mem->addDirectory(QStringLiteral("/arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    const Result<QString> points
        = m_mem->readLink(VfsUri::fromString(QStringLiteral("mem:///arrived/dangling.bin")));
    QVERIFY(points.ok());
    QCOMPARE(points.value(), m_tree->absolute(QStringLiteral("not-there.bin")));
}

void TestTransferTask::deleteRemovesFilesAndTrees()
{
    m_mem->addFile(QStringLiteral("/gone.txt"));
    m_mem->addFile(QStringLiteral("/tree/deep/leaf.txt"));
    m_mem->addFile(QStringLiteral("/keep.txt"));

    auto* task = new DeleteTask(m_mem,
        { VfsUri::fromString(QStringLiteral("mem:///gone.txt")),
            VfsUri::fromString(QStringLiteral("mem:///tree")) });
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->deletedCount(), 2);
    QVERIFY(!m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///gone.txt"))).ok());
    QVERIFY(!m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///tree/deep/leaf.txt"))).ok());
    QVERIFY(m_mem->stat(VfsUri::fromString(QStringLiteral("mem:///keep.txt"))).ok());
}

void TestTransferTask::deleteReportsFailures()
{
    auto* task = new DeleteTask(m_mem, { VfsUri::fromString(QStringLiteral("mem:///nothing")) });
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->deletedCount(), 0);
    QCOMPARE(task->failures().size(), 1);
    QVERIFY(task->statusText().contains(QStringLiteral("failed")));
}

MOLE_TEST_MAIN(TestTransferTask)
#include "tst_TransferTask.moc"
