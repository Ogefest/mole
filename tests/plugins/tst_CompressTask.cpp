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
    void changingTheFormatKeepsTheNameThatWasTyped();
    void aPasswordEncryptsTheContents();
    void aPasswordOnAFormatThatCannotCarryOneIsRefused();
    void aBareXzHoldsOneFileAndSaysSoOtherwise();
    void sevenZipCannotCarryAPasswordAndSaysSo();
    void theOriginalsGoOnlyWhenAskedAndOnlyAfterTheArchiveIsWritten();
    void theOriginalsAreKeptWhenAnythingCouldNotBeRead();
    void theSourceHoldingTheArchiveIsNeverDeleted();
    void anArchiveWrittenIntoTheTreeItIsArchivingDoesNotIncludeItself();

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

void TestCompressTask::anArchiveWrittenIntoTheTreeItIsArchivingDoesNotIncludeItself()
{
    // The archive is being written into the folder it is packing, so the folder
    // grows a file while it is being read. Including it would mean packing a
    // file that is still being written -- and, on a format that can be appended
    // to, packing what it has just packed.
    CompressTask::Request request;
    request.sourceFileSystem = m_fs;
    request.targetFileSystem = m_fs;
    request.sources.append(m_tree->rootUri().child(QStringLiteral("reports")));
    request.target = m_tree->rootUri().child(QStringLiteral("reports/self.zip"));

    auto* task = new CompressTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));
    QCOMPARE(task->state(), Task::State::Succeeded);

    const QStringList inside = contentsOf(QStringLiteral("reports/self.zip"));
    for (const QString& entry : inside) {
        QVERIFY2(!entry.endsWith(QLatin1String("self.zip")),
            qPrintable(QStringLiteral("the archive packed itself: %1").arg(entry)));
    }
    QVERIFY2(!inside.isEmpty(), "and it packed everything else");
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
    QTest::newRow("7z") << int(CompressTask::Format::SevenZip) << "bundle.7z";
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

void TestCompressTask::changingTheFormatKeepsTheNameThatWasTyped()
{
    // The bug this exists for: choosing a different kind rebuilt the name from the
    // selection, so a name somebody had typed was silently replaced and the archive
    // was written under the suggested one.
    QCOMPARE(CompressTask::nameWithSuffix(QStringLiteral("holiday.zip"), CompressTask::Format::TarGz),
        QStringLiteral("holiday.tar.gz"));
    QCOMPARE(CompressTask::nameWithSuffix(QStringLiteral("holiday.tar.gz"), CompressTask::Format::Zip),
        QStringLiteral("holiday.zip"));

    // The multi-part suffix has to go whole, or ".tar.gz" leaves "holiday.tar".
    QCOMPARE(CompressTask::nameWithSuffix(QStringLiteral("holiday.tar.xz"), CompressTask::Format::SevenZip),
        QStringLiteral("holiday.7z"));
    QCOMPARE(CompressTask::nameWithSuffix(QStringLiteral("holiday.7z"), CompressTask::Format::TarXz),
        QStringLiteral("holiday.tar.xz"));

    // Dots in the name are part of the name.
    QCOMPARE(CompressTask::nameWithSuffix(QStringLiteral("my.notes.v2.zip"), CompressTask::Format::Xz),
        QStringLiteral("my.notes.v2.xz"));
    // Nothing to strip means nothing is stripped.
    QCOMPARE(CompressTask::nameWithSuffix(QStringLiteral("holiday"), CompressTask::Format::Zip),
        QStringLiteral("holiday.zip"));
    // Case does not save a suffix from being replaced.
    QCOMPARE(CompressTask::nameWithSuffix(QStringLiteral("holiday.ZIP"), CompressTask::Format::TarGz),
        QStringLiteral("holiday.tar.gz"));
    // Nothing at all is nothing, rather than a file called ".zip".
    QVERIFY(CompressTask::nameWithSuffix(QString(), CompressTask::Format::Zip).isEmpty());
    QVERIFY(CompressTask::nameWithSuffix(QStringLiteral("  "), CompressTask::Format::Zip).isEmpty());
}

