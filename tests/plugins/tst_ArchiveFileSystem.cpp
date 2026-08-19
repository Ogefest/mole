#include "plugins/archive/ArchiveFileSystem.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <unistd.h>

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

/// Compresses one file into a single stream -- no container, no tar inside.
///
/// `gzip`, `xz` and `bzip2` all read standard input and write standard output
/// with `-c`, so the fixture is built here rather than committed. `extraArgs` is
/// how `gzip -n` gets asked for: with it, no original filename is stored.
QString compressFile(const QString& tool, const QByteArray& contents, const QString& outputPath,
    const QStringList& extraArgs = {})
{
    const QString found = QStandardPaths::findExecutable(tool);
    if (found.isEmpty())
        return {};

    QProcess process;
    process.setStandardOutputFile(outputPath);
    process.start(found, QStringList { QStringLiteral("-c") } + extraArgs);
    if (!process.waitForStarted(30000))
        return {};
    process.write(contents);
    process.closeWriteChannel();
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return {};
    return QFile::exists(outputPath) ? outputPath : QString();
}

/// The same, but with the original filename stored, which only gzip does and only
/// when it is the one doing the naming. `gzip notes.txt` writes `notes.txt.gz`
/// carrying `notes.txt` in its header; there is no way to ask for that through a
/// pipe, so the file has to exist under the name that is wanted.
QString gzipInPlace(const QByteArray& contents, const QString& directory, const QString& fileName)
{
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("gzip"));
    if (tool.isEmpty())
        return {};

    const QString plain = QDir(directory).filePath(fileName);
    QFile file(plain);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        return {};
    file.close();

    QProcess process;
    process.start(tool, { plain });
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return {};
    const QString compressed = plain + QStringLiteral(".gz");
    return QFile::exists(compressed) ? compressed : QString();
}

/// One large member, written to the compressor a megabyte at a time.
///
/// Never held whole in this process: the assertion about what opening a member
/// costs is made against resident memory, and a test that had just allocated the
/// payload itself would hand the allocator a hole of exactly the right size to
/// satisfy the fault it is looking for.
QString compressBigStream(const QString& tool, qint64 totalBytes, const QString& outputPath)
{
    const QString found = QStandardPaths::findExecutable(tool);
    if (found.isEmpty())
        return {};

    QProcess process;
    process.setStandardOutputFile(outputPath);
    process.start(found, { QStringLiteral("-c") });
    if (!process.waitForStarted(30000))
        return {};

    // Not zeros: a pattern, so a read that answers from the wrong offset answers
    // wrongly rather than plausibly. Still compresses to almost nothing.
    QByteArray block(1024 * 1024, Qt::Uninitialized);
    for (int i = 0; i < block.size(); ++i)
        block[i] = static_cast<char>('a' + (i % 26));

    qint64 written = 0;
    while (written < totalBytes) {
        const qint64 wanted = std::min<qint64>(block.size(), totalBytes - written);
        if (process.write(block.constData(), wanted) != wanted)
            return {};
        if (!process.waitForBytesWritten(30000))
            return {};
        written += wanted;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(60000) || process.exitCode() != 0)
        return {};
    return QFile::exists(outputPath) ? outputPath : QString();
}

/// The byte this process wrote at `offset` of that stream.
char patternByteAt(qint64 offset)
{
    return static_cast<char>('a' + ((offset % (1024 * 1024)) % 26));
}

