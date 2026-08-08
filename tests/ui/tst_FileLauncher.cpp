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

MOLE_TEST_MAIN(TestFileLauncher)
#include "tst_FileLauncher.moc"