void TestCompressTask::aPasswordEncryptsTheContents()
{
    CompressTask::Request request;
    request.sourceFileSystem = m_fs;
    request.targetFileSystem = m_fs;
    request.sources.append(m_tree->rootUri().child(QStringLiteral("notes.txt")));
    request.target = m_tree->rootUri().child(QStringLiteral("secret.zip"));
    request.passphrase = QStringLiteral("correct horse battery staple");

    auto* task = new CompressTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("secret.zip"))));

    // The point of the password, checked rather than assumed: the plain text is not
    // sitting in the file. An archive that accepted a password and wrote the
    // contents in the clear would pass every other assertion here.
    QFile written(QDir(m_tree->path()).filePath(QStringLiteral("secret.zip")));
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QByteArray bytes = written.readAll();
    QVERIFY2(!bytes.contains(QByteArray("plain notes")), "the contents must not be readable in the file");

    // And the reader that browses archives cannot get at the contents without it --
    // the names are still listed, because zip encrypts what is in the entries rather
    // than the list of them.
    auto archive
        = std::make_shared<ArchiveFileSystem>(QDir(m_tree->path()).filePath(QStringLiteral("secret.zip")));
    Result<std::unique_ptr<QIODevice>> opened
        = archive->openRead(VfsUri::fromString(QStringLiteral("archive:///notes.txt")));
    if (opened.ok()) {
        const QByteArray got = opened.value()->readAll();
        QVERIFY2(got != QByteArray("plain notes"),
            "reading an encrypted entry without the password must not hand back the contents");
    }
}

void TestCompressTask::aPasswordOnAFormatThatCannotCarryOneIsRefused()
{
    QVERIFY(!CompressTask::formatSupportsPassword(CompressTask::Format::TarGz));
    QVERIFY(!CompressTask::formatSupportsPassword(CompressTask::Format::TarXz));
    QVERIFY(CompressTask::formatSupportsPassword(CompressTask::Format::Zip));

    CompressTask::Request request;
    request.sourceFileSystem = m_fs;
    request.targetFileSystem = m_fs;
    request.sources.append(m_tree->rootUri().child(QStringLiteral("notes.txt")));
    request.target = m_tree->rootUri().child(QStringLiteral("impossible.tar.gz"));
    request.format = CompressTask::Format::TarGz;
    request.passphrase = QStringLiteral("hunter2");

    auto* task = new CompressTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    // Refused, not silently written in the clear. Someone who typed a password and
    // got an archive anybody can open has been lied to.
    QCOMPARE(task->state(), Task::State::Failed);
    QVERIFY(task->error().message.contains(QStringLiteral("password")));
    QVERIFY2(!QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("impossible.tar.gz"))),
        "and nothing is left behind");
}

