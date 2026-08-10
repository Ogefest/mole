#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTest>

using namespace mole;
using namespace mole::test;

namespace {

/// Content whose every byte depends on where it sits.
///
/// A file of repeated zeroes proves only that the right *number* of bytes
/// arrived. This proves they arrived in the right order, that no span was
/// delivered twice, and that there is no hole -- the three ways a copy goes
/// wrong while still weighing the right amount.
QByteArray payloadOf(qint64 size)
{
    QByteArray data(static_cast<int>(size), Qt::Uninitialized);
    for (qint64 i = 0; i < size; ++i)
        data[static_cast<int>(i)] = static_cast<char>((i * 31 + (i >> 11) * 7) & 0xff);
    return data;
}

QByteArray digestOf(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

} // namespace

/// A file arrives whole, or the copy says it did not.
///
/// The sizes are not arbitrary. Each one is a boundary in the code that moves
/// the bytes -- the copy loop's chunk, and the degenerate ends -- and a file one
/// byte either side of a boundary takes a different path through the final read.
/// That is the classic way to lose a byte and report a success. The boundaries
/// that belong to a *stream* rather than to the copy loop are held one layer
/// down, in tst_StreamingDownload, where there is a stream to hold them against.
///
/// What the destination holds is read back through a channel that is not the
/// backend that wrote it wherever there is one: a local file is opened with
/// QFile rather than through LocalFileSystem, so a reading bug cannot cancel out
/// a writing bug. The memory drive has no second channel, being nothing but its
/// own storage, which is why every pair that can land on disk does.
class TestFilesArriveWhole : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void everyPairCarriesEveryBoundarySize_data();
    void everyPairCarriesEveryBoundarySize();

    void aFileTheExactLengthOfSeveralChunksEndsRatherThanFailing();
    void anEmptyFileIsCreatedRatherThanSkipped();
    void aSparseFileArrivesWithTheSameContent();
    void aTreeAHundredLevelsDeepArrivesWhole();
    void tenThousandSmallFilesAllArrive();

private:
    /// Copies one source into `targetDirectory` and waits for the end.
    TransferTask* copy(const FileSystemPtr& from, const VfsUri& source, const FileSystemPtr& to,
        const VfsUri& targetDirectory);

    /// What is on local disk, read with QFile -- out of band, on purpose.
    QByteArray onDisk(const QString& relative) const;

    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::unique_ptr<TempTree> m_tree;
};

void TestFilesArriveWhole::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_disk = std::make_shared<LocalFileSystem>();
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
}

void TestFilesArriveWhole::cleanup()
{
    m_tasks.reset();
    m_memory.reset();
    m_disk.reset();
    m_tree.reset();
}

TransferTask* TestFilesArriveWhole::copy(
    const FileSystemPtr& from, const VfsUri& source, const FileSystemPtr& to, const VfsUri& targetDirectory)
{
    TransferTask::Request request;
    request.sourceFileSystem = from;
    request.targetFileSystem = to;
    request.sources = { source };
    request.targetDirectory = targetDirectory;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    return waitForTask(task, 120000) ? task : nullptr;
}

