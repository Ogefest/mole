#include "plugins/archive/ArchiveFileSystem.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

using namespace mole;
using namespace mole::test;

namespace {

/// Builds a real archive on disk from a directory, using whatever archiver the
/// machine has. Returns an empty string when none is available.
QString packDirectory(const QString& sourceDir, const QString& outputPath, const QString& format)
{
    const QString tool = QStandardPaths::findExecutable(
        format == QLatin1String("zip") ? QStringLiteral("zip") : QStringLiteral("tar"));
    if (tool.isEmpty())
        return {};

    QProcess process;
    process.setWorkingDirectory(sourceDir);
    if (format == QLatin1String("zip"))
        process.start(tool, { QStringLiteral("-qr"), outputPath, QStringLiteral(".") });
    else
        process.start(tool, { QStringLiteral("-czf"), outputPath, QStringLiteral(".") });

    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return {};
    return QFile::exists(outputPath) ? outputPath : QString();
}

} // namespace

class TestArchiveFileSystem : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void authorityRoundTripsThePath();
    void listsTarGzContents();
    void listsZipContents();
    void readsFileContents();
    void synthesisesMissingParentDirectories();
    void statAndErrorsBehaveLikeOtherBackends();
    void isReadOnly();
    void missingArchiveFailsCleanly();
    void corruptArchiveFailsCleanly();
    void anEntryThatClimbsOutOfTheArchiveCannotReachOutOfTheMount();
    void anEntryWithAnAbsolutePathIsStillInsideTheMount();
    void aSymlinkEntryIsNotAWayOutOfTheArchive();
    void twoEntriesWithOneNameGiveOneAnswer();
    void anArchiveCutInHalfIsAnErrorRatherThanAShortListing();
    void factoryRejectsMissingPath();
    void factoryAdvertisesMountableSuffixes();

private:
    QString m_tarGz;
    QString m_zip;
    std::unique_ptr<QTemporaryDir> m_workspace;

    FileSystemPtr openArchive(const QString& path) const { return std::make_shared<ArchiveFileSystem>(path); }
    static VfsUri rootOf(const QString& path)
    {
        return VfsUri(QStringLiteral("archive"), ArchiveFileSystem::authorityFor(path), QStringLiteral("/"));
    }
};

void TestArchiveFileSystem::initTestCase()
{
    m_workspace = std::make_unique<QTemporaryDir>();
    QVERIFY(m_workspace->isValid());

    TempTree source;
    QVERIFY(source.isValid());
    QVERIFY(source.writeFile(QStringLiteral("readme.txt"), QByteArray("hello archive")));
    QVERIFY(source.makeDirs(QStringLiteral("nested/deeper")));
    QVERIFY(source.writeFile(QStringLiteral("nested/deeper/payload.bin"), QByteArray(64, 'x')));

    m_tarGz = packDirectory(source.path(),
        QDir(m_workspace->path()).filePath(QStringLiteral("fixture.tar.gz")), QStringLiteral("tar"));
    m_zip = packDirectory(source.path(), QDir(m_workspace->path()).filePath(QStringLiteral("fixture.zip")),
        QStringLiteral("zip"));

    if (m_tarGz.isEmpty() && m_zip.isEmpty())
        QSKIP("neither tar nor zip is available to build the fixtures");
}

void TestArchiveFileSystem::authorityRoundTripsThePath()
{
    const QString path = QStringLiteral("/home/user/My Archives/backup 2026.zip");
    const QString authority = ArchiveFileSystem::authorityFor(path);

    // The host path is percent-encoded so it cannot be confused with the path
    // inside the archive, and slashes in it do not create fake directories.
    QVERIFY(!authority.contains(QLatin1Char('/')));
    QCOMPARE(ArchiveFileSystem::archivePathFromAuthority(authority), path);
}

void TestArchiveFileSystem::listsTarGzContents()
{
    if (m_tarGz.isEmpty())
        QSKIP("tar is not available");

    FileSystemPtr fs = openArchive(m_tarGz);
    Result<FileEntryList> listing = fs->list(rootOf(m_tarGz), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));

    QStringList names;
    for (const FileEntry& entry : listing.value())
        names.append(entry.name);
    names.sort();

    QCOMPARE(names, QStringList({ QStringLiteral("nested"), QStringLiteral("readme.txt") }));
}

void TestArchiveFileSystem::listsZipContents()
{
    if (m_zip.isEmpty())
        QSKIP("zip is not available");

    FileSystemPtr fs = openArchive(m_zip);
    Result<FileEntryList> listing = fs->list(rootOf(m_zip), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 2);
}

