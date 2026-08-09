#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>

using namespace mole;
using namespace mole::test;

namespace {

/// A write stream that accepts everything and only fails when it is closed.
///
/// That is what every remote backend looks like: it stages the payload locally and
/// sends it in close(), because a signature needs the length up front. The failure
/// therefore arrives after the last successful write() -- and QIODevice::close()
/// returns void, so it can only be found by asking.
class FailsOnCloseDevice
    : public QIODevice
    , public ICommitsOnClose
{
public:
    bool open(OpenMode mode) override { return QIODevice::open(mode | QIODevice::Unbuffered); }
    VfsError commitError() const override
    {
        return VfsError::make(VfsError::NetworkError, QStringLiteral("the server hung up"));
    }

protected:
    qint64 readData(char*, qint64) override { return -1; }
    // Every write succeeds, which is the point: nothing before close() knows.
    qint64 writeData(const char*, qint64 size) override { return size; }
};

/// A backend that behaves like the memory one in every way except that its write
/// streams fail when closed. By delegation rather than inheritance, because
/// MemoryFileSystem is final.
class CommitFailingFileSystem final : public IFileSystem
{
public:
    explicit CommitFailingFileSystem(std::shared_ptr<MemoryFileSystem> inner)
        : m_inner(std::move(inner))
    {
    }

    QString scheme() const override { return m_inner->scheme(); }
    VfsCapabilities capabilities() const override { return m_inner->capabilities(); }
    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override
    {
        return m_inner->list(dir, cancel);
    }
    Result<FileEntry> stat(const VfsUri& target) override { return m_inner->stat(target); }
    Result<void> makeDirectory(const VfsUri& target) override { return m_inner->makeDirectory(target); }
    Result<void> remove(const VfsUri& target, bool recursive) override
    {
        return m_inner->remove(target, recursive);
    }
    Result<void> rename(const VfsUri& from, const VfsUri& to) override
    {
        return m_inner->rename(from, to);
    }
    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target) override
    {
        return m_inner->openRead(target);
    }

    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri&) override
    {
        auto device = std::make_unique<FailsOnCloseDevice>();
        device->open(QIODevice::WriteOnly);
        return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(device.release()));
    }

private:
    std::shared_ptr<MemoryFileSystem> m_inner;
};

} // namespace

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
    void readOnlyTargetFails();
    void aWriteThatFailsOnlyWhenClosedIsStillAFailure();
    void cancellationStopsMidway();
    void emptyRequestSucceeds();

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
    auto target = std::make_shared<CommitFailingFileSystem>(inner);

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
