#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

class TestLocalFileSystem : public QObject
{
    Q_OBJECT

private slots:
    /// The shared contract. If this passes, the backend behaves like every
    /// other backend as far as the rest of the application is concerned.
    void conformance();

    void reportsHiddenFiles();
    void listsRootWithoutCrashing();
    void factoryProducesUsableBackend();
    void theListingAndTheDetailsAgreeAboutModeStrings();
    void aPlatformWithoutModesReportsNoneRatherThanSynthesisingOne();

    void aRenameOntoAnotherDeviceIsRefusedRatherThanQuietlyCopied();
    void aFolderRenamedOntoAnotherDeviceIsRefusedWithTheSameKind();

    void replaceLeavesTheDestinationHoldingTheNewBytes();
    void replaceWithAWorkingFileThatIsGoneKeepsTheDestination();
    void replaceAcrossKindsFallsBackToTheTwoStep();
};

void TestLocalFileSystem::conformance()
{
    TempTree tree;
    QVERIFY(tree.isValid());

    ConformanceContext context;
    context.fileSystem = std::make_shared<LocalFileSystem>();
    context.root = tree.rootUri();
    context.seedFile
        = [&tree](const QString& path, const QByteArray& data) { return tree.writeFile(path, data); };
    context.seedDir = [&tree](const QString& path) { return tree.makeDirs(path); };
    context.whileUnlistable = [&tree](const QString& path, const std::function<void()>& check) {
        const QString absolute = tree.absolute(path);
        if (!madeUnreadable(absolute))
            return false;
        check();
        // Put back whatever the assertion did, so the temporary tree can still
        // be deleted -- a directory nobody may read takes the whole tree with it.
        QFile::setPermissions(
            absolute, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        return true;
    };

    runFileSystemConformance(context);
}

void TestLocalFileSystem::reportsHiddenFiles()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral(".hidden")));
    QVERIFY(tree.writeFile(QStringLiteral("visible")));

    LocalFileSystem fs;
    Result<FileEntryList> listing = fs.list(tree.rootUri(), CancelToken());
    QVERIFY(listing.ok());
    QCOMPARE(listing.value().size(), 2);

    for (const FileEntry& entry : listing.value())
        QCOMPARE(entry.isHidden, entry.name.startsWith(QLatin1Char('.')));
}

void TestLocalFileSystem::listsRootWithoutCrashing()
{
    LocalFileSystem fs;
    Result<FileEntryList> listing = fs.list(VfsUri::fromLocalPath(QDir::rootPath()), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QVERIFY(!listing.value().isEmpty());
}

void TestLocalFileSystem::factoryProducesUsableBackend()
{
    LocalFileSystemFactory factory;
    QCOMPARE(factory.scheme(), QStringLiteral("file"));

    QString error;
    FileSystemPtr fs = factory.create({}, &error);
    QVERIFY2(fs != nullptr, qPrintable(error));
    QCOMPARE(fs->scheme(), QStringLiteral("file"));
}

void TestLocalFileSystem::theListingAndTheDetailsAgreeAboutModeStrings()
{
    // Compared against each other rather than against one platform's spelling,
    // so this runs and means something on all three -- and would have caught the
    // fault on the day the guard was added to one of them and not the other.
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("notes.txt"), QByteArray("hello")));

    LocalFileSystem fs;
    const VfsUri file = tree.rootUri().child(QStringLiteral("notes.txt"));

    Result<FileEntryList> listed = fs.list(tree.rootUri(), CancelToken());
    QVERIFY(listed.ok());
    QCOMPARE(listed.value().size(), 1);

    Result<AccessInfo> access = fs.access(file);
    QVERIFY(access.ok());

    QCOMPARE(listed.value().first().permissions.isEmpty(), access.value().nativeText.isEmpty());
    QCOMPARE(listed.value().first().permissions, access.value().nativeText);
}

