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

QByteArray payloadOf(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 13 + (i >> 9)) & 0xff);
    return data;
}

/// A drive that cannot rename, which is what a move across two devices looks
/// like from above: the fast path is refused and the slow one has to take over.
class DriveThatCannotRename final : public IFileSystem
{
public:
    explicit DriveThatCannotRename(FileSystemPtr inner)
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
    Result<void> rename(const VfsUri&, const VfsUri&) override
    {
        ++m_renamesRefused;
        return Result<void>::failure(
            VfsError::NotSupported, QStringLiteral("that would cross a device boundary"));
    }
    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override
    {
        return m_inner->openRead(target, expectedSize);
    }
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override
    {
        return m_inner->openWrite(target, expectedSize);
    }

    int renamesRefused() const { return m_renamesRefused; }

private:
    FileSystemPtr m_inner;
    int m_renamesRefused = 0;
};

} // namespace

/// A move is the one operation that deletes something, and everything here is
/// about the source surviving whatever went wrong.
///
/// A copy that fails costs the time it took. A move that fails in the wrong way
/// costs the file, and there is nowhere to get it back from -- which is why the
/// question asked of every scenario below is the same one: is the original still
/// there, and does the failure say so.
class TestMoveIsPermanent : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aMoveInterruptedAtAnyStageKeepsTheSource_data();
    void aMoveInterruptedAtAnyStageKeepsTheSource();
    void aDeleteThatFailsAfterAGoodCopyLeavesBothAndSaysSo();

    void aMoveOntoSomethingThatExists_data();
    void aMoveOntoSomethingThatExists();
    void aRenameWithinOneBackendMovesNothingAcrossTheWire();
    void aBackendThatCannotRenameFallsBackToCopyingAndDeleting();

    void aDirectoryMovedIntoItsOwnSubdirectoryIsRefused();
    void aDirectoryCopiedIntoItselfIsRefused();
    void aDirectoryMovedOntoItselfIsRefused();
    void aDestinationThatDiffersOnlyInCaseIsStillInsideTheSource();
    void onACaseSensitiveVolumeTheSamePairIsTwoPlaces();

    void movingASymlinkDoesNotFollowIt();

private:
    TransferTask* run(const TransferTask::Request& request);
    TransferTask::Request moveOf(
        const FileSystemPtr& from, const VfsUri& source, const FileSystemPtr& to, const VfsUri& into) const;

    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::unique_ptr<TempTree> m_tree;
};

void TestMoveIsPermanent::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_disk = std::make_shared<LocalFileSystem>();
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
}

void TestMoveIsPermanent::cleanup()
{
    m_tasks.reset();
    m_memory.reset();
    m_disk.reset();
    m_tree.reset();
}

TransferTask* TestMoveIsPermanent::run(const TransferTask::Request& request)
{
    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    return waitForTask(task, 30000) ? task : nullptr;
}

TransferTask::Request TestMoveIsPermanent::moveOf(
    const FileSystemPtr& from, const VfsUri& source, const FileSystemPtr& to, const VfsUri& into) const
{
    TransferTask::Request request;
    request.mode = TransferTask::Mode::Move;
    request.sourceFileSystem = from;
    request.targetFileSystem = to;
    request.sources = { source };
    request.targetDirectory = into;
    return request;
}

void TestMoveIsPermanent::aMoveInterruptedAtAnyStageKeepsTheSource_data()
{
    QTest::addColumn<QString>("stage");

    // One row per stage a cross-backend move passes through. The answer is the
    // same at every one of them, which is the point: there is no window in
    // which the source has been deleted and the destination is not yet good.
    QTest::newRow("the source cannot be opened") << QStringLiteral("open");
    QTest::newRow("the read dies half way") << QStringLiteral("read");
    QTest::newRow("the write is refused half way") << QStringLiteral("write");
    QTest::newRow("the destination fails when it is closed") << QStringLiteral("close");
    QTest::newRow("the file that landed is the wrong size") << QStringLiteral("arrival");
    QTest::newRow("the source cannot be deleted afterwards") << QStringLiteral("delete");
}