void TestCompressTask::aBareXzHoldsOneFileAndSaysSoOtherwise()
{
    // One file, no container: what a bare .xz is.
    // Big and repetitive, because eleven bytes are not compressible: LZMA2 stores
    // an input that small as an uncompressed chunk, and the plain text stays visible
    // inside a perfectly valid .xz. Asserting on that would have been asserting on
    // the size of the fixture.
    const QByteArray payload = QByteArray("the same line over and over\n").repeated(2000);
    QVERIFY(m_tree->writeFile(QStringLiteral("big-notes.txt"), payload));

    CompressTask::Request one;
    one.sourceFileSystem = m_fs;
    one.targetFileSystem = m_fs;
    one.format = CompressTask::Format::Xz;
    one.sources.append(m_tree->rootUri().child(QStringLiteral("big-notes.txt")));
    one.target = m_tree->rootUri().child(QStringLiteral("big-notes.xz"));

    auto* single = new CompressTask(one);
    m_tasks->submit(single);
    QVERIFY(waitForTask(single, 30000));
    QCOMPARE(single->state(), Task::State::Succeeded);

    // A bare .xz carries no names -- there is no container to keep them in -- so
    // there is no entry list to compare, which is a property of the format rather
    // than a shortcoming of the writer. What can be checked is that it is an xz
    // stream and that it really compressed rather than copied.
    QFile written(QDir(m_tree->path()).filePath(QStringLiteral("big-notes.xz")));
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QByteArray bytes = written.readAll();
    QCOMPARE(bytes.left(6), QByteArray::fromHex("fd377a585a00"));
    // Smaller, but not by as much as the payload suggests: libarchive pads its
    // output to a ten-kilobyte block, so a highly compressible 56 kB file comes out
    // at 10 kB of which most is padding. Checked with `xz -t` and `xz -dc` while
    // writing this -- the stream is valid and gives back every original byte -- so
    // the bound stays loose on purpose rather than being tightened into a flake.
    QVERIFY2(bytes.size() < payload.size(),
        qPrintable(QStringLiteral("%1 bytes from %2: the filter has to actually compress")
                       .arg(bytes.size())
                       .arg(payload.size())));

    // Several items, or a folder, cannot fit in a format with no container -- said
    // before a byte is written rather than failing on the second entry with "Raw
    // format only supports one entry per archive", which is true and useless.
    QVERIFY(CompressTask::takesOneFileOnly(CompressTask::Format::Xz));
    QVERIFY(!CompressTask::takesOneFileOnly(CompressTask::Format::TarXz));

    CompressTask::Request several = one;
    several.sources.append(m_tree->rootUri().child(QStringLiteral("reports")));
    several.target = m_tree->rootUri().child(QStringLiteral("several.xz"));

    auto* many = new CompressTask(several);
    m_tasks->submit(many);
    QVERIFY(waitForTask(many, 30000));
    QCOMPARE(many->state(), Task::State::Failed);
    QVERIFY(many->error().message.contains(QStringLiteral("one file")));
    QVERIFY(!QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("several.xz"))));
}

void TestCompressTask::sevenZipCannotCarryAPasswordAndSaysSo()
{
    // Measured, not assumed: this libarchive rejects "7zip:encryption" as an
    // undefined option. The trap is that it accepts a passphrase anyway and the
    // written file contains no plain text -- because LZMA2 compressed it, not
    // because anything encrypted it. So the answer is a fact about the format.
    QVERIFY(!CompressTask::formatSupportsPassword(CompressTask::Format::SevenZip));

    CompressTask::Request request;
    request.sourceFileSystem = m_fs;
    request.targetFileSystem = m_fs;
    request.format = CompressTask::Format::SevenZip;
    request.passphrase = QStringLiteral("hunter2");
    request.sources.append(m_tree->rootUri().child(QStringLiteral("notes.txt")));
    request.target = m_tree->rootUri().child(QStringLiteral("secret.7z"));

    auto* task = new CompressTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    // Refused rather than written unencrypted with a password box ticked, which
    // would be the worst of both.
    QCOMPARE(task->state(), Task::State::Failed);
    QVERIFY(task->error().message.contains(QStringLiteral("password")));
    QVERIFY(!QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("secret.7z"))));
}

