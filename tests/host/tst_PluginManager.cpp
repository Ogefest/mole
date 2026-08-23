#include "host/ActionRegistry.h"
#include "host/FeatureRegistry.h"
#include "host/MetadataRegistry.h"
#include "host/PluginManager.h"
#include "host/PreviewRegistry.h"
#include "host/ThumbnailRegistry.h"
#include "support/FakePlugin.h"
#include "support/MoleTestMain.h"

#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QDir>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

class TestPluginManager : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void builtInPluginContributesEverything();
    void pluginSeesValidServices();
    void wrongApiVersionIsRejected();
    void duplicatePluginIdIsRejected();
    void duplicateSchemeIsRejectedWithNamedError();
    void duplicateFeatureIdIsRejectedWithNamedError();
    void duplicateMenuActionIsRejectedWithNamedError();
    void missingPluginDirectoryIsNotAnError();
    void theInstalledLayoutIsLookedForUnderBothLibAndLib64();
    void nonPluginFileIsReportedNotFatal();
    void shutdownIsCalledOnDestruction();
    void loadsTheRealArchivePluginFromDisk();

private:
    PluginManager* makeManager();

    std::unique_ptr<QTemporaryDir> m_dir;
    VfsManager* m_vfs = nullptr;
    TaskManager* m_tasks = nullptr;
    EventBus* m_events = nullptr;
    std::unique_ptr<IndexDatabase> m_index;
    FeatureRegistry* m_features = nullptr;
    PreviewRegistry* m_previews = nullptr;
    MetadataRegistry* m_metadata = nullptr;
    ThumbnailRegistry* m_thumbnails = nullptr;
    ActionRegistry* m_actions = nullptr;
    PluginServices m_services;
};

void TestPluginManager::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());

    m_vfs = new VfsManager(this);
    m_tasks = new TaskManager(this);
    m_events = new EventBus(this);
    m_features = new FeatureRegistry(this);
    m_previews = new PreviewRegistry(this);
    m_metadata = new MetadataRegistry(this);
    m_thumbnails = new ThumbnailRegistry(this);
    m_actions = new ActionRegistry(this);
    m_index = std::make_unique<IndexDatabase>(QDir(m_dir->path()).filePath(QStringLiteral("i.sqlite")));
    QVERIFY(m_index->open().ok());

    m_services = PluginServices { m_vfs, m_tasks, m_index.get(), m_events };
}

void TestPluginManager::cleanup()
{
    delete m_tasks;
    m_tasks = nullptr;
    delete m_vfs;
    m_vfs = nullptr;
    delete m_events;
    m_events = nullptr;
    delete m_features;
    m_features = nullptr;
    delete m_previews;
    m_previews = nullptr;
    delete m_metadata;
    m_metadata = nullptr;
    delete m_thumbnails;
    m_thumbnails = nullptr;
    delete m_actions;
    m_actions = nullptr;
    m_index.reset();
    m_dir.reset();
}

PluginManager* TestPluginManager::makeManager()
{
    PluginManager::Destinations destinations;
    destinations.vfs = m_vfs;
    destinations.features = m_features;
    destinations.previews = m_previews;
    destinations.metadata = m_metadata;
    destinations.thumbnails = m_thumbnails;
    destinations.actions = m_actions;
    return new PluginManager(m_services, destinations, this);
}

void TestPluginManager::builtInPluginContributesEverything()
{
    PluginManager* manager = makeManager();

    FakePlugin::Config config;
    config.featureIds = { QStringLiteral("a"), QStringLiteral("b") };
    config.schemes = { QStringLiteral("fake") };
    config.previewIds = { QStringLiteral("p1") };
    config.menuActionIds = { QStringLiteral("org.test.tool") };

    QVERIFY(manager->addBuiltIn(std::make_unique<FakePlugin>(config)));

    QCOMPARE(manager->loaded().size(), 1u);
    QVERIFY(manager->loaded().front().builtIn);
    QCOMPARE(m_features->rowCount(), 2);
    QVERIFY(m_vfs->factoryFor(QStringLiteral("fake")) != nullptr);
    QCOMPARE(m_previews->providers().size(), 1);
    // A plugin can put its own entry in the menu, which is how a new tool
    // becomes reachable without touching the shell.
    QVERIFY(m_actions->contains(QStringLiteral("org.test.tool")));
    QVERIFY2(manager->errors().isEmpty(), qPrintable(manager->errors().join(QLatin1Char('\n'))));
}