QByteArray TestFilesArriveWhole::onDisk(const QString& relative) const
{
    QFile file(m_tree->absolute(relative));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void TestFilesArriveWhole::everyPairCarriesEveryBoundarySize_data()
{
    QTest::addColumn<QString>("pair");
    QTest::addColumn<qint64>("size");

    // Every pair of the two backends that need no server. The remote ones are
    // the same matrix against the testbed -- see tst_SftpFileSystem and the
    // conformance suite, which is where a pair that needs an account belongs.
    const QStringList pairs { QStringLiteral("disk->disk"), QStringLiteral("disk->memory"),
        QStringLiteral("memory->disk"), QStringLiteral("memory->memory") };

    const qint64 chunk = TransferTask::kCopyChunkBytes;
    const QList<QPair<QString, qint64>> sizes {
        { QStringLiteral("empty"), 0 },
        { QStringLiteral("one byte"), 1 },
        { QStringLiteral("a chunk less one"), chunk - 1 },
        { QStringLiteral("exactly a chunk"), chunk },
        { QStringLiteral("a chunk and one"), chunk + 1 },
        { QStringLiteral("two chunks"), 2 * chunk },
    };

    for (const QString& pair : pairs) {
        for (const auto& size : sizes)
            QTest::newRow(qPrintable(QStringLiteral("%1, %2").arg(pair, size.first))) << pair << size.second;
    }
}

void TestFilesArriveWhole::everyPairCarriesEveryBoundarySize()
{
    QFETCH(QString, pair);
    QFETCH(qint64, size);

    const bool fromDisk = pair.startsWith(QStringLiteral("disk"));
    const bool toDisk = pair.endsWith(QStringLiteral("disk"));
    const QByteArray payload = payloadOf(size);

    FileSystemPtr from;
    VfsUri source;
    if (fromDisk) {
        QVERIFY(m_tree->makeDirs(QStringLiteral("source")));
        QVERIFY(m_tree->writeFile(QStringLiteral("source/payload.bin"), payload));
        from = m_disk;
        source = m_tree->rootUri().child(QStringLiteral("source")).child(QStringLiteral("payload.bin"));
    } else {
        m_memory->addFile(QStringLiteral("/source/payload.bin"), payload);
        from = m_memory;
        source = VfsUri::fromString(QStringLiteral("mem:///source/payload.bin"));
    }

    FileSystemPtr to;
    VfsUri target;
    if (toDisk) {
        QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));
        to = m_disk;
        target = m_tree->rootUri().child(QStringLiteral("arrived"));
    } else {
        m_memory->addDirectory(QStringLiteral("/arrived"));
        to = m_memory;
        target = VfsUri::fromString(QStringLiteral("mem:///arrived"));
    }

    TransferTask* task = copy(from, source, to, target);
    QVERIFY(task != nullptr);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->copiedCount(), 1);

    QByteArray arrived;
    if (toDisk) {
        arrived = onDisk(QStringLiteral("arrived/payload.bin"));
    } else {
        Result<std::unique_ptr<QIODevice>> reader
            = m_memory->openRead(VfsUri::fromString(QStringLiteral("mem:///arrived/payload.bin")));
        QVERIFY2(reader.ok(), qPrintable(reader.error().message));
        arrived = reader.value()->readAll();
    }

    QCOMPARE(arrived.size(), payload.size());
    // Hashed rather than compared byte by byte, so a failure prints a line
    // rather than eight megabytes of hex.
    QCOMPARE(digestOf(arrived).toHex(), digestOf(payload).toHex());
}

void TestFilesArriveWhole::aFileTheExactLengthOfSeveralChunksEndsRatherThanFailing()
{
    // The case that distinguishes "the file ended" from "the read failed". When
    // the length divides exactly, the last useful read fills the buffer and the
    // *next* one returns zero -- and a copy loop that cannot tell that zero from
    // a dropped connection either loses the last chunk or fails a good copy.
    const QByteArray payload = payloadOf(4 * TransferTask::kCopyChunkBytes);
    m_memory->addFile(QStringLiteral("/source/exact.bin"), payload);
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));

    TransferTask* task = copy(m_memory, VfsUri::fromString(QStringLiteral("mem:///source/exact.bin")), m_disk,
        m_tree->rootUri().child(QStringLiteral("arrived")));
    QVERIFY(task != nullptr);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(onDisk(QStringLiteral("arrived/exact.bin")), payload);
}

void TestFilesArriveWhole::anEmptyFileIsCreatedRatherThanSkipped()
{
    // Nothing to copy is not nothing to do. An empty file that never arrives is
    // a missing file, and on an object store an empty object is a real thing
    // that other software goes looking for.
    m_memory->addFile(QStringLiteral("/source/marker"), QByteArray());
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));

    TransferTask* task = copy(m_memory, VfsUri::fromString(QStringLiteral("mem:///source/marker")), m_disk,
        m_tree->rootUri().child(QStringLiteral("arrived")));
    QVERIFY(task != nullptr);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY2(QFile::exists(m_tree->absolute(QStringLiteral("arrived/marker"))),
        "an empty file has to be created, not skipped for having no bytes");
    QCOMPARE(QFileInfo(m_tree->absolute(QStringLiteral("arrived/marker"))).size(), 0);
}