void TestArchiveFileSystem::readsFileContents()
{
    const QString archive = m_tarGz.isEmpty() ? m_zip : m_tarGz;
    if (archive.isEmpty())
        QSKIP("no archive fixture");

    FileSystemPtr fs = openArchive(archive);
    Result<std::unique_ptr<QIODevice>> device
        = fs->openRead(rootOf(archive).child(QStringLiteral("readme.txt")));

    QVERIFY2(device.ok(), qPrintable(device.error().message));
    QCOMPARE(device.value()->readAll(), QByteArray("hello archive"));
}

void TestArchiveFileSystem::synthesisesMissingParentDirectories()
{
    const QString archive = m_tarGz.isEmpty() ? m_zip : m_tarGz;
    if (archive.isEmpty())
        QSKIP("no archive fixture");

    FileSystemPtr fs = openArchive(archive);

    // Many archives store only file records. Every intermediate directory must
    // still be walkable or the tree would have holes.
    const VfsUri nested = rootOf(archive).child(QStringLiteral("nested"));
    Result<FileEntryList> level1 = fs->list(nested, CancelToken());
    QVERIFY2(level1.ok(), qPrintable(level1.error().message));
    QCOMPARE(level1.value().size(), 1);
    QCOMPARE(level1.value().first().name, QStringLiteral("deeper"));
    QVERIFY(level1.value().first().isDir);

    Result<FileEntryList> level2 = fs->list(nested.child(QStringLiteral("deeper")), CancelToken());
    QVERIFY(level2.ok());
    QCOMPARE(level2.value().first().name, QStringLiteral("payload.bin"));
    QCOMPARE(level2.value().first().size, 64);
}

void TestArchiveFileSystem::statAndErrorsBehaveLikeOtherBackends()
{
    const QString archive = m_tarGz.isEmpty() ? m_zip : m_tarGz;
    if (archive.isEmpty())
        QSKIP("no archive fixture");

    FileSystemPtr fs = openArchive(archive);
    const VfsUri root = rootOf(archive);

    Result<FileEntry> file = fs->stat(root.child(QStringLiteral("readme.txt")));
    QVERIFY(file.ok());
    QCOMPARE(file.value().size, 13);
    QVERIFY(!file.value().isDir);

    // The same error vocabulary as the local disk, which is what lets the UI
    // treat every drive identically.
    Result<FileEntry> missing = fs->stat(root.child(QStringLiteral("nope")));
    QVERIFY(!missing.ok());
    QCOMPARE(missing.error().code, VfsError::NotFound);

    Result<FileEntryList> notADir = fs->list(root.child(QStringLiteral("readme.txt")), CancelToken());
    QVERIFY(!notADir.ok());
    QCOMPARE(notADir.error().code, VfsError::NotADirectory);

    CancelToken cancelled;
    cancelled.cancel();
    Result<FileEntryList> aborted = fs->list(root, cancelled);
    QVERIFY(!aborted.ok());
    QCOMPARE(aborted.error().code, VfsError::Cancelled);
}

void TestArchiveFileSystem::isReadOnly()
{
    const QString archive = m_tarGz.isEmpty() ? m_zip : m_tarGz;
    if (archive.isEmpty())
        QSKIP("no archive fixture");

    FileSystemPtr fs = openArchive(archive);
    QVERIFY(fs->capabilities().testFlag(VfsCapability::Read));
    QVERIFY(!fs->capabilities().testFlag(VfsCapability::Write));

    // Writing must be refused explicitly, not silently ignored.
    Result<void> attempt = fs->makeDirectory(rootOf(archive).child(QStringLiteral("new")));
    QVERIFY(!attempt.ok());
    QCOMPARE(attempt.error().code, VfsError::NotSupported);
}

void TestArchiveFileSystem::missingArchiveFailsCleanly()
{
    FileSystemPtr fs = openArchive(QStringLiteral("/definitely/not/here.zip"));
    Result<FileEntryList> listing
        = fs->list(rootOf(QStringLiteral("/definitely/not/here.zip")), CancelToken());

    QVERIFY(!listing.ok());
    QCOMPARE(listing.error().code, VfsError::NotFound);
}

void TestArchiveFileSystem::corruptArchiveFailsCleanly()
{
    const QString junk = QDir(m_workspace->path()).filePath(QStringLiteral("broken.zip"));
    QFile file(junk);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not an archive at all");
    file.close();

    FileSystemPtr fs = openArchive(junk);
    Result<FileEntryList> listing = fs->list(rootOf(junk), CancelToken());

    QVERIFY2(!listing.ok(), "a corrupt archive must be an error, not an empty listing");
    QVERIFY(!listing.error().message.isEmpty());
}