/// Resident memory, in bytes, or -1 where the platform will not say.
qint64 residentBytes()
{
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (!statm.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
    if (fields.size() < 2)
        return -1;
    bool ok = false;
    const qint64 pages = fields.at(1).toLongLong(&ok);
    return ok ? pages * static_cast<qint64>(sysconf(_SC_PAGESIZE)) : -1;
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

    void aFileCompressedWithGzipAloneOpensAsADriveWithOneMember();
    void aGzipWithNoStoredNameIsNamedFromTheArchiveInstead();
    void xzAndBzip2MembersAreNamedFromTheArchive_data();
    void xzAndBzip2MembersAreNamedFromTheArchive();
    void aSingleStreamMemberNeverClaimsToBeNoughtBytes();
    void aFileNamedGzThatIsNotGzipIsStillRefused();
    void aPlainFileIsStillNotAnArchive();
    void aSevenZipIsUnaffected();
    void aGzippedFileAnotherFormatWouldBidForStillOpens();

    void openingAMemberOfATruncatedArchiveStillGivesItsFirstWindow();
    void theDamageInATruncatedMemberIsReportedByTheReadThatReachesIt();
    void openingAMemberDoesNotAllocateItsUncompressedSize();
    void aMemberIsReadableAtAnyOffsetAndNotOnlyFromTheStart();
    void aWholeMemberStillArrivesForAnExtraction();

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

// ---- a single compressed stream is an archive of one thing ---------------
//
// `notes.txt.gz` -- gzip and nothing else, no tar inside -- was offered as
// something to open as a drive and then could not be opened: libarchive's
// support_format_all() deliberately leaves out the `raw` format, and a gzip
// stream with no container in it is exactly what raw is for. The same held for a
// bare .xz, .bz2 and .zst, all four of which the factory advertises. See
// MOLE-216. What these assert is the fallback and, just as much, its three
// conditions -- because raw enabled without them turns "this is not an archive"
// into "an archive of one thing called data" for every file nothing recognises.

void TestArchiveFileSystem::aFileCompressedWithGzipAloneOpensAsADriveWithOneMember()
{
    const QByteArray payload("one stream, no container, and a line of text\n");
    const QString archive = gzipInPlace(payload, m_workspace->path(), QStringLiteral("notes.txt"));
    if (archive.isEmpty())
        QSKIP("gzip is not available");

    FileSystemPtr fs = openArchive(archive);
    Result<FileEntryList> listing = fs->list(rootOf(archive), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 1);

    // The name gzip stored in its header, which is why this is notes.txt and not
    // `data` -- the name libarchive gives a raw member with nothing to go on.
    const FileEntry member = listing.value().first();
    QCOMPARE(member.name, QStringLiteral("notes.txt"));
    QVERIFY(!member.isDir);

    Result<std::unique_ptr<QIODevice>> device = fs->openRead(member.uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));
    QCOMPARE(device.value()->readAll(), payload);
}

void TestArchiveFileSystem::aGzipWithNoStoredNameIsNamedFromTheArchiveInstead()
{
    // `gzip -n` stores no original filename, and a stream written by a library
    // usually does not either. libarchive then calls the member `data`, which
    // tells the user nothing the archive's own name does not already say.
    const QByteArray payload("no name in the header at all\n");
    const QString archive = compressFile(QStringLiteral("gzip"), payload,
        QDir(m_workspace->path()).filePath(QStringLiteral("anonymous.log.gz")), { QStringLiteral("-n") });
    if (archive.isEmpty())
        QSKIP("gzip is not available");

    FileSystemPtr fs = openArchive(archive);
    Result<FileEntryList> listing = fs->list(rootOf(archive), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 1);
    QCOMPARE(listing.value().first().name, QStringLiteral("anonymous.log"));

    Result<std::unique_ptr<QIODevice>> device = fs->openRead(listing.value().first().uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));
    QCOMPARE(device.value()->readAll(), payload);
}

void TestArchiveFileSystem::xzAndBzip2MembersAreNamedFromTheArchive_data()
{
    QTest::addColumn<QString>("tool");
    QTest::addColumn<QString>("archiveName");
    QTest::addColumn<QString>("memberName");

    // Neither format has a filename field at all, so the archive's own name is
    // the only thing there is to go on.
    QTest::newRow("xz") << QStringLiteral("xz") << QStringLiteral("report.txt.xz")
                        << QStringLiteral("report.txt");
    QTest::newRow("bzip2") << QStringLiteral("bzip2") << QStringLiteral("report.txt.bz2")
                           << QStringLiteral("report.txt");
    QTest::newRow("zstd") << QStringLiteral("zstd") << QStringLiteral("report.txt.zst")
                          << QStringLiteral("report.txt");
}

void TestArchiveFileSystem::xzAndBzip2MembersAreNamedFromTheArchive()
{
    QFETCH(QString, tool);
    QFETCH(QString, archiveName);
    QFETCH(QString, memberName);

    const QByteArray payload("compressed on its own, with no name inside\n");
    const QString archive = compressFile(tool, payload, QDir(m_workspace->path()).filePath(archiveName));
    if (archive.isEmpty())
        QSKIP("this compressor is not available");

    FileSystemPtr fs = openArchive(archive);
    Result<FileEntryList> listing = fs->list(rootOf(archive), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 1);
    QCOMPARE(listing.value().first().name, memberName);

    Result<std::unique_ptr<QIODevice>> device = fs->openRead(listing.value().first().uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));
    QCOMPARE(device.value()->readAll(), payload);
}

