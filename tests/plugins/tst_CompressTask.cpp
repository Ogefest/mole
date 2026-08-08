#include "plugins/archive/ArchiveFileSystem.h"
#include "plugins/archive/CompressTask.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/vfs/DirectoryWalker.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>

using namespace mole;
using namespace mole::test;

/// Writing an archive, checked by reading it back with the backend that browses
/// them -- so the writer and the reader hold each other to account rather than the
/// writer being checked against its own idea of what it wrote.
class TestCompressTask : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void packsFilesAndFoldersThatComeBackOut_data();
    void packsFilesAndFoldersThatComeBackOut();
    void refusesToOverwriteAnArchiveThatExists();
    void cancellingLeavesNoHalfWrittenArchive();
    void anUnreadableFileIsRecordedRatherThanFatal();
    void namesAndSuffixesMatchTheFormats();

private:
    /// Runs a compression to completion and returns the task.
    CompressTask* pack(const QStringList& relativeSources, const QString& archiveName,
        CompressTask::Format format = CompressTask::Format::Zip);
    /// Every path inside an archive, read back through the archive backend.
    QStringList contentsOf(const QString& archiveName);

    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<TaskManager> m_tasks;
    FileSystemPtr m_fs;
};

void TestCompressTask::init()
{
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->writeFile(QStringLiteral("notes.txt"), QByteArray("plain notes")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("reports/deeper")));
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/q1.txt"), QByteArray("first quarter")));
    QVERIFY(m_tree->writeFile(QStringLiteral("reports/deeper/q2.txt"), QByteArray("second quarter")));

    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<LocalFileSystem>();
}

void TestCompressTask::cleanup()
{
    m_tasks.reset();
    m_tree.reset();
}

CompressTask* TestCompressTask::pack(
    const QStringList& relativeSources, const QString& archiveName, CompressTask::Format format)
{
    CompressTask::Request request;
    request.sourceFileSystem = m_fs;
    request.targetFileSystem = m_fs;
    request.format = format;
    for (const QString& relative : relativeSources)
        request.sources.append(m_tree->rootUri().child(relative));
    request.target = m_tree->rootUri().child(archiveName);

    auto* task = new CompressTask(request);
    m_tasks->submit(task);
    if (!waitForTask(task, 30000))
        return nullptr;
    return task;
}

QStringList TestCompressTask::contentsOf(const QString& archiveName)
{
    const QString path = QDir(m_tree->path()).filePath(archiveName);
    auto archive = std::make_shared<ArchiveFileSystem>(path);

    QStringList paths;
    DirectoryWalker walker(archive);
    walker.walk(VfsUri::fromString(QStringLiteral("archive:///")), CancelToken {},
        [&paths](const FileEntry& entry, int) {
            paths.append(entry.uri.path());
            return DirectoryWalker::Action::Continue;
        });
    paths.sort();
    return paths;
}

void TestCompressTask::packsFilesAndFoldersThatComeBackOut_data()
{
    QTest::addColumn<int>("format");
    QTest::addColumn<QString>("name");

    // Every format offered, because "it works" for zip says nothing about the others.
    QTest::newRow("zip") << int(CompressTask::Format::Zip) << "bundle.zip";
    QTest::newRow("tar.gz") << int(CompressTask::Format::TarGz) << "bundle.tar.gz";
    QTest::newRow("tar.xz") << int(CompressTask::Format::TarXz) << "bundle.tar.xz";
}

void TestCompressTask::packsFilesAndFoldersThatComeBackOut()
{
    QFETCH(int, format);
    QFETCH(QString, name);

    CompressTask* task = pack({ QStringLiteral("notes.txt"), QStringLiteral("reports") }, name,
        static_cast<CompressTask::Format>(format));
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->failures().isEmpty());
    QVERIFY(QFile::exists(QDir(m_tree->path()).filePath(name)));

    // Read back through the browsing backend. A folder comes back as a folder with
    // what was inside it, and the paths are relative to the folder rather than to
    // the drive -- unpacking should give back "reports", not a chain of parents.
    const QStringList inside = contentsOf(name);
    QVERIFY2(inside.contains(QStringLiteral("/notes.txt")), qPrintable(inside.join(QLatin1Char(' '))));
    QVERIFY2(inside.contains(QStringLiteral("/reports/q1.txt")), qPrintable(inside.join(QLatin1Char(' '))));
    QVERIFY2(
        inside.contains(QStringLiteral("/reports/deeper/q2.txt")), qPrintable(inside.join(QLatin1Char(' '))));

    // And the bytes survived the round trip, not just the names.
    auto archive = std::make_shared<ArchiveFileSystem>(QDir(m_tree->path()).filePath(name));
    Result<std::unique_ptr<QIODevice>> opened
        = archive->openRead(VfsUri::fromString(QStringLiteral("archive:///reports/q1.txt")));
    QVERIFY(opened.ok());
    QCOMPARE(opened.value()->readAll(), QByteArray("first quarter"));
}

