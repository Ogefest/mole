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
    const QString path = QStringLiteral("/home/lg/My Archives/backup 2026.zip");
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