void TestArchiveFileSystem::aSingleStreamMemberNeverClaimsToBeNoughtBytes()
{
    // A compressed stream does not know its uncompressed length until it has been
    // read, and raw says so by leaving the size unset. Nought would be a claim,
    // and a wrong one about a member with a page of text in it.
    const QByteArray payload(4096, 'a');
    const QString archive = compressFile(
        QStringLiteral("gzip"), payload, QDir(m_workspace->path()).filePath(QStringLiteral("sized.txt.gz")));
    if (archive.isEmpty())
        QSKIP("gzip is not available");

    FileSystemPtr fs = openArchive(archive);
    Result<FileEntryList> listing = fs->list(rootOf(archive), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 1);
    QVERIFY2(listing.value().first().size != 0,
        "a member with contents was listed as nought bytes, which is a lie rather than an unknown");

    Result<FileEntry> stated = fs->stat(listing.value().first().uri);
    QVERIFY2(stated.ok(), qPrintable(stated.error().message));
    QVERIFY(stated.value().size != 0);
    // And what is actually there is the whole of it.
    Result<std::unique_ptr<QIODevice>> device = fs->openRead(listing.value().first().uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));
    QCOMPARE(device.value()->readAll().size(), payload.size());
}

void TestArchiveFileSystem::aFileNamedGzThatIsNotGzipIsStillRefused()
{
    // The filter test earning its place. With raw enabled and nothing having
    // decompressed anything, this file's own bytes would be offered as a member --
    // so a text file with a misleading name would open as an archive of itself.
    const QString liar = QDir(m_workspace->path()).filePath(QStringLiteral("pretending.gz"));
    QFile file(liar);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("gzip starts with 1f 8b and this does not");
    file.close();

    FileSystemPtr fs = openArchive(liar);
    Result<FileEntryList> listing = fs->list(rootOf(liar), CancelToken());
    QVERIFY2(!listing.ok(), "a file that is not compressed at all was opened as a compressed stream");
}

void TestArchiveFileSystem::aPlainFileIsStillNotAnArchive()
{
    // The suffix test earning its place, and the error path the browser depends
    // on: `canOpenAsDrive()` says no for a .txt, and if one is asked for anyway
    // the answer has to stay "this is not an archive" rather than becoming an
    // archive of one member called data.
    const QString plain = QDir(m_workspace->path()).filePath(QStringLiteral("ordinary.txt"));
    QFile file(plain);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("just a file");
    file.close();

    FileSystemPtr fs = openArchive(plain);
    Result<FileEntryList> listing = fs->list(rootOf(plain), CancelToken());
    QVERIFY2(!listing.ok(), "an ordinary text file was opened as an archive of itself");
}

void TestArchiveFileSystem::aSevenZipIsUnaffected()
{
    // A container the fallback must not come near: nothing here runs unless the
    // ordinary open has already failed, and this one does not fail.
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("7z"));
    if (tool.isEmpty())
        QSKIP("7z is not available");

    TempTree source;
    QVERIFY(source.isValid());
    QVERIFY(source.writeFile(QStringLiteral("inside.txt"), QByteArray("seven zip")));

    const QString archive = QDir(m_workspace->path()).filePath(QStringLiteral("fixture.7z"));
    QProcess process;
    process.setWorkingDirectory(source.path());
    process.start(tool, { QStringLiteral("a"), QStringLiteral("-bso0"), archive, QStringLiteral(".") });
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        QSKIP("7z could not build the fixture");

    FileSystemPtr fs = openArchive(archive);
    Result<FileEntryList> listing = fs->list(rootOf(archive), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 1);
    QCOMPARE(listing.value().first().name, QStringLiteral("inside.txt"));
    QCOMPARE(listing.value().first().size, qint64(9));
}

// ---- a member is decompressed as it is read, not all at once -------------
//
// openRead() used to append the whole member to a QByteArray and hand back a
// QBuffer over it, with no cap, so opening a 20 GB member asked for 20 GB before
// the caller saw a byte -- while HexPreviewController's own header promises "the
// file is never held, only the window being shown". MOLE-216 made it urgent
// rather than theoretical: a file compressed with gzip alone is typically a log
// or a dump, and those are the large ones. See MOLE-218.