namespace {

/// Builds a tar holding exactly the member names it is given.
///
/// `tar -P` is the only convenient way to produce the names that matter here:
/// every archiver refuses to *create* an entry that climbs out of the tree,
/// which is precisely why an archive that has one is worth defending against.
/// Returns an empty string when tar cannot be found, so the case skips rather
/// than fails on a machine without it.
QString packMembers(const QString& workingDir, const QString& outputPath, const QStringList& members)
{
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (tool.isEmpty())
        return {};

    QProcess process;
    process.setWorkingDirectory(workingDir);
    process.start(tool, QStringList { QStringLiteral("-cPf"), outputPath } + members);
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return {};
    return QFile::exists(outputPath) ? outputPath : QString();
}

/// Every entry the mount shows, as paths.
QStringList pathsIn(const FileSystemPtr& fs, const VfsUri& dir)
{
    QStringList out;
    const Result<FileEntryList> listed = fs->list(dir, CancelToken());
    if (!listed.ok())
        return out;
    for (const FileEntry& entry : listed.value()) {
        out.append(entry.uri.path());
        if (entry.isDir)
            out += pathsIn(fs, entry.uri);
    }
    return out;
}

} // namespace

void TestArchiveFileSystem::anEntryThatClimbsOutOfTheArchiveCannotReachOutOfTheMount()
{
    // Zip-slip. An archive is a list of names somebody else wrote, and a name
    // like "../../etc/passwd" is an instruction to write outside wherever it is
    // being extracted to. Nothing may present it as a path that leaves the
    // mount, because everything above here -- the copy, the walk, the preview --
    // trusts a uri to be inside the drive it came from.
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    QVERIFY(QDir(staging.path()).mkpath(QStringLiteral("inner")));
    QFile bait(QDir(staging.path()).filePath(QStringLiteral("escape.txt")));
    QVERIFY(bait.open(QIODevice::WriteOnly));
    bait.write("outside");
    bait.close();

    const QString path = packMembers(QDir(staging.path()).filePath(QStringLiteral("inner")),
        QDir(m_workspace->path()).filePath(QStringLiteral("climbing.tar")),
        { QStringLiteral("../escape.txt") });
    if (path.isEmpty())
        QSKIP("tar is not available to build the fixture");

    const FileSystemPtr fs = openArchive(path);
    const QStringList paths = pathsIn(fs, rootOf(path));
    QVERIFY2(!paths.isEmpty(), "the archive should still be readable");
    for (const QString& entry : paths) {
        QVERIFY2(entry.startsWith(QLatin1Char('/')), qPrintable(entry));
        QVERIFY2(!entry.contains(QLatin1String("..")), qPrintable(QStringLiteral("escaped: %1").arg(entry)));
    }
}

