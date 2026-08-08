#include "support/MoleTestMain.h"

#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QSignalSpy>

using namespace mole;

class TestVfsManager : public QObject
{
    Q_OBJECT

private slots:
    void registersFactoriesByScheme();
    void addMountThroughFactory();
    void addMountRejectsUnknownScheme();
    void resolvesUriToItsBackend();
    void resolvesToTheMostSpecificMount();
    void unrelatedUriResolvesToNothing();
    void removeMountEmitsAndForgets();
    void mountIdsAreUnique();
};

namespace {

VfsManager* makeManager(QObject* parent)
{
    auto* manager = new VfsManager(parent);
    manager->registerFactory(std::make_unique<LocalFileSystemFactory>());
    manager->registerFactory(std::make_unique<MemoryFileSystemFactory>());
    return manager;
}

Mount memoryMount(const QString& authority, const QString& path)
{
    Mount mount;
    mount.displayName = QStringLiteral("scratch");
    mount.root = VfsUri(QStringLiteral("mem"), authority, path);
    mount.fileSystem = std::make_shared<MemoryFileSystem>();
    return mount;
}

} // namespace

void TestVfsManager::registersFactoriesByScheme()
{
    VfsManager manager;
    manager.registerFactory(std::make_unique<LocalFileSystemFactory>());

    QCOMPARE(manager.factories().size(), 1);
    QVERIFY(manager.factoryFor(QStringLiteral("file")) != nullptr);
    QVERIFY(manager.factoryFor(QStringLiteral("sftp")) == nullptr);
}

void TestVfsManager::addMountThroughFactory()
{
    VfsManager* manager = makeManager(this);
    QSignalSpy spy(manager, &VfsManager::mountsChanged);

    QString error;
    const QString id = manager->addMount(QStringLiteral("file"), QStringLiteral("Home"),
        { { QStringLiteral("rootPath"), QStringLiteral("/home") } }, &error);

    QVERIFY2(!id.isEmpty(), qPrintable(error));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(manager->mounts().size(), 1);
    QCOMPARE(manager->mount(id).displayName, QStringLiteral("Home"));
    QCOMPARE(manager->mount(id).root.path(), QStringLiteral("/home"));
}

void TestVfsManager::addMountRejectsUnknownScheme()
{
    VfsManager* manager = makeManager(this);

    QString error;
    const QString id = manager->addMount(QStringLiteral("ipfs"), QStringLiteral("Nope"), {}, &error);

    QVERIFY(id.isEmpty());
    QVERIFY2(!error.isEmpty(), "a rejected mount must explain itself");
    QVERIFY(manager->mounts().isEmpty());
}

void TestVfsManager::resolvesUriToItsBackend()
{
    VfsManager manager;
    Mount mount = memoryMount(QString(), QStringLiteral("/"));
    const QString id = manager.addMount(mount);
    QVERIFY(!id.isEmpty());

    FileSystemPtr fs = manager.resolve(VfsUri::fromString(QStringLiteral("mem:///a/b/c")));
    QVERIFY(fs != nullptr);
    QCOMPARE(fs->scheme(), QStringLiteral("mem"));
}

void TestVfsManager::resolvesToTheMostSpecificMount()
{
    VfsManager manager;
    Mount broad = memoryMount(QString(), QStringLiteral("/"));
    Mount narrow = memoryMount(QString(), QStringLiteral("/projects"));
    narrow.displayName = QStringLiteral("projects");

    manager.addMount(broad);
    manager.addMount(narrow);

    // Nesting one drive inside another must resolve to the inner one.
    const Mount hit = manager.mountForUri(VfsUri::fromString(QStringLiteral("mem:///projects/x")));
    QCOMPARE(hit.displayName, QStringLiteral("projects"));

    const Mount outside = manager.mountForUri(VfsUri::fromString(QStringLiteral("mem:///other")));
    QCOMPARE(outside.displayName, QStringLiteral("scratch"));
}

void TestVfsManager::unrelatedUriResolvesToNothing()
{
    VfsManager manager;
    manager.addMount(memoryMount(QString(), QStringLiteral("/")));

    QVERIFY(manager.resolve(VfsUri::fromString(QStringLiteral("s3://bucket/key"))) == nullptr);
    QVERIFY(!manager.mountForUri(VfsUri::fromString(QStringLiteral("s3://bucket/key"))).isValid());
}

void TestVfsManager::removeMountEmitsAndForgets()
{
    VfsManager manager;
    const QString id = manager.addMount(memoryMount(QString(), QStringLiteral("/")));

    QSignalSpy removed(&manager, &VfsManager::mountRemoved);
    manager.removeMount(id);

    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.first().first().toString(), id);
    QVERIFY(manager.mounts().isEmpty());
    QVERIFY(manager.resolve(VfsUri::fromString(QStringLiteral("mem:///a"))) == nullptr);

    // Removing twice is a no-op, not a crash.
    manager.removeMount(id);
    QCOMPARE(removed.count(), 1);
}

void TestVfsManager::mountIdsAreUnique()
{
    VfsManager manager;
    const QString first = manager.addMount(memoryMount(QString(), QStringLiteral("/a")));
    const QString second = manager.addMount(memoryMount(QString(), QStringLiteral("/b")));

    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(first != second);
}

MOLE_TEST_MAIN(TestVfsManager)
#include "tst_VfsManager.moc"