void TestArchiveFileSystem::openingAMemberOfATruncatedArchiveStillGivesItsFirstWindow()
{
    // Laziness proved by the data rather than by a measurement. The archive is cut
    // short, so decompressing the whole member is impossible -- and reading the
    // first window works anyway, which it cannot if opening the member read to the
    // end. Before this, openRead() answered the truncation with an error and the
    // window nobody had asked for was never handed over.
    const QString whole = compressBigStream(QStringLiteral("gzip"), 4 * 1024 * 1024,
        QDir(m_workspace->path()).filePath(QStringLiteral("whole.log.gz")));
    if (whole.isEmpty())
        QSKIP("gzip is not available");

    QFile source(whole);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray bytes = source.readAll();
    source.close();
    QVERIFY(bytes.size() > 2048);

    const QString cut = QDir(m_workspace->path()).filePath(QStringLiteral("cut.log.gz"));
    QFile shortened(cut);
    QVERIFY(shortened.open(QIODevice::WriteOnly));
    shortened.write(bytes.left(bytes.size() - 512));
    shortened.close();

    FileSystemPtr fs = openArchive(cut);
    Result<FileEntryList> listing = fs->list(rootOf(cut), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 1);

    Result<std::unique_ptr<QIODevice>> device = fs->openRead(listing.value().first().uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));

    const QByteArray window = device.value()->read(64 * 1024);
    QCOMPARE(window.size(), 64 * 1024);
    QCOMPARE(window.at(0), patternByteAt(0));
    QCOMPARE(window.at(window.size() - 1), patternByteAt(window.size() - 1));
}

void TestArchiveFileSystem::theDamageInATruncatedMemberIsReportedByTheReadThatReachesIt()
{
    // The other half of the same change, and the half that could lose data: the
    // failure has moved from open() to the read that arrives at it, so a caller
    // reading a member to the end -- a copy, an extraction -- must still be told.
    // A short, clean stream would be a truncated file reported as a whole one.
    const QString whole = compressBigStream(QStringLiteral("gzip"), 4 * 1024 * 1024,
        QDir(m_workspace->path()).filePath(QStringLiteral("damaged-source.log.gz")));
    if (whole.isEmpty())
        QSKIP("gzip is not available");

    QFile source(whole);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray bytes = source.readAll();
    source.close();

    const QString cut = QDir(m_workspace->path()).filePath(QStringLiteral("damaged.log.gz"));
    QFile shortened(cut);
    QVERIFY(shortened.open(QIODevice::WriteOnly));
    shortened.write(bytes.left(bytes.size() / 2));
    shortened.close();

    FileSystemPtr fs = openArchive(cut);
    Result<FileEntryList> listing = fs->list(rootOf(cut), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));

    Result<std::unique_ptr<QIODevice>> device = fs->openRead(listing.value().first().uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));

    QIODevice& member = *device.value();
    bool refused = false;
    for (int reads = 0; reads < 4096; ++reads) {
        QByteArray chunk(64 * 1024, Qt::Uninitialized);
        const qint64 read = member.read(chunk.data(), chunk.size());
        if (read < 0) {
            refused = true;
            break;
        }
        if (read == 0)
            break;
    }
    QVERIFY2(refused, "a member cut in half read to a clean end instead of saying it was damaged");
    QVERIFY(!member.errorString().isEmpty());
}

void TestArchiveFileSystem::openingAMemberDoesNotAllocateItsUncompressedSize()
{
    if (residentBytes() < 0)
        QSKIP("this platform does not report resident memory");

    // Sixty-four megabytes: far above anything the allocator or the fixture
    // accounts for, and small enough that gzip builds it in about a second. It is
    // written to the compressor a megabyte at a time, so this process has never
    // held it and the allocator has no hole of the right size to hide the fault in.
    const qint64 memberBytes = 64 * 1024 * 1024;
    const QString archive = compressBigStream(QStringLiteral("gzip"), memberBytes,
        QDir(m_workspace->path()).filePath(QStringLiteral("big.log.gz")));
    if (archive.isEmpty())
        QSKIP("gzip is not available");

    FileSystemPtr fs = openArchive(archive);
    Result<FileEntryList> listing = fs->list(rootOf(archive), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 1);

    const qint64 before = residentBytes();
    Result<std::unique_ptr<QIODevice>> device = fs->openRead(listing.value().first().uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));
    const QByteArray window = device.value()->read(64 * 1024);
    const qint64 after = residentBytes();

    QCOMPARE(window.size(), 64 * 1024);
    // A generous bound, because the claim is not "a few kilobytes" but "not the
    // member": sixteen megabytes is a quarter of it and four times anything the
    // decompressor needs.
    const qint64 grew = after - before;
    QVERIFY2(grew < 16 * 1024 * 1024,
        qPrintable(QStringLiteral("opening a %1 MB member grew resident memory by %2 MB")
                       .arg(memberBytes / (1024 * 1024))
                       .arg(grew / (1024 * 1024))));
}