void TestPluginManager::pluginSeesValidServices()
{
    PluginManager* manager = makeManager();
    auto plugin = std::make_unique<FakePlugin>(FakePlugin::Config {});
    FakePlugin* raw = plugin.get();

    QVERIFY(manager->addBuiltIn(std::move(plugin)));
    // A plugin that cannot reach the task manager or the bus could only work
    // by blocking the UI thread, so this is part of the contract.
    QVERIFY2(raw->sawValidServices(), "plugins must receive fully wired services");
}

void TestPluginManager::wrongApiVersionIsRejected()
{
    PluginManager* manager = makeManager();

    FakePlugin::Config config;
    config.apiVersion = kPluginApiVersion + 1;
    config.featureIds = { QStringLiteral("from-the-future") };

    QVERIFY2(!manager->addBuiltIn(std::make_unique<FakePlugin>(config)),
        "a plugin built against another API version must not load");
    QCOMPARE(m_features->rowCount(), 0);
    QCOMPARE(manager->errors().size(), 1);
    QVERIFY(manager->errors().first().contains(QStringLiteral("plugin API")));
}

void TestPluginManager::duplicatePluginIdIsRejected()
{
    PluginManager* manager = makeManager();

    FakePlugin::Config first;
    first.id = QStringLiteral("same.id");
    first.featureIds = { QStringLiteral("one") };
    QVERIFY(manager->addBuiltIn(std::make_unique<FakePlugin>(first)));

    FakePlugin::Config second;
    second.id = QStringLiteral("same.id");
    second.featureIds = { QStringLiteral("two") };
    QVERIFY(!manager->addBuiltIn(std::make_unique<FakePlugin>(second)));

    QCOMPARE(manager->loaded().size(), 1u);
    QCOMPARE(m_features->rowCount(), 1);
}

void TestPluginManager::duplicateSchemeIsRejectedWithNamedError()
{
    PluginManager* manager = makeManager();

    FakePlugin::Config first;
    first.id = QStringLiteral("plugin.one");
    first.schemes = { QStringLiteral("git") };
    QVERIFY(manager->addBuiltIn(std::make_unique<FakePlugin>(first)));

    FakePlugin::Config second;
    second.id = QStringLiteral("plugin.two");
    second.schemes = { QStringLiteral("git") };
    // The second plugin still loads; only the clashing contribution is refused.
    QVERIFY(manager->addBuiltIn(std::make_unique<FakePlugin>(second)));

    QCOMPARE(manager->errors().size(), 1);
    QVERIFY2(manager->errors().first().contains(QStringLiteral("plugin.two")),
        "the error has to name which plugin caused it");
    QVERIFY(manager->errors().first().contains(QStringLiteral("git")));
}

void TestPluginManager::duplicateFeatureIdIsRejectedWithNamedError()
{
    PluginManager* manager = makeManager();

    FakePlugin::Config first;
    first.id = QStringLiteral("plugin.one");
    first.featureIds = { QStringLiteral("dup") };
    manager->addBuiltIn(std::make_unique<FakePlugin>(first));

    FakePlugin::Config second;
    second.id = QStringLiteral("plugin.two");
    second.featureIds = { QStringLiteral("dup") };
    manager->addBuiltIn(std::make_unique<FakePlugin>(second));

    QCOMPARE(m_features->rowCount(), 1);
    QCOMPARE(manager->errors().size(), 1);
    QVERIFY(manager->errors().first().contains(QStringLiteral("plugin.two")));
}

void TestPluginManager::duplicateMenuActionIsRejectedWithNamedError()
{
    PluginManager* manager = makeManager();

    FakePlugin::Config first;
    first.id = QStringLiteral("plugin.one");
    first.menuActionIds = { QStringLiteral("shared.entry") };
    manager->addBuiltIn(std::make_unique<FakePlugin>(first));

    FakePlugin::Config second;
    second.id = QStringLiteral("plugin.two");
    second.menuActionIds = { QStringLiteral("shared.entry") };
    manager->addBuiltIn(std::make_unique<FakePlugin>(second));

    QCOMPARE(manager->errors().size(), 1);
    QVERIFY2(manager->errors().first().contains(QStringLiteral("plugin.two")),
        "the error has to name which plugin caused it");
}

