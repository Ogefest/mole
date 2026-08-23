#include "plugins/archive/ArchiveFileSystem.h"
#include "sdk/ScanReaders.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/VfsManager.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace mole;
using namespace mole::test;

namespace {

/// A real zip on disk, through whatever archiver the machine has. Empty when
/// there is none, and every case that needs one skips rather than asserting
/// on a file that was never written.
QString packZip(const QString& sourceDir, const QString& outputPath)
{
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("zip"));
    if (tool.isEmpty())
        return {};
    QProcess process;
    process.setWorkingDirectory(sourceDir);
    process.start(tool, { QStringLiteral("-qr"), outputPath, QStringLiteral(".") });
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return {};
    return QFile::exists(outputPath) ? outputPath : QString();
}

} // namespace

/// Which files a scan goes inside, asked of the function that decides it.
///
/// **tst_ScanTask scans into a container through a stub reader, so the real
/// suffix list was never consulted on this path.** That left the fourth caller of
/// mountableFileSuffixes() untested, and it is the one where a regression is
/// silent: an archive that stops being descended into does not fail anything, it
/// stops appearing in an index, and the number of files a search finds goes down
/// by an amount nobody has a figure for. The stub stays where it is -- it is
/// testing the scan rather than the wiring, and one test answering two questions
/// is worth less than two. See MOLE-305, and MOLE-301 for what added the eleven
/// suffixes that made this worth writing down.
class TestScanReaders : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void aFormatOnTheListIsDescendedInto();
    void aDocumentThatHappensToBeAZipIsOneRow();
    void aContainerInsideAContainerIsARowAndIsNotFollowed();
    void aLargeContainerOnARemoteDriveIsLeftAlone();
    void withNothingThatMountsAnythingThereIsNoReaderAtAll();

private:
    std::unique_ptr<QTemporaryDir> m_workspace;
    std::unique_ptr<VfsManager> m_vfs;
    PluginServices m_services;
    QString m_zip;

    /// A local root, which is what makes the reader the careless-but-cheap one.
    static VfsUri localRoot() { return VfsUri::fromLocalPath(QDir::tempPath()); }

    /// The same bytes under another name, which is all a .jar or a .docx is.
    QString copyOfTheZipCalled(const QString& name) const
    {
        const QString path = QDir(m_workspace->path()).filePath(name);
        QFile source(m_zip);
        return source.copy(path) ? path : QString();
    }

    FileEntry entryFor(const QString& localPath) const
    {
        FileEntry entry;
        entry.uri = VfsUri::fromLocalPath(localPath);
        entry.name = QFileInfo(localPath).fileName();
        entry.size = QFileInfo(localPath).size();
        return entry;
    }
};

void TestScanReaders::initTestCase()
{
    m_workspace = std::make_unique<QTemporaryDir>();
    QVERIFY(m_workspace->isValid());

    TempTree source;
    QVERIFY(source.isValid());
    QVERIFY(source.writeFile(QStringLiteral("readme.txt"), QByteArray("hello")));
    QVERIFY(source.makeDirs(QStringLiteral("nested")));
    QVERIFY(source.writeFile(QStringLiteral("nested/payload.bin"), QByteArray(32, 'x')));

    m_zip = packZip(source.path(), QDir(m_workspace->path()).filePath(QStringLiteral("fixture.zip")));
}

void TestScanReaders::init()
{
    // The real registry with the real factory in it: the point of this suite is
    // that the answer comes through supportedSuffixes() rather than through
    // anything a test wrote down.
    m_vfs = std::make_unique<VfsManager>();
    m_vfs->registerFactory(std::make_unique<ArchiveFileSystemFactory>());
    m_services = PluginServices {};
    m_services.vfs = m_vfs.get();
}