void TestMoveIsPermanent::aMoveInterruptedAtAnyStageKeepsTheSource()
{
    QFETCH(QString, stage);

    const QByteArray payload = payloadOf(64 * 1024);
    m_memory->addFile(QStringLiteral("/source/payload.bin"), payload);
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));

    auto source = std::make_shared<FaultyFileSystem>(m_memory);
    auto target = std::make_shared<FaultyFileSystem>(m_disk);

    if (stage == QLatin1String("open"))
        source->readFailsAt(0, {}, VfsError::AccessDenied, QStringLiteral("no"));
    else if (stage == QLatin1String("read"))
        source->readFailsAt(20 * 1024);
    else if (stage == QLatin1String("write"))
        target->writeFailsAt(20 * 1024);
    else if (stage == QLatin1String("close"))
        target->writeFailsOnClose();
    else if (stage == QLatin1String("arrival"))
        target->writeKeepsEveryNth(2);
    else if (stage == QLatin1String("delete"))
        source->removeFails();

    TransferTask* task = run(moveOf(source, VfsUri::fromString(QStringLiteral("mem:///source/payload.bin")),
        target, m_tree->rootUri().child(QStringLiteral("arrived"))));
    QVERIFY(task != nullptr);
    QVERIFY2(!task->failures().isEmpty(), "a move that went wrong has to say so");

    // The one assertion that matters, at every stage: the original is still
    // there and still whole. Read through the drive underneath, so the wrapper
    // that broke the copy cannot also be the witness.
    Result<std::unique_ptr<QIODevice>> original
        = m_memory->openRead(VfsUri::fromString(QStringLiteral("mem:///source/payload.bin")));
    QVERIFY2(original.ok(), qPrintable(original.error().message));
    QCOMPARE(original.value()->readAll(), payload);
}

void TestMoveIsPermanent::aDeleteThatFailsAfterAGoodCopyLeavesBothAndSaysSo()
{
    // The safe direction, and it has to be chosen deliberately. Every byte
    // arrived, and the source cannot be removed -- so there are two copies now.
    // Two copies and a reported failure is an inconvenience; one copy and a
    // reported success, when the one is the half-written one, is a lost file.
    const QByteArray payload = payloadOf(32 * 1024);
    m_memory->addFile(QStringLiteral("/source/payload.bin"), payload);
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));

    auto source = std::make_shared<FaultyFileSystem>(m_memory);
    source->removeFails({}, VfsError::AccessDenied, QStringLiteral("the folder is read-only"));

    TransferTask* task = run(moveOf(source, VfsUri::fromString(QStringLiteral("mem:///source/payload.bin")),
        m_disk, m_tree->rootUri().child(QStringLiteral("arrived"))));
    QVERIFY(task != nullptr);
    QCOMPARE(task->failedCount(), 1);
    QVERIFY2(
        task->failures().first().contains(QStringLiteral("read-only")), qPrintable(task->failures().first()));

    QFile arrived(m_tree->absolute(QStringLiteral("arrived/payload.bin")));
    QVERIFY(arrived.open(QIODevice::ReadOnly));
    QCOMPARE(arrived.readAll(), payload);
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///source/payload.bin"))).ok());
}

void TestMoveIsPermanent::aMoveOntoSomethingThatExists_data()
{
    QTest::addColumn<int>("policy");
    QTest::addColumn<bool>("sameBackend");

    for (bool same : { true, false }) {
        const QString where = same ? QStringLiteral("renamed") : QStringLiteral("copied");
        QTest::newRow(qPrintable(QStringLiteral("fail, %1").arg(where)))
            << int(TransferTask::Conflict::Fail) << same;
        QTest::newRow(qPrintable(QStringLiteral("skip, %1").arg(where)))
            << int(TransferTask::Conflict::Skip) << same;
        QTest::newRow(qPrintable(QStringLiteral("overwrite, %1").arg(where)))
            << int(TransferTask::Conflict::Overwrite) << same;
    }
}