void TestPluginManager::theInstalledLayoutIsLookedForUnderBothLibAndLib64()
{
    // GNUInstallDirs gives CMAKE_INSTALL_LIBDIR as `lib64` on every RPM
    // distribution, so the install rules put the plugins in
    // <prefix>/lib64/mole/plugins there. With only `lib` searched, the .rpm and the
    // AppImage each shipped an archive plugin and a network plugin that nothing ever
    // looked for: no archive browsing and no sftp, ftp, s3, webdav, smb or nfs
    // drives, reported as nothing at all rather than as a failure -- a plugin that
    // is not found is not a plugin that failed. See MOLE-296.
    const QStringList paths = PluginManager::defaultSearchPaths();

    const QDir beside(QCoreApplication::applicationDirPath());
    for (const QString& libdir : { QStringLiteral("lib"), QStringLiteral("lib64") }) {
        const QString wanted
            = QDir::cleanPath(beside.absoluteFilePath(QStringLiteral("../%1/mole/plugins").arg(libdir)));
        QVERIFY2(paths.contains(wanted),
            qPrintable(QStringLiteral("%1 is not searched; it looks in: %2")
                           .arg(wanted, paths.join(QStringLiteral(", ")))));
    }

    // And next to the binary, which is what makes a build tree work uninstalled.
    QVERIFY(paths.contains(beside.filePath(QStringLiteral("plugins"))));
}

void TestPluginManager::missingPluginDirectoryIsNotAnError()
{
    PluginManager* manager = makeManager();
    QCOMPARE(manager->loadFromDirectory(QStringLiteral("/definitely/not/here")), 0);
    QVERIFY2(manager->errors().isEmpty(), "most installs will lack most plugin directories");
}

void TestPluginManager::nonPluginFileIsReportedNotFatal()
{
    // A stray shared library in the plugin directory must produce a message,
    // not a failure to start.
    const QString junk = QDir(m_dir->path()).filePath(QStringLiteral("libjunk.so"));
    QFile file(junk);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not a plugin");
    file.close();

    PluginManager* manager = makeManager();
    QCOMPARE(manager->loadFromDirectory(m_dir->path()), 0);
    QCOMPARE(manager->errors().size(), 1);
}

void TestPluginManager::shutdownIsCalledOnDestruction()
{
    // The flag lives in the test, not in the plugin: the manager owns the
    // plugin and destroys it, so reading anything off it afterwards would be a
    // use-after-free -- which is exactly what the sanitizer build caught.
    bool shutdownCalled = false;

    FakePlugin::Config config;
    config.shutdownFlag = &shutdownCalled;

    {
        PluginManager manager(m_services,
            PluginManager::Destinations {
                m_vfs, m_features, m_previews, m_metadata, m_thumbnails, m_actions });
        QVERIFY(manager.addBuiltIn(std::make_unique<FakePlugin>(config)));
        QVERIFY(!shutdownCalled);
    }

    QVERIFY2(shutdownCalled, "plugins must get a chance to release resources");
}

void TestPluginManager::loadsTheRealArchivePluginFromDisk()
{
    // End-to-end proof that the published API works across a shared-library
    // boundary, not just for statically linked built-ins.
    const QString pluginDir = QStringLiteral(MOLE_TEST_PLUGIN_DIR);
    if (!QDir(pluginDir).exists())
        QSKIP("archive plugin was not built (libarchive missing)");

    PluginManager* manager = makeManager();
    const int loaded = manager->loadFromDirectory(pluginDir);

    QVERIFY2(loaded >= 1,
        qPrintable(QStringLiteral("no plugin loaded from %1: %2")
                       .arg(pluginDir, manager->errors().join(QLatin1Char(' ')))));
    QVERIFY2(m_vfs->factoryFor(QStringLiteral("archive")) != nullptr,
        "the archive plugin must contribute its filesystem factory");
    QVERIFY(!manager->loaded().front().builtIn);
}

MOLE_TEST_MAIN(TestPluginManager)
#include "tst_PluginManager.moc"