void TestArchiveFileSystem::aMemberIsReadableAtAnyOffsetAndNotOnlyFromTheStart()
{
    // The backend advertises RandomAccessRead, and the span loop that makes a
    // large file copyable rests on it: seek, read a stretch, seek again. A stream
    // format cannot be addressed by offset, so a seek forward decompresses and
    // discards and a seek backwards starts again -- slow, and correct, which is
    // the order those two matter in. The shape of the assertions is the one every
    // other backend is held to in FileSystemConformance.
    const qint64 memberBytes = 2 * 1024 * 1024;
    const QString archive = compressBigStream(QStringLiteral("gzip"), memberBytes,
        QDir(m_workspace->path()).filePath(QStringLiteral("ranged.log.gz")));
    if (archive.isEmpty())
        QSKIP("gzip is not available");

    FileSystemPtr fs = openArchive(archive);
    Result<FileEntryList> listing = fs->list(rootOf(archive), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));

    Result<std::unique_ptr<QIODevice>> device = fs->openRead(listing.value().first().uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));
    QIODevice& member = *device.value();

    QCOMPARE(member.read(4), QByteArrayLiteral("abcd"));

    // Forwards.
    QVERIFY2(member.seek(1000), "a backend advertising random access must be able to seek");
    QCOMPARE(member.read(3),
        QByteArray(1, patternByteAt(1000)) + QByteArray(1, patternByteAt(1001))
            + QByteArray(1, patternByteAt(1002)));

    // Backwards, which is the one a stream format has to work for.
    QVERIFY(member.seek(4));
    QCOMPARE(member.read(2), QByteArray(1, patternByteAt(4)) + QByteArray(1, patternByteAt(5)));

    // A long way forward, over many chunks.
    QVERIFY(member.seek(memberBytes - 3));
    QCOMPARE(member.read(100).size(), 3);

    // And past the end, where the honest answer is nothing at all.
    QVERIFY(member.seek(memberBytes));
    QCOMPARE(member.read(10), QByteArray());
}

void TestArchiveFileSystem::aWholeMemberStillArrivesForAnExtraction()
{
    // A caller that wants the whole member still gets the whole member: nothing
    // here is a cap, only a refusal to hold what has not been asked for. This is
    // an extraction in miniature, over many more chunks than one.
    const qint64 memberBytes = 3 * 1024 * 1024 + 777;
    const QString archive = compressBigStream(QStringLiteral("gzip"), memberBytes,
        QDir(m_workspace->path()).filePath(QStringLiteral("extracted.log.gz")));
    if (archive.isEmpty())
        QSKIP("gzip is not available");

    FileSystemPtr fs = openArchive(archive);
    Result<FileEntryList> listing = fs->list(rootOf(archive), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));

    Result<std::unique_ptr<QIODevice>> device = fs->openRead(listing.value().first().uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));

    const QByteArray everything = device.value()->readAll();
    QCOMPARE(everything.size(), memberBytes);
    QCOMPARE(everything.at(0), patternByteAt(0));
    QCOMPARE(everything.at(1024 * 1024 + 5), patternByteAt(1024 * 1024 + 5));
    QCOMPARE(everything.at(everything.size() - 1), patternByteAt(memberBytes - 1));
}

void TestArchiveFileSystem::aGzippedFileAnotherFormatWouldBidForStillOpens()
{
    // Rows of `word;word` -- a shape libarchive's mtree bidder claims. It found
    // both faults in the first version of this fallback, and neither was visible
    // with a gzipped line of prose:
    //
    // - libarchive accepted the file at `archive_read_open_filename` and only
    //   refused at the first header, so testing the open alone concluded a
    //   container had claimed it and the retry never ran.
    // - with `raw` enabled *alongside* the containers, mtree outbid raw -- raw's
    //   bid is the lowest there is -- won, and failed at the first header, so the
    //   file listed as an archive of nothing.
    //
    // Hence: the test is whether a first header can be read, and the retry asks
    // for raw on its own.
    const QByteArray payload("name;price\nwidget;1,50\nbolt;0,99\n");
    const QString archive = compressFile(
        QStringLiteral("gzip"), payload, QDir(m_workspace->path()).filePath(QStringLiteral("prices.csv.gz")));
    if (archive.isEmpty())
        QSKIP("gzip is not available");

    FileSystemPtr fs = openArchive(archive);
    Result<FileEntryList> listing = fs->list(rootOf(archive), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QVERIFY2(listing.value().size() == 1,
        qPrintable(QStringLiteral("listed %1 members").arg(listing.value().size())));
    QCOMPARE(listing.value().first().name, QStringLiteral("prices.csv"));

    Result<std::unique_ptr<QIODevice>> device = fs->openRead(listing.value().first().uri);
    QVERIFY2(device.ok(), qPrintable(device.error().message));
    QCOMPARE(device.value()->readAll(), payload);
}

MOLE_TEST_MAIN(TestArchiveFileSystem)
#include "tst_ArchiveFileSystem.moc"
