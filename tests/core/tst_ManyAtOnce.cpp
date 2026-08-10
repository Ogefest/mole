#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QTest>

using namespace mole;
using namespace mole::test;

namespace {

QByteArray payloadOf(int size, char fill)
{
    return QByteArray(size, fill);
}

} // namespace

/// Several jobs at once over the same files.
///
/// Each one on its own is covered elsewhere. What is here is what happens when
/// two of them want the same thing at the same time -- and the answer that must
/// never appear is a file that is half of one and half of the other, or a copy
/// reported as finished after the thing it was copying went away.
class TestManyAtOnce : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void twoTasksWritingOneTargetLeaveAWholeFile();
    void aCopyOfAFileAnotherTaskIsDeletingIsWholeOrItFails();
    void aMoveAndACopyOfOneSourceDoNotProduceHalfAFile();
    void halfOfFiftyTasksCancelledLeavesTheRestCorrect();

private:
    TransferTask* start(const TransferTask::Request& request);

    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::unique_ptr<TempTree> m_tree;
};

void TestManyAtOnce::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_disk = std::make_shared<LocalFileSystem>();
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
}

void TestManyAtOnce::cleanup()
{
    m_tasks.reset();
    m_memory.reset();
    m_disk.reset();
    m_tree.reset();
}

TransferTask* TestManyAtOnce::start(const TransferTask::Request& request)
{
    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    return task;
}

void TestManyAtOnce::twoTasksWritingOneTargetLeaveAWholeFile()
{
    // Two people drop two different files on the same folder, both called
    // report.bin. One of them wins -- either is defensible -- and what must not
    // be there afterwards is a file made of both.
    const QByteArray first = payloadOf(128 * 1024, 'a');
    const QByteArray second = payloadOf(128 * 1024, 'b');
    m_memory->addFile(QStringLiteral("/source/one/report.bin"), first);
    m_memory->addFile(QStringLiteral("/source/two/report.bin"), second);
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));

    QList<TransferTask*> tasks;
    for (const QString& which : { QStringLiteral("one"), QStringLiteral("two") }) {
        TransferTask::Request request;
        request.sourceFileSystem = m_memory;
        request.targetFileSystem = m_disk;
        request.sources = { VfsUri::fromString(QStringLiteral("mem:///source/%1/report.bin").arg(which)) };
        request.targetDirectory = m_tree->rootUri().child(QStringLiteral("arrived"));
        request.onConflict = TransferTask::Conflict::Overwrite;
        tasks.append(start(request));
    }

    for (TransferTask* task : tasks)
        QVERIFY(waitForTask(task, 30000));

    QFile arrived(m_tree->absolute(QStringLiteral("arrived/report.bin")));
    QVERIFY(arrived.open(QIODevice::ReadOnly));
    const QByteArray landed = arrived.readAll();
    QVERIFY2(landed == first || landed == second,
        qPrintable(QStringLiteral("the file is neither of the two: %1 bytes").arg(landed.size())));

    // And nothing is left under a working name, whichever order they finished in.
    const QStringList leftovers
        = QDir(m_tree->absolute(QStringLiteral("arrived"))).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    QCOMPARE(leftovers, QStringList { QStringLiteral("report.bin") });
}

void TestManyAtOnce::aCopyOfAFileAnotherTaskIsDeletingIsWholeOrItFails()
{
    // A copy in flight and a delete of its source. Either order is an answer:
    // the copy finishes from the handle it already has, or it fails because the
    // bytes stopped. A short file reported as a finished copy is not.
    const QByteArray payload = payloadOf(64 * 1024, 'c');
    m_memory->addFile(QStringLiteral("/source/report.bin"), payload);
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));

    auto source = std::make_shared<FaultyFileSystem>(m_memory);
    source->readStallsAt(16 * 1024);

    TransferTask::Request request;
    request.sourceFileSystem = source;
    request.targetFileSystem = m_disk;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///source/report.bin")) };
    request.targetDirectory = m_tree->rootUri().child(QStringLiteral("arrived"));
    TransferTask* copy = start(request);

    QVERIFY(waitFor([&] { return source->isStalled(); }));
    auto* removal
        = new DeleteTask(m_memory, { VfsUri::fromString(QStringLiteral("mem:///source/report.bin")) });
    m_tasks->submit(removal);
    QVERIFY(waitForTask(removal, 30000));
    source->release();
    QVERIFY(waitForTask(copy, 30000));

    QFile arrived(m_tree->absolute(QStringLiteral("arrived/report.bin")));
    if (arrived.exists()) {
        QVERIFY(arrived.open(QIODevice::ReadOnly));
        QCOMPARE(arrived.readAll(), payload);
        QVERIFY2(copy->failures().isEmpty(), "a file that is all there was not a failure");
    } else {
        QVERIFY2(!copy->failures().isEmpty() || copy->state() == Task::State::Cancelled,
            "a copy that produced nothing has to say why");
    }
}