// "I want the archive, not the files." The deletion happens after the archive is
// written and closed -- never as part of writing it -- so there is no window where
// the originals are gone and the archive is not yet there.
void TestCompressTask::theOriginalsGoOnlyWhenAskedAndOnlyAfterTheArchiveIsWritten()
{
    // Unasked first, in the same tree: this is what stops the deletion becoming
    // something that quietly happens to everybody.
    CompressTask* kept = pack({ QStringLiteral("notes.txt") }, QStringLiteral("kept.zip"));
    QVERIFY(kept);
    QCOMPARE(kept->state(), Task::State::Succeeded);
    QVERIFY2(QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("notes.txt"))),
        "nothing is deleted unless it was asked for");
    QVERIFY(kept->removedSources().isEmpty());

    CompressTask::Request request;
    request.sourceFileSystem = m_fs;
    request.targetFileSystem = m_fs;
    request.sources.append(m_tree->rootUri().child(QStringLiteral("notes.txt")));
    request.sources.append(m_tree->rootUri().child(QStringLiteral("reports")));
    request.target = m_tree->rootUri().child(QStringLiteral("packed.zip"));
    request.removeSourcesWhenDone = true;

    auto* task = new CompressTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));
    QCOMPARE(task->state(), Task::State::Succeeded);

    // The archive first, because that is the whole point: what was deleted has to be
    // recoverable from what was written.
    const QStringList inside = contentsOf(QStringLiteral("packed.zip"));
    QVERIFY(inside.contains(QStringLiteral("/notes.txt")));
    QVERIFY(inside.contains(QStringLiteral("/reports/q1.txt")));
    QVERIFY(inside.contains(QStringLiteral("/reports/deeper/q2.txt")));

    QVERIFY2(!QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("notes.txt"))),
        "the file that was packed is gone");
    QVERIFY2(!QDir(QDir(m_tree->path()).filePath(QStringLiteral("reports"))).exists(),
        "a folder goes with everything inside it");
    QCOMPARE(task->removedSources().size(), 2);
    QVERIFY2(task->statusText().contains(QStringLiteral("removed")), "and it says so");
}

// The archive is the only copy once the originals go, so anything missing from it
// means nothing may be deleted. One unreadable file is enough.
void TestCompressTask::theOriginalsAreKeptWhenAnythingCouldNotBeRead()
{
    const QString unreadable = QDir(m_tree->path()).filePath(QStringLiteral("reports/q1.txt"));
    QFile locked(unreadable);
    QVERIFY(locked.setPermissions(QFile::Permissions {}));

    CompressTask::Request request;
    request.sourceFileSystem = m_fs;
    request.targetFileSystem = m_fs;
    request.sources.append(m_tree->rootUri().child(QStringLiteral("reports")));
    request.target = m_tree->rootUri().child(QStringLiteral("partial.zip"));
    request.removeSourcesWhenDone = true;

    auto* task = new CompressTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));

    // Restore first, so a failure here does not leave the temporary tree undeletable.
    locked.setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY2(!task->failures().isEmpty(), "the unreadable file was recorded");
    QVERIFY2(QDir(QDir(m_tree->path()).filePath(QStringLiteral("reports"))).exists(),
        "nothing is deleted when the archive is not a complete copy");
    QVERIFY(task->removedSources().isEmpty());
    QVERIFY2(task->statusText().contains(QStringLiteral("kept")), "and it says why");
}

// Packing the folder you are standing in writes the archive inside it. Deleting that
// source would take the archive with it -- turning "keep the archive, drop the files"
// into keeping nothing at all.
void TestCompressTask::theSourceHoldingTheArchiveIsNeverDeleted()
{
    CompressTask::Request request;
    request.sourceFileSystem = m_fs;
    request.targetFileSystem = m_fs;
    request.sources.append(m_tree->rootUri().child(QStringLiteral("reports")));
    request.target = m_tree->rootUri().child(QStringLiteral("reports/inside.zip"));
    request.removeSourcesWhenDone = true;

    auto* task = new CompressTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 30000));
    QCOMPARE(task->state(), Task::State::Succeeded);

    QVERIFY2(QFile::exists(QDir(m_tree->path()).filePath(QStringLiteral("reports/inside.zip"))),
        "the archive is still there");
    QVERIFY2(QDir(QDir(m_tree->path()).filePath(QStringLiteral("reports"))).exists(),
        "and so is the folder holding it");
    QVERIFY(task->removedSources().isEmpty());
    QVERIFY2(task->statusText().contains(QStringLiteral("could not be removed")),
        "said plainly rather than silently skipped");
}

MOLE_TEST_MAIN(TestCompressTask)
#include "tst_CompressTask.moc"