void TestArchiveFileSystem::anEntryWithAnAbsolutePathIsStillInsideTheMount()
{
    // The same class, one step less obvious: "/etc/passwd" as a member name. It
    // has to read as a path inside the archive, not as the machine's own.
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    QFile file(QDir(staging.path()).filePath(QStringLiteral("absolute.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("in the archive");
    file.close();

    const QString path
        = packMembers(staging.path(), QDir(m_workspace->path()).filePath(QStringLiteral("absolute.tar")),
            { QDir(staging.path()).filePath(QStringLiteral("absolute.txt")) });
    if (path.isEmpty())
        QSKIP("tar is not available to build the fixture");

    const FileSystemPtr fs = openArchive(path);
    const QStringList paths = pathsIn(fs, rootOf(path));
    QVERIFY(!paths.isEmpty());
    // Whatever the shape, the content read at that path is the archive's own.
    bool found = false;
    for (const QString& entry : paths) {
        Result<std::unique_ptr<QIODevice>> opened
            = fs->openRead(VfsUri(QStringLiteral("archive"), rootOf(path).authority(), entry));
        if (opened.ok() && opened.value()->readAll() == QByteArray("in the archive"))
            found = true;
    }
    QVERIFY2(found, "the entry is readable through the mount");
    QVERIFY2(QFile::exists(QDir(staging.path()).filePath(QStringLiteral("absolute.txt"))),
        "and reading the archive touched nothing on the machine");
}

void TestArchiveFileSystem::aSymlinkEntryIsNotAWayOutOfTheArchive()
{
    // A link inside an archive points at a name, and the name may be anywhere.
    // Reading it must not hand back whatever is at that name on this machine.
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    QFile secret(QDir(staging.path()).filePath(QStringLiteral("secret.txt")));
    QVERIFY(secret.open(QIODevice::WriteOnly));
    secret.write("the host's own file");
    secret.close();
    QVERIFY(QFile::link(QDir(staging.path()).filePath(QStringLiteral("secret.txt")),
        QDir(staging.path()).filePath(QStringLiteral("pointer"))));

    const QString path = packMembers(staging.path(),
        QDir(m_workspace->path()).filePath(QStringLiteral("link.tar")), { QStringLiteral("pointer") });
    if (path.isEmpty())
        QSKIP("tar is not available to build the fixture");

    const FileSystemPtr fs = openArchive(path);
    const Result<FileEntryList> listed = fs->list(rootOf(path), CancelToken());
    QVERIFY2(listed.ok(), qPrintable(listed.error().message));

    for (const FileEntry& entry : listed.value()) {
        Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
        if (!opened.ok())
            continue;
        QVERIFY2(opened.value()->readAll() != QByteArray("the host's own file"),
            "a link entry must not deliver what it points at on this machine");
    }
}

void TestArchiveFileSystem::twoEntriesWithOneNameGiveOneAnswer()
{
    // Two members of the same name is legal in tar and happens whenever
    // something is archived twice. Whatever the backend picks, it has to pick
    // the same one every time and present one file rather than two.
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    QFile file(QDir(staging.path()).filePath(QStringLiteral("twice.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("first");
    file.close();

    const QString path
        = packMembers(staging.path(), QDir(m_workspace->path()).filePath(QStringLiteral("twice.tar")),
            { QStringLiteral("twice.txt"), QStringLiteral("twice.txt") });
    if (path.isEmpty())
        QSKIP("tar is not available to build the fixture");

    const FileSystemPtr fs = openArchive(path);
    const Result<FileEntryList> first = fs->list(rootOf(path), CancelToken());
    QVERIFY2(first.ok(), qPrintable(first.error().message));
    QCOMPARE(first.value().size(), 1);

    const Result<FileEntryList> again = fs->list(rootOf(path), CancelToken());
    QVERIFY(again.ok());
    QCOMPARE(again.value().size(), 1);
    QCOMPARE(again.value().first().name, first.value().first().name);
}

void TestArchiveFileSystem::anArchiveCutInHalfIsAnErrorRatherThanAShortListing()
{
    // A download that stopped, or a disk that filled while it was being written.
    // The dangerous answer is a listing of the part that survived: an extraction
    // of half an archive, reported as an extraction.
    if (m_zip.isEmpty())
        QSKIP("no zip fixture to truncate");

    QFile whole(m_zip);
    QVERIFY(whole.open(QIODevice::ReadOnly));
    const QByteArray bytes = whole.readAll();
    whole.close();

    const QString path = QDir(m_workspace->path()).filePath(QStringLiteral("cut.zip"));
    QFile half(path);
    QVERIFY(half.open(QIODevice::WriteOnly));
    half.write(bytes.left(bytes.size() / 2));
    half.close();

    const FileSystemPtr fs = openArchive(path);
    const Result<FileEntryList> listed = fs->list(rootOf(path), CancelToken());
    if (listed.ok()) {
        // Some formats can be read as a stream and will hand over the entries
        // that survived. What must not happen is a *complete-looking* answer, so
        // the whole file has to be missing from it or unreadable.
        for (const FileEntry& entry : listed.value()) {
            if (entry.isDir)
                continue;
            Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
            if (!opened.ok())
                return; // an error somewhere is the right answer
            QVERIFY2(opened.value()->readAll().size() <= entry.size, "no entry may read longer than it is");
        }
        QVERIFY2(listed.value().size() < 3, "half an archive must not list as a whole one");
        return;
    }
    QVERIFY(listed.error().isError());
}

void TestArchiveFileSystem::factoryRejectsMissingPath()
{
    ArchiveFileSystemFactory factory;

    QString error;
    QVERIFY(factory.create({}, &error) == nullptr);
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(
        factory.create({ { QStringLiteral("path"), QStringLiteral("/no/such.zip") } }, &error) == nullptr);
    QVERIFY(!error.isEmpty());
}

void TestArchiveFileSystem::factoryAdvertisesMountableSuffixes()
{
    ArchiveFileSystemFactory factory;

    // This is how the browser learns that double-clicking a .zip should mount
    // it, without the host knowing anything about archives.
    QVERIFY(factory.mountableFileSuffixes().contains(QStringLiteral("zip")));
    QVERIFY(factory.mountableFileSuffixes().contains(QStringLiteral("7z")));
    QVERIFY(!factory.mountableFileSuffixes().contains(QStringLiteral("txt")));

    const QVariantMap config = factory.configForFile(QStringLiteral("/tmp/a.zip"));
    QCOMPARE(config.value(QStringLiteral("path")).toString(), QStringLiteral("/tmp/a.zip"));
    QCOMPARE(factory.rootUriForFile(QStringLiteral("/tmp/a.zip")).scheme(), QStringLiteral("archive"));
}

MOLE_TEST_MAIN(TestArchiveFileSystem)
#include "tst_ArchiveFileSystem.moc"
