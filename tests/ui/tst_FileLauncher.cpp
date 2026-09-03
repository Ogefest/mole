#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/FileLauncher.h"

#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// The launcher is the one place that hands work to the desktop, so every test
/// here replaces that final step with a recorder. Nothing actually launches.
class TestFileLauncher : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void localFileIsHandedStraightToTheDesktop();
    void remoteFileIsExtractedFirst();
    void extractedCopyKeepsItsNameAndContents();
    void unmountedFileReportsFailure();
    void invalidUriReportsFailure();
    void refusedByDesktopReportsFailure();
    void aStagedCopyIsAlwaysInsideTheScratchDirectory_data();
    void aStagedCopyIsAlwaysInsideTheScratchDirectory();
    void theSamePathOnTwoServersIsTwoStagedFiles();
    void aScratchDirectoryThatCannotHoldTheFileFailsRatherThanHandingOverHalfOfIt();

private:
    FileLauncher* makeLauncher();

    std::unique_ptr<QTemporaryDir> m_dir;
    VfsManager* m_vfs = nullptr;
    TaskManager* m_tasks = nullptr;
    EventBus* m_events = nullptr;
    std::unique_ptr<IndexDatabase> m_index;
    std::shared_ptr<MemoryFileSystem> m_mem;
    PluginServices m_services;

    QStringList m_opened;
    bool m_hookResult = true;
};

void TestFileLauncher::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());

    m_vfs = new VfsManager(this);
    m_tasks = new TaskManager(this);
    m_events = new EventBus(this);
    m_index = std::make_unique<IndexDatabase>(QDir(m_dir->path()).filePath(QStringLiteral("i.sqlite")));
    QVERIFY(m_index->open().ok());

    m_mem = std::make_shared<MemoryFileSystem>();
    m_mem->addFile(QStringLiteral("/docs/manual.txt"), QByteArray("inside the drive"));

    Mount mount;
    mount.displayName = QStringLiteral("scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = m_mem;
    m_vfs->addMount(mount);

    m_services = PluginServices { m_vfs, m_tasks, m_index.get(), m_events };
    m_opened.clear();
    m_hookResult = true;
}

void TestFileLauncher::cleanup()
{
    delete m_tasks;
    m_tasks = nullptr;
    delete m_vfs;
    m_vfs = nullptr;
    delete m_events;
    m_events = nullptr;
    m_index.reset();
    m_mem.reset();
    m_dir.reset();
}

FileLauncher* TestFileLauncher::makeLauncher()
{
    auto* launcher = new FileLauncher(m_services, this);
    launcher->setOpenHook([this](const QString& path) {
        m_opened.append(path);
        return m_hookResult;
    });
    return launcher;
}

void TestFileLauncher::localFileIsHandedStraightToTheDesktop()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("report.pdf"), QByteArray("%PDF")));

    FileLauncher* launcher = makeLauncher();
    QSignalSpy opened(launcher, &FileLauncher::opened);

    launcher->open(VfsUri::fromLocalPath(tree.absolute(QStringLiteral("report.pdf"))));

    // No copying, no task: the desktop can already reach this path.
    QCOMPARE(m_opened.size(), 1);
    QCOMPARE(m_opened.first(), tree.absolute(QStringLiteral("report.pdf")));
    QCOMPARE(opened.count(), 1);
}

void TestFileLauncher::remoteFileIsExtractedFirst()
{
    FileLauncher* launcher = makeLauncher();
    launcher->open(VfsUri::fromString(QStringLiteral("mem:///docs/manual.txt")));

    // A file inside an archive or on a NAS has no path the desktop can open,
    // so it has to be materialised somewhere real first.
    QVERIFY(waitFor([this] { return !m_opened.isEmpty(); }));
    QCOMPARE(m_opened.size(), 1);
    QVERIFY(QFile::exists(m_opened.first()));
}