void TestManyAtOnce::aMoveAndACopyOfOneSourceDoNotProduceHalfAFile()
{
    // A move and a copy of the same file, at the same time. The move's delete
    // runs only after its own copy is verified -- but the other task is reading
    // the same source, and what it produces must be all of the file or none.
    const QByteArray payload = payloadOf(96 * 1024, 'd');
    m_memory->addFile(QStringLiteral("/source/report.bin"), payload);
    QVERIFY(m_tree->makeDirs(QStringLiteral("moved")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("copied")));

    const auto requestFor = [&](const QString& into, TransferTask::Mode mode) {
        TransferTask::Request request;
        request.mode = mode;
        request.sourceFileSystem = m_memory;
        request.targetFileSystem = m_disk;
        request.sources = { VfsUri::fromString(QStringLiteral("mem:///source/report.bin")) };
        request.targetDirectory = m_tree->rootUri().child(into);
        return request;
    };

    TransferTask* mover = start(requestFor(QStringLiteral("moved"), TransferTask::Mode::Move));
    TransferTask* copier = start(requestFor(QStringLiteral("copied"), TransferTask::Mode::Copy));
    QVERIFY(waitForTask(mover, 30000));
    QVERIFY(waitForTask(copier, 30000));

    // Whatever landed, landed whole. Nothing may be a prefix of the payload.
    for (const QString& where : { QStringLiteral("moved/report.bin"), QStringLiteral("copied/report.bin") }) {
        QFile file(m_tree->absolute(where));
        if (!file.exists())
            continue;
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), payload);
    }
    // The move is the one that deletes, and it only does so having verified its
    // own copy -- so a file exists somewhere no matter which order they ran in.
    QVERIFY2(QFile::exists(m_tree->absolute(QStringLiteral("moved/report.bin")))
            || m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///source/report.bin"))).ok(),
        "the move either finished or left the source alone");
}

void TestManyAtOnce::halfOfFiftyTasksCancelledLeavesTheRestCorrect()
{
    // Fifty jobs queued and half of them stopped. The ones that were cancelled
    // leave nothing behind; the ones that were not are unaffected by the noise.
    constexpr int kTasks = 50;
    const QByteArray payload = payloadOf(4096, 'e');
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));
    for (int i = 0; i < kTasks; ++i)
        m_memory->addFile(QStringLiteral("/source/file%1.bin").arg(i), payload);

    QList<TransferTask*> tasks;
    for (int i = 0; i < kTasks; ++i) {
        TransferTask::Request request;
        request.sourceFileSystem = m_memory;
        request.targetFileSystem = m_disk;
        request.sources = { VfsUri::fromString(QStringLiteral("mem:///source/file%1.bin").arg(i)) };
        request.targetDirectory = m_tree->rootUri().child(QStringLiteral("arrived"));
        tasks.append(start(request));
    }
    for (int i = 0; i < kTasks; i += 2)
        tasks.at(i)->requestCancel();

    for (TransferTask* task : tasks)
        QVERIFY(waitForTask(task, 60000));

    // Every task that was left alone did its job.
    for (int i = 1; i < kTasks; i += 2) {
        QVERIFY2(tasks.at(i)->failures().isEmpty(),
            qPrintable(tasks.at(i)->failures().join(QStringLiteral("; "))));
        QFile arrived(m_tree->absolute(QStringLiteral("arrived/file%1.bin").arg(i)));
        QVERIFY(arrived.open(QIODevice::ReadOnly));
        QCOMPARE(arrived.readAll(), payload);
    }

    // And every cancelled one either never started or left nothing half-written:
    // a cancel that lands after the last byte is a finished copy, not a fault.
    for (int i = 0; i < kTasks; i += 2) {
        QFile arrived(m_tree->absolute(QStringLiteral("arrived/file%1.bin").arg(i)));
        if (!arrived.exists())
            continue;
        QVERIFY(arrived.open(QIODevice::ReadOnly));
        QCOMPARE(arrived.readAll(), payload);
    }

    // Nothing under a working name, from any of the fifty.
    const QStringList entries
        = QDir(m_tree->absolute(QStringLiteral("arrived"))).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    for (const QString& name : entries)
        QVERIFY2(!name.contains(QStringLiteral("mole-partial")), qPrintable(name));
}

MOLE_TEST_MAIN(TestManyAtOnce)

#include "tst_ManyAtOnce.moc"