void TestMoveIsPermanent::aMoveOntoSomethingThatExists()
{
    QFETCH(int, policy);
    QFETCH(bool, sameBackend);

    // The rename path and the copy path answer the same three questions, and
    // they have to answer them the same way -- a move that overwrites within one
    // drive and skips across two would be a coin toss dressed up as a setting.
    const QByteArray moving("the one being moved");
    const QByteArray standing("the one already there");
    m_memory->addFile(QStringLiteral("/source/report.txt"), moving);
    m_memory->addFile(QStringLiteral("/arrived/report.txt"), standing);

    FileSystemPtr target = sameBackend ? FileSystemPtr(m_memory) : FileSystemPtr(m_disk);
    VfsUri into = VfsUri::fromString(QStringLiteral("mem:///arrived"));
    if (!sameBackend) {
        QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));
        QVERIFY(m_tree->writeFile(QStringLiteral("arrived/report.txt"), standing));
        into = m_tree->rootUri().child(QStringLiteral("arrived"));
    }

    TransferTask::Request request
        = moveOf(m_memory, VfsUri::fromString(QStringLiteral("mem:///source/report.txt")), target, into);
    request.onConflict = static_cast<TransferTask::Conflict>(policy);
    TransferTask* task = run(request);
    QVERIFY(task != nullptr);

    const auto contentsOfTarget = [&]() -> QByteArray {
        if (sameBackend) {
            Result<std::unique_ptr<QIODevice>> reader
                = m_memory->openRead(VfsUri::fromString(QStringLiteral("mem:///arrived/report.txt")));
            return reader.ok() ? reader.value()->readAll() : QByteArray();
        }
        QFile file(m_tree->absolute(QStringLiteral("arrived/report.txt")));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const bool sourceSurvives
        = m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///source/report.txt"))).ok();

    switch (static_cast<TransferTask::Conflict>(policy)) {
    case TransferTask::Conflict::Fail:
        QCOMPARE(task->failedCount(), 1);
        QCOMPARE(contentsOfTarget(), standing);
        QVERIFY2(sourceSurvives, "a move that was refused must not have deleted anything");
        break;
    case TransferTask::Conflict::Skip:
        QCOMPARE(task->skippedCount(), 1);
        QCOMPARE(contentsOfTarget(), standing);
        // Skipped means "leave the existing file alone", and the one that was
        // not moved is still where it was. Deleting it would be a move to
        // nowhere.
        QVERIFY2(sourceSurvives, "a skipped move must not delete the file it did not move");
        break;
    case TransferTask::Conflict::Overwrite:
        QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
        QCOMPARE(contentsOfTarget(), moving);
        QVERIFY2(!sourceSurvives, "a move that overwrote has finished, so the source is gone");
        break;
    }
}

void TestMoveIsPermanent::aRenameWithinOneBackendMovesNothingAcrossTheWire()
{
    // The fast path: one operation on the server, no bytes read and none
    // written, and nothing to verify afterwards because nothing was copied.
    // What has to hold is that the content is the same file it was.
    const QByteArray payload = payloadOf(128 * 1024);
    m_memory->addFile(QStringLiteral("/source/big.bin"), payload);
    m_memory->addDirectory(QStringLiteral("/arrived"));

    auto watched = std::make_shared<FaultyFileSystem>(m_memory);
    // Any read at all would be the slow path pretending to be the fast one.
    watched->readFailsAt(0, {}, VfsError::IoError, QStringLiteral("a rename must not read the file"));

    TransferTask* task = run(moveOf(watched, VfsUri::fromString(QStringLiteral("mem:///source/big.bin")),
        watched, VfsUri::fromString(QStringLiteral("mem:///arrived"))));
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));

    QVERIFY(!m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///source/big.bin"))).ok());
    Result<std::unique_ptr<QIODevice>> moved
        = m_memory->openRead(VfsUri::fromString(QStringLiteral("mem:///arrived/big.bin")));
    QVERIFY2(moved.ok(), qPrintable(moved.error().message));
    QCOMPARE(moved.value()->readAll(), payload);
}