void TestFileLauncher::extractedCopyKeepsItsNameAndContents()
{
    FileLauncher* launcher = makeLauncher();
    launcher->open(VfsUri::fromString(QStringLiteral("mem:///docs/manual.txt")));
    QVERIFY(waitFor([this] { return !m_opened.isEmpty(); }));

    const QString extracted = m_opened.first();
    // The name has to survive, or the desktop picks the wrong handler.
    QVERIFY2(extracted.endsWith(QStringLiteral("manual.txt")),
        qPrintable(QStringLiteral("unexpected scratch path: %1").arg(extracted)));

    QFile file(extracted);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("inside the drive"));
}

void TestFileLauncher::aStagedCopyIsAlwaysInsideTheScratchDirectory_data()
{
    QTest::addColumn<QString>("uri");
    QTest::addColumn<QString>("suffix");

    QTest::newRow("an ordinary remote path") << "mem:///docs/manual.txt" << "manual.txt";
    // The one that broke it. Stripping the leading slash left "C:/Users/..."
    // which QFileInfo calls absolute on Windows, so QDir::filePath() handed it
    // back unchanged and the scratch directory was never involved.
    QTest::newRow("a drive letter") << "file:///C:/Users/ann/notes.txt" << "notes.txt";
    QTest::newRow("a unc authority") << "file://server/share/report.pdf" << "report.pdf";
    // Normalised away before it ever reaches here, and asserted anyway: this is
    // the segment that would climb out if one ever survived.
    QTest::newRow("a dotdot segment") << "mem:///docs/../../../etc/passwd.txt" << "passwd.txt";
    QTest::newRow("a name the local disk may refuse") << "sftp://nas/home/really?.txt" << "?.txt";
    QTest::newRow("a colon in a name") << "sftp://nas/home/a:b.txt" << "b.txt";
}

void TestFileLauncher::aStagedCopyIsAlwaysInsideTheScratchDirectory()
{
    QFETCH(QString, uri);
    QFETCH(QString, suffix);

    FileLauncher* launcher = makeLauncher();
    const QString staged = launcher->scratchPathFor(VfsUri::fromString(uri));
    QVERIFY2(!staged.isEmpty(), qPrintable(uri));

    const QString root = QDir::cleanPath(QDir(launcher->scratchDirectory()).absolutePath());
    QVERIFY2(!root.isEmpty(), "there should be a scratch directory once something is staged");
    QVERIFY2(QDir::cleanPath(staged).startsWith(root + QLatin1Char('/')),
        qPrintable(QStringLiteral("%1 staged outside the scratch directory: %2 is not under %3")
                       .arg(uri, staged, root)));

    // The extension has to survive, since picking the handler by extension is
    // the reason the name is kept at all.
    QVERIFY2(staged.endsWith(suffix), qPrintable(QStringLiteral("%1 lost its name: %2").arg(uri, staged)));

    // And the directory it needs really exists, so the copy has somewhere to go.
    QVERIFY2(QFileInfo(staged).absoluteDir().exists(), qPrintable(staged));
}

void TestFileLauncher::theSamePathOnTwoServersIsTwoStagedFiles()
{
    // The reason the parent path is kept as a subdirectory at all is that two
    // files called readme.txt from different folders must not collide. The same
    // argument reaches one step further and the old construction did not: it
    // used only uri.path(), so /reports/2026.pdf on two different servers was
    // one staging path, and opening the second overwrote the first while it was
    // still being read.
    FileLauncher* launcher = makeLauncher();

    const QString first
        = launcher->scratchPathFor(VfsUri::fromString(QStringLiteral("sftp://alpha/reports/2026.pdf")));
    const QString second
        = launcher->scratchPathFor(VfsUri::fromString(QStringLiteral("sftp://beta/reports/2026.pdf")));

    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY2(first != second, qPrintable(QStringLiteral("two servers, one staging path: %1").arg(first)));
    QVERIFY(first.endsWith(QStringLiteral("2026.pdf")));
    QVERIFY(second.endsWith(QStringLiteral("2026.pdf")));
}