void TestCompressTask::refusesToOverwriteAnArchiveThatExists()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("taken.zip"), QByteArray("not really an archive")));

    CompressTask* task = pack({ QStringLiteral("notes.txt") }, QStringLiteral("taken.zip"));
    QVERIFY(task);
    // An archive somebody already has is not this operation's to replace.
    QCOMPARE(task->state(), Task::State::Failed);
    QCOMPARE(QFile(QDir(m_tree->path()).filePath(QStringLiteral("taken.zip"))).size(),
        qint64(QByteArray("not really an archive").size()));
}

void TestCompressTask::cancellingLeavesNoHalfWrittenArchive()
{
    // The source is a drive that is slow to open files, not a big local tree.
    // Waiting to observe Running and then cancelling is a race the task can win --
    // under a loaded machine it did, once, and the test failed for no reason of its
    // own. A drive that takes its time makes the cancel land mid-write every time.
    auto slow = std::make_shared<MemoryFileSystem>();
    for (int i = 0; i < 6; ++i)
        slow->addFile(QStringLiteral("/bulk/file%1.bin").arg(i), QByteArray(64 * 1024, 'x'));
    slow->setReadDelayMs(400);

    CompressTask::Request request;
    request.sourceFileSystem = slow;
    request.targetFileSystem = m_fs;
    request.sources.append(VfsUri::fromString(QStringLiteral("mem://slow/bulk")));
    request.target = m_tree->rootUri().child(QStringLiteral("cancelled.zip"));

    auto* task = new CompressTask(request);
    // Cancelled from the first sign of progress, which cannot happen before the
    // task has started writing.
    connect(task, &Task::statusTextChanged, task, [task] { task->requestCancel(); });

    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    // An archive that exists is one that finished. A partial file waiting to be
    // mistaken for a good one is the worst outcome available here.
    QVERIFY2(!QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("cancelled.zip"))),
        "a cancelled compression leaves nothing behind");
    QVERIFY(task->state() == Task::State::Cancelled || task->state() == Task::State::Failed);
}

void TestCompressTask::anUnreadableFileIsRecordedRatherThanFatal()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("mixed/readable.txt"), QByteArray("fine")));
    QVERIFY(m_tree->writeFile(QStringLiteral("mixed/locked.txt"), QByteArray("secret")));

    const QString locked = QDir(m_tree->path()).filePath(QStringLiteral("mixed/locked.txt"));
    if (!QFile::setPermissions(locked, {}))
        QSKIP("cannot make a file unreadable here");

    // The unreadable one named first, deliberately. The walker gives no order, so
    // packing the folder made this a coin toss -- and the bug it found only showed
    // when the unreadable file came before a good one, which is why the test failed
    // about one run in three instead of every time.
    CompressTask* task = pack({ QStringLiteral("mixed/locked.txt"), QStringLiteral("mixed/readable.txt") },
        QStringLiteral("partial.zip"));
    QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QVERIFY(task);

    // The rest of the archive is still worth having, and what was missed is said
    // rather than swallowed.
    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->failures().size(), 1);
    QVERIFY(task->failures().first().contains(QStringLiteral("locked.txt")));
    // The good file is in the archive and the archive is readable: a header written
    // for a file that then could not be read would have left an unkept promise of N
    // bytes and corrupted everything after it.
    QVERIFY(contentsOf(QStringLiteral("partial.zip")).contains(QStringLiteral("/readable.txt")));
}

void TestCompressTask::namesAndSuffixesMatchTheFormats()
{
    QCOMPARE(CompressTask::formatNames().first(), QStringLiteral("zip"));
    QCOMPARE(CompressTask::suffixFor(CompressTask::Format::Zip), QStringLiteral(".zip"));
    QCOMPARE(CompressTask::suffixFor(CompressTask::Format::TarGz), QStringLiteral(".tar.gz"));
    QCOMPARE(CompressTask::suffixFor(CompressTask::Format::TarXz), QStringLiteral(".tar.xz"));

    // Round trip through the names the interface uses.
    for (const QString& name : CompressTask::formatNames())
        QCOMPARE(CompressTask::suffixFor(CompressTask::formatFromName(name)), QStringLiteral(".") + name);

    // Anything unrecognised is zip, because that is the one anyone can open.
    QCOMPARE(CompressTask::formatFromName(QStringLiteral("nonsense")), CompressTask::Format::Zip);
    QCOMPARE(CompressTask::formatFromName(QString()), CompressTask::Format::Zip);
}

MOLE_TEST_MAIN(TestCompressTask)
#include "tst_CompressTask.moc"