void TestMoveIsPermanent::aBackendThatCannotRenameFallsBackToCopyingAndDeleting()
{
    // What a move across two devices on one disk looks like from up here: the
    // rename is refused because it would cross a boundary, and the copy path has
    // to take over rather than the move failing.
    const QByteArray payload = payloadOf(16 * 1024);
    m_memory->addFile(QStringLiteral("/source/report.txt"), payload);
    m_memory->addDirectory(QStringLiteral("/arrived"));

    auto drive = std::make_shared<DriveThatCannotRename>(m_memory);
    TransferTask* task = run(moveOf(drive, VfsUri::fromString(QStringLiteral("mem:///source/report.txt")),
        drive, VfsUri::fromString(QStringLiteral("mem:///arrived"))));
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(drive->renamesRefused(), 1);
    QCOMPARE(task->copiedCount(), 1);

    QVERIFY(!m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///source/report.txt"))).ok());
    Result<std::unique_ptr<QIODevice>> moved
        = m_memory->openRead(VfsUri::fromString(QStringLiteral("mem:///arrived/report.txt")));
    QVERIFY2(moved.ok(), qPrintable(moved.error().message));
    QCOMPARE(moved.value()->readAll(), payload);
}

void TestMoveIsPermanent::aDirectoryMovedIntoItsOwnSubdirectoryIsRefused()
{
    // The one that eats a directory. The plan is built by walking the source, so
    // the copy is finite -- and then the move deletes the source, which now
    // contains the only copy of everything that was in it.
    //
    // Two drives over the same tree is not a contrivance: a bookmarked folder
    // and the disk it lives on are two mounts, and nothing above here knows they
    // are the same place.
    m_memory->addFile(QStringLiteral("/work/notes.txt"), QByteArray("keep me"));
    m_memory->addFile(QStringLiteral("/work/inner/deep.txt"), QByteArray("me too"));

    auto second = std::make_shared<FaultyFileSystem>(m_memory);
    TransferTask* task = run(moveOf(m_memory, VfsUri::fromString(QStringLiteral("mem:///work")), second,
        VfsUri::fromString(QStringLiteral("mem:///work/inner"))));
    QVERIFY(task != nullptr);
    QCOMPARE(task->failedCount(), 1);
    QVERIFY2(
        task->failures().first().contains(QStringLiteral("inside")), qPrintable(task->failures().first()));

    // Everything is exactly where it was.
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///work/notes.txt"))).ok());
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///work/inner/deep.txt"))).ok());
}

void TestMoveIsPermanent::aDestinationThatDiffersOnlyInCaseIsStillInsideTheSource()
{
    // The same directory-eating move as above, reached by typing the
    // destination with a different capitalisation. On an NTFS volume, and on a
    // default APFS one, /work and /WORK are one directory -- so comparing the
    // two spellings exactly let the destination walk straight past the guard,
    // and the move then deleted the source with the only copy of everything
    // underneath it.
    //
    // The volume is what knows, so the guard asks the destination backend
    // rather than the uri's scheme. Here that is a MemoryFileSystem told to
    // answer the way such a volume does, which is why this needs no real one.
    m_memory->setCaseSensitivity(Qt::CaseInsensitive);
    m_memory->addFile(QStringLiteral("/work/notes.txt"), QByteArray("keep me"));
    m_memory->addFile(QStringLiteral("/work/inner/deep.txt"), QByteArray("me too"));

    // Two mounts over the same tree, as in the test above: one drive would take
    // the rename shortcut and never reach the plan the guard lives in.
    auto second = std::make_shared<FaultyFileSystem>(m_memory);
    TransferTask* task = run(moveOf(m_memory, VfsUri::fromString(QStringLiteral("mem:///work")), second,
        VfsUri::fromString(QStringLiteral("mem:///WORK/inner"))));
    QVERIFY(task != nullptr);
    QCOMPARE(task->failedCount(), 1);
    QVERIFY2(
        task->failures().first().contains(QStringLiteral("inside")), qPrintable(task->failures().first()));

    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///work/notes.txt"))).ok());
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///work/inner/deep.txt"))).ok());
}

void TestMoveIsPermanent::onACaseSensitiveVolumeTheSamePairIsTwoPlaces()
{
    // The other half, and it matters as much: /work and /WORK really are two
    // directories on ext4 and in a bucket, so the guard must not start refusing
    // a move it has always allowed.
    m_memory->addFile(QStringLiteral("/work/notes.txt"), QByteArray("keep me"));

    auto second = std::make_shared<FaultyFileSystem>(m_memory);
    TransferTask* task = run(moveOf(m_memory, VfsUri::fromString(QStringLiteral("mem:///work")), second,
        VfsUri::fromString(QStringLiteral("mem:///WORK/inner"))));
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///WORK/inner/work/notes.txt"))).ok());
}

void TestMoveIsPermanent::aDirectoryCopiedIntoItselfIsRefused()
{
    // The same shape without the delete: nothing is lost, but a directory that
    // now contains a copy of itself is nobody's intention, and the next copy
    // would contain that one.
    m_memory->addFile(QStringLiteral("/work/notes.txt"), QByteArray("keep me"));
    m_memory->addDirectory(QStringLiteral("/work/inner"));

    auto second = std::make_shared<FaultyFileSystem>(m_memory);
    TransferTask::Request request;
    request.sourceFileSystem = m_memory;
    request.targetFileSystem = second;
    request.sources = { VfsUri::fromString(QStringLiteral("mem:///work")) };
    request.targetDirectory = VfsUri::fromString(QStringLiteral("mem:///work/inner"));

    TransferTask* task = run(request);
    QVERIFY(task != nullptr);
    QCOMPARE(task->failedCount(), 1);
    QVERIFY(!m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///work/inner/work"))).ok());
}

void TestMoveIsPermanent::aDirectoryMovedOntoItselfIsRefused()
{
    // Moving a folder into the folder it is already in. Harmless when it is
    // refused, and a delete of the original when it is not.
    m_memory->addFile(QStringLiteral("/work/inner/notes.txt"), QByteArray("keep me"));

    auto second = std::make_shared<FaultyFileSystem>(m_memory);
    TransferTask* task = run(moveOf(m_memory, VfsUri::fromString(QStringLiteral("mem:///work/inner")), second,
        VfsUri::fromString(QStringLiteral("mem:///work/inner"))));
    QVERIFY(task != nullptr);
    QCOMPARE(task->failedCount(), 1);
    QVERIFY(m_memory->stat(VfsUri::fromString(QStringLiteral("mem:///work/inner/notes.txt"))).ok());
}

void TestMoveIsPermanent::movingASymlinkDoesNotFollowIt()
{
    // A link is a name pointing somewhere, and moving the name must not touch
    // what it points at. The thing to rule out is a move that copies the target
    // through the link and then deletes the link -- which quietly turns a
    // reference into a duplicate, and on a large target into a full disk.
    QVERIFY(m_tree->makeDirs(QStringLiteral("source")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));
    QVERIFY(m_tree->writeFile(QStringLiteral("source/real.bin"), QByteArray("the real thing")));
    QVERIFY(QFile::link(m_tree->absolute(QStringLiteral("source/real.bin")),
        m_tree->absolute(QStringLiteral("source/link.bin"))));

    TransferTask* task = run(
        moveOf(m_disk, m_tree->rootUri().child(QStringLiteral("source")).child(QStringLiteral("link.bin")),
            m_disk, m_tree->rootUri().child(QStringLiteral("arrived"))));
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));

    // Whatever the link became on the way, the file it pointed at is untouched
    // and the name is no longer where it was.
    QVERIFY(QFile::exists(m_tree->absolute(QStringLiteral("source/real.bin"))));
    QVERIFY(!QFileInfo(m_tree->absolute(QStringLiteral("source/link.bin"))).isSymLink());
    QVERIFY(QFile::exists(m_tree->absolute(QStringLiteral("arrived/link.bin"))));

    // A rename within one drive moves the link itself, which is what the fast
    // path does and what a link should do.
    QVERIFY2(QFileInfo(m_tree->absolute(QStringLiteral("arrived/link.bin"))).isSymLink(),
        "a move within one drive is a rename, and a renamed link is still a link");
}

MOLE_TEST_MAIN(TestMoveIsPermanent)

#include "tst_MoveIsPermanent.moc"