void TestLocalFileSystem::aPlatformWithoutModesReportsNoneRatherThanSynthesisingOne()
{
    const QFile::Permissions readable = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup;

    // Windows has no mode. Qt synthesises one from the ACL, and nine characters
    // of it read as fact -- so nothing is offered, and an alert rule about
    // permissions has nothing to fire on rather than something invented.
    QVERIFY(LocalFileSystem::modeString(readable, HostPlatform::Windows).isEmpty());

    QCOMPARE(LocalFileSystem::modeString(readable, HostPlatform::Posix), QStringLiteral("rw-r-----"));
    QCOMPARE(LocalFileSystem::modeString(readable, HostPlatform::MacOS), QStringLiteral("rw-r-----"));
    QCOMPARE(
        LocalFileSystem::modeString(QFile::Permissions(), HostPlatform::Posix), QStringLiteral("---------"));
}

/// The one-step overwrite, and the reason it has to be one step.
///
/// `rename(2)` puts one file over another with no instant in between at which
/// the name has nothing at it. Everything else -- every protocol backend, and
/// this class until MOLE-331 -- has to remove the destination and then rename,
/// and the gap between those two calls is a window in which the file being
/// replaced is already gone and the replacement is not there yet.
/// A move that crosses a mount point, from down here.
///
/// QFile::rename does not fail on EXDEV: for a regular file it opens the
/// destination truncating, copies it in 4 KiB blocks and removes the source. So
/// the one call that is supposed to be instant and atomic silently becomes a
/// transfer with no working name, no arrival weighing, no short-read guard, no
/// cancellation and no progress -- and a file truncated under its final name if
/// the process dies half way. Refusing is what lets the copy path, which has all
/// of those, take the work instead.
///
/// /dev/shm is tmpfs wherever this runs and the temporary directory is usually
/// the real disk, but a machine where both are one filesystem has no
/// cross-device case to offer and the test says so rather than passing on
/// nothing.
void TestLocalFileSystem::aRenameOntoAnotherDeviceIsRefusedRatherThanQuietlyCopied()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("report.txt"), QByteArrayLiteral("the only copy")));

    QTemporaryDir elsewhere(QStringLiteral("/dev/shm/mole-other-device-XXXXXX"));
    if (!elsewhere.isValid())
        QSKIP("no second filesystem on this machine to move onto");
    if (QStorageInfo(tree.path()).device() == QStorageInfo(elsewhere.path()).device())
        QSKIP("the temporary directory and /dev/shm are the same filesystem");

    LocalFileSystem fs;
    const QString destination = elsewhere.filePath(QStringLiteral("report.txt"));
    const Result<void> renamed
        = fs.rename(tree.rootUri().child(QStringLiteral("report.txt")), VfsUri::fromLocalPath(destination));

    QVERIFY2(!renamed.ok(), "a rename across a device boundary is a copy, and has to be refused as one");
    QCOMPARE(renamed.error().code, VfsError::NotSupported);
    QVERIFY2(!QFile::exists(destination), "the destination was written by an unguarded block copy");
    QVERIFY2(QFile::exists(tree.absolute(QStringLiteral("report.txt"))), "the source was removed anyway");
}

/// The same boundary for a directory, which is the half that failed outright.
///
/// QFile::rename has no copy to fall back on for a folder, so it returns false
/// and the backend used to map that to IoError -- which the transfer task reads
/// as a real failure rather than as "ask somebody else". The kind is the whole
/// fix: both halves have to answer NotSupported for the fallback to engage.
void TestLocalFileSystem::aFolderRenamedOntoAnotherDeviceIsRefusedWithTheSameKind()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("work/notes.txt"), QByteArrayLiteral("keep me")));

    QTemporaryDir elsewhere(QStringLiteral("/dev/shm/mole-other-device-XXXXXX"));
    if (!elsewhere.isValid())
        QSKIP("no second filesystem on this machine to move onto");
    if (QStorageInfo(tree.path()).device() == QStorageInfo(elsewhere.path()).device())
        QSKIP("the temporary directory and /dev/shm are the same filesystem");

    LocalFileSystem fs;
    const QString destination = elsewhere.filePath(QStringLiteral("work"));
    const Result<void> renamed
        = fs.rename(tree.rootUri().child(QStringLiteral("work")), VfsUri::fromLocalPath(destination));

    QVERIFY2(!renamed.ok(), "no filesystem renames a directory across a mount point");
    QCOMPARE(renamed.error().code, VfsError::NotSupported);
    QVERIFY(!QFile::exists(destination));
    QVERIFY(QFile::exists(tree.absolute(QStringLiteral("work/notes.txt"))));
}