void TestFilesArriveWhole::aSparseFileArrivesWithTheSameContent()
{
    // A file with a hole in it reads as zeroes and occupies almost nothing. What
    // a copy must preserve is the content and the length; whether the hole
    // survives is the filesystem's business, and claiming otherwise would be a
    // promise nothing here can keep.
    QVERIFY(m_tree->makeDirs(QStringLiteral("source")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));

    const qint64 length = 4 * 1024 * 1024;
    const QByteArray tail("the end");
    {
        QFile sparse(m_tree->absolute(QStringLiteral("source/sparse.bin")));
        QVERIFY(sparse.open(QIODevice::WriteOnly));
        QVERIFY(sparse.resize(length));
        QVERIFY(sparse.seek(length - tail.size()));
        QCOMPARE(sparse.write(tail), static_cast<qint64>(tail.size()));
    }

    TransferTask* task
        = copy(m_disk, m_tree->rootUri().child(QStringLiteral("source")).child(QStringLiteral("sparse.bin")),
            m_disk, m_tree->rootUri().child(QStringLiteral("arrived")));
    QVERIFY(task != nullptr);
    QCOMPARE(task->state(), Task::State::Succeeded);

    const QByteArray arrived = onDisk(QStringLiteral("arrived/sparse.bin"));
    QCOMPARE(arrived.size(), static_cast<int>(length));
    QCOMPARE(arrived.right(tail.size()), tail);
    QCOMPARE(arrived.left(static_cast<int>(length) - tail.size()),
        QByteArray(static_cast<int>(length) - tail.size(), '\0'));
}

void TestFilesArriveWhole::aTreeAHundredLevelsDeepArrivesWhole()
{
    // The walker is recursive and the paths get long. A hundred levels is far
    // past anything a person builds by hand and far short of a path limit, which
    // makes it the depth that tells recursion apart from the filesystem.
    constexpr int kDepth = 100;
    QString path;
    for (int level = 0; level < kDepth; ++level)
        path += QStringLiteral("/level%1").arg(level);
    m_memory->addFile(QStringLiteral("/source%1/leaf.txt").arg(path), QByteArray("the bottom"));
    m_memory->addFile(QStringLiteral("/source/top.txt"), QByteArray("the top"));
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));

    TransferTask* task = copy(m_memory, VfsUri::fromString(QStringLiteral("mem:///source")), m_disk,
        m_tree->rootUri().child(QStringLiteral("arrived")));
    QVERIFY(task != nullptr);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->copiedCount(), 2);

    QCOMPARE(onDisk(QStringLiteral("arrived/source%1/leaf.txt").arg(path)), QByteArray("the bottom"));
    QCOMPARE(onDisk(QStringLiteral("arrived/source/top.txt")), QByteArray("the top"));
}

void TestFilesArriveWhole::tenThousandSmallFilesAllArrive()
{
    // Not about size: about per-file overhead, and about whether anything leaks
    // a descriptor or a handle once per file. Ten thousand of them is enough
    // that a leak of one apiece hits the process limit.
    constexpr int kFiles = 10000;
    for (int i = 0; i < kFiles; ++i) {
        m_memory->addFile(
            QStringLiteral("/source/file%1.bin").arg(i, 5, 10, QLatin1Char('0')), payloadOf(64 + (i % 97)));
    }
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));

    TransferTask* task = copy(m_memory, VfsUri::fromString(QStringLiteral("mem:///source")), m_disk,
        m_tree->rootUri().child(QStringLiteral("arrived")));
    QVERIFY(task != nullptr);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->copiedCount(), kFiles);

    // Counted out of band, and a sample of the content checked rather than all
    // of it: the count catches a file that never arrived, and the sample catches
    // bytes going to the wrong name.
    const QDir arrived(m_tree->absolute(QStringLiteral("arrived/source")));
    QCOMPARE(arrived.entryList(QDir::Files).size(), kFiles);
    for (int i = 0; i < kFiles; i += 997) {
        const QString name = QStringLiteral("file%1.bin").arg(i, 5, 10, QLatin1Char('0'));
        QCOMPARE(onDisk(QStringLiteral("arrived/source/") + name), payloadOf(64 + (i % 97)));
    }
}

MOLE_TEST_MAIN(TestFilesArriveWhole)

#include "tst_FilesArriveWhole.moc"