void TestScanReaders::aFormatOnTheListIsDescendedInto()
{
    if (m_zip.isEmpty())
        QSKIP("zip is not available to build the fixture");

    const QString jar = copyOfTheZipCalled(QStringLiteral("plugin.jar"));
    QVERIFY(!jar.isEmpty());

    auto reader = containerReaderFor(m_services, localRoot());
    QVERIFY2(reader, "a build with a factory that mounts files has a container reader");

    bool truncated = true;
    const QList<IndexedFile> rows = reader(entryFor(jar), &truncated);

    // Its members, as rows beside it -- and this is the case that fails if a
    // suffix leaves the list, which is the whole reason the suite exists.
    QCOMPARE(rows.size(), 3);
    QStringList names;
    for (const IndexedFile& row : rows)
        names.append(row.name);
    names.sort();
    QCOMPARE(names,
        QStringList(
            { QStringLiteral("nested"), QStringLiteral("payload.bin"), QStringLiteral("readme.txt") }));
    QVERIFY2(!truncated, "three members is not a container that had to be cut short");

    // Addressed as it really is: a member lives on the archive's own authority,
    // and a row rebuilt from the volume's scheme would put it loose on the disk.
    for (const IndexedFile& row : rows)
        QVERIFY2(row.uri.startsWith(QStringLiteral("archive://")), qPrintable(row.uri));
}

void TestScanReaders::aDocumentThatHappensToBeAZipIsOneRow()
{
    if (m_zip.isEmpty())
        QSKIP("zip is not available to build the fixture");

    // The other half of MOLE-301's decision, and the half easiest to undo by
    // accident: a .docx is a zip, and a scan must index it as the document it is
    // rather than scattering its parts through somebody's index.
    const QString document = copyOfTheZipCalled(QStringLiteral("report.docx"));
    QVERIFY(!document.isEmpty());

    auto reader = containerReaderFor(m_services, localRoot());
    QVERIFY(reader);
    QVERIFY2(reader(entryFor(document), nullptr).isEmpty(),
        "a document is a file, whatever its bytes happen to be");
}

void TestScanReaders::aContainerInsideAContainerIsARowAndIsNotFollowed()
{
    // The function's own comment says why: following one is an unbounded
    // recursion with a bad failure mode. A comment is not a check.
    auto reader = containerReaderFor(m_services, localRoot());
    QVERIFY(reader);

    FileEntry member;
    member.name = QStringLiteral("inner.zip");
    member.uri = VfsUri(QStringLiteral("archive"),
        ArchiveFileSystem::authorityFor(QDir(m_workspace->path()).filePath(QStringLiteral("outer.zip"))),
        QStringLiteral("/inner.zip"));
    member.size = 1024;

    QVERIFY2(reader(member, nullptr).isEmpty(), "a member that is itself a container is a row and no more");
}

void TestScanReaders::aLargeContainerOnARemoteDriveIsLeftAlone()
{
    if (m_zip.isEmpty())
        QSKIP("zip is not available to build the fixture");

    const QString jar = copyOfTheZipCalled(QStringLiteral("remote.jar"));
    QVERIFY(!jar.isEmpty());

    // Opening a container on a remote drive means fetching it whole, so above the
    // ceiling it is left alone. The reader is asked about the same file twice, and
    // the size on the row is what decides -- so this is the ceiling being
    // honoured rather than the file being unreadable.
    auto remote = containerReaderFor(
        m_services, VfsUri(QStringLiteral("sftp"), QStringLiteral("host"), QStringLiteral("/")));
    QVERIFY(remote);

    FileEntry large = entryFor(jar);
    large.size = 64 * 1024 * 1024;
    QVERIFY2(remote(large, nullptr).isEmpty(), "a large container on a remote drive is not fetched");

    FileEntry small = entryFor(jar);
    small.size = 1024;
    QVERIFY2(!remote(small, nullptr).isEmpty(), "a small one still is");
}

void TestScanReaders::withNothingThatMountsAnythingThereIsNoReaderAtAll()
{
    // What a build without the archive plugin looks like from here, and it has to
    // be no reader rather than a reader that answers nothing: the caller uses the
    // difference to decide whether to say anything about containers at all.
    VfsManager empty;
    PluginServices services;
    services.vfs = &empty;
    QVERIFY(!containerReaderFor(services, localRoot()));

    // And no registry at all, which is what a headless context can be.
    QVERIFY(!containerReaderFor(PluginServices {}, localRoot()));
}

MOLE_TEST_MAIN(TestScanReaders)

#include "tst_ScanReaders.moc"