void TestLocalFileSystem::replaceLeavesTheDestinationHoldingTheNewBytes()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("report.pdf"), QByteArrayLiteral("the old one")));
    QVERIFY(tree.writeFile(QStringLiteral("report.pdf.mole-partial"), QByteArrayLiteral("the new one")));

    LocalFileSystem fs;
    const VfsUri target = tree.rootUri().child(QStringLiteral("report.pdf"));
    const VfsUri staging = tree.rootUri().child(QStringLiteral("report.pdf.mole-partial"));

    const Result<void> replaced = fs.replace(staging, target);
    QVERIFY2(replaced.ok(), qPrintable(replaced.error().message));

    QFile landed(tree.absolute(QStringLiteral("report.pdf")));
    QVERIFY(landed.open(QIODevice::ReadOnly));
    QCOMPARE(landed.readAll(), QByteArrayLiteral("the new one"));
    QVERIFY2(!QFile::exists(tree.absolute(QStringLiteral("report.pdf.mole-partial"))),
        "the working name is gone once the bytes are under the real one");
}

/// The window, made visible.
///
/// A commit whose working file has been swept away -- by a cleanup, by another
/// process, by a user tidying up what looked like litter -- is the case that
/// tells the two implementations apart deterministically. Remove-then-rename
/// destroys the destination and *then* discovers there is nothing to put in its
/// place, so both files are gone and the failure is reported afterwards. One
/// step fails as one step: nothing was there to move, so nothing moved.
void TestLocalFileSystem::replaceWithAWorkingFileThatIsGoneKeepsTheDestination()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("report.pdf"), QByteArrayLiteral("the only copy")));

    LocalFileSystem fs;
    const VfsUri target = tree.rootUri().child(QStringLiteral("report.pdf"));
    const VfsUri staging = tree.rootUri().child(QStringLiteral("report.pdf.mole-partial"));

    const Result<void> replaced = fs.replace(staging, target);
    QVERIFY2(!replaced.ok(), "replacing with something that is not there cannot succeed");

    QFile standing(tree.absolute(QStringLiteral("report.pdf")));
    QVERIFY2(standing.open(QIODevice::ReadOnly), "the file being replaced was destroyed by a failed replace");
    QCOMPARE(standing.readAll(), QByteArrayLiteral("the only copy"));
}

/// Where one step is not on offer at all.
///
/// A directory arriving over a file, which is what a same-backend move of a
/// folder onto an existing file is. No filesystem does that in one operation, so
/// it gets the two-step every other backend uses -- there is no atomicity to
/// lose, because there was none available.
void TestLocalFileSystem::replaceAcrossKindsFallsBackToTheTwoStep()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.makeDirs(QStringLiteral("arriving")));
    QVERIFY(tree.writeFile(QStringLiteral("arriving/inside.txt"), QByteArrayLiteral("carried along")));
    QVERIFY(tree.writeFile(QStringLiteral("standing"), QByteArrayLiteral("in the way")));

    LocalFileSystem fs;
    const Result<void> replaced = fs.replace(
        tree.rootUri().child(QStringLiteral("arriving")), tree.rootUri().child(QStringLiteral("standing")));
    QVERIFY2(replaced.ok(), qPrintable(replaced.error().message));

    QVERIFY(QFileInfo(tree.absolute(QStringLiteral("standing"))).isDir());
    QVERIFY(QFile::exists(tree.absolute(QStringLiteral("standing/inside.txt"))));
    QVERIFY(!QFile::exists(tree.absolute(QStringLiteral("arriving"))));
}

MOLE_TEST_MAIN(TestLocalFileSystem)
#include "tst_LocalFileSystem.moc"