void TestFileLauncher::unmountedFileReportsFailure()
{
    FileLauncher* launcher = makeLauncher();
    QSignalSpy failed(launcher, &FileLauncher::failed);

    launcher->open(VfsUri::fromString(QStringLiteral("sftp://nowhere/file.txt")));

    QCOMPARE(failed.count(), 1);
    QVERIFY(m_opened.isEmpty());
}

void TestFileLauncher::invalidUriReportsFailure()
{
    FileLauncher* launcher = makeLauncher();
    QSignalSpy failed(launcher, &FileLauncher::failed);

    launcher->open(VfsUri());

    QCOMPARE(failed.count(), 1);
    QVERIFY(m_opened.isEmpty());
}

void TestFileLauncher::refusedByDesktopReportsFailure()
{
    // No registered application for the type is a normal outcome and has to
    // become a message rather than silence.
    m_hookResult = false;

    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("thing.unknown-ext")));

    FileLauncher* launcher = makeLauncher();
    QSignalSpy failed(launcher, &FileLauncher::failed);

    launcher->open(VfsUri::fromLocalPath(tree.absolute(QStringLiteral("thing.unknown-ext"))));

    QCOMPARE(failed.count(), 1);
    QVERIFY(!failed.first().at(1).toString().isEmpty());
}

/// A truncated copy is worse than a failure, because the program it is handed to
/// has no way to tell.
///
/// The read was on a worker "so that opening a 200 MB file out of a zip does not
/// stall the window"; the write of the same bytes was in the finished handler on
/// the thread that draws, and nothing looked at whether it worked. A scratch
/// directory on a full /tmp handed the desktop a file of the right name and the
/// wrong length. See MOLE-406.
///
/// A directory nothing may write into stands for the full disk here: the failure
/// is the same one and it can be arranged.
void TestFileLauncher::aScratchDirectoryThatCannotHoldTheFileFailsRatherThanHandingOverHalfOfIt()
{
    m_mem->addFile(QStringLiteral("/docs/second.txt"), QByteArray("also inside the drive"));

    FileLauncher* launcher = makeLauncher();
    // One that works, to find out where the scratch directory is.
    launcher->open(VfsUri::fromString(QStringLiteral("mem:///docs/manual.txt")));
    QVERIFY(waitFor([this] { return !m_opened.isEmpty(); }));
    const QString folder = QFileInfo(m_opened.first()).absolutePath();

    if (!QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner))
        QSKIP("this platform will not take a read-only directory");
    {
        // Root writes into a directory with no write bit, so there is nothing
        // to prove on an account that can.
        QFile probe(QDir(folder).filePath(QStringLiteral("probe")));
        if (probe.open(QIODevice::WriteOnly)) {
            probe.close();
            QFile::remove(probe.fileName());
            QFile::setPermissions(
                folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            QSKIP("this account can write into a directory with no write permission");
        }
    }

    QSignalSpy failures(launcher, &FileLauncher::failed);
    launcher->open(VfsUri::fromString(QStringLiteral("mem:///docs/second.txt")));
    QVERIFY(waitFor([&failures] { return !failures.isEmpty(); }));

    // Put back, so the temporary directory can still be removed.
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    // Nothing was handed to the desktop, and the failure says which file.
    QCOMPARE(m_opened.size(), 1);
    const QString reason = failures.first().at(1).toString();
    QVERIFY2(reason.contains(QStringLiteral("second.txt")), qPrintable(reason));
    QVERIFY2(!QFile::exists(QDir(folder).filePath(QStringLiteral("second.txt"))),
        "half a file was left under the name the desktop would have been given");
}

MOLE_TEST_MAIN(TestFileLauncher)
#include "tst_FileLauncher.moc"
