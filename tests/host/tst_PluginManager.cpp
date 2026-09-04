#include "host/ActionRegistry.h"
#include "host/FeatureRegistry.h"
#include "host/MetadataRegistry.h"
#include "host/PluginManager.h"
#include "host/PreviewRegistry.h"
#include "host/ThumbnailRegistry.h"
#include "support/FakePlugin.h"
#include "support/MoleTestMain.h"

#include "core/automation/Chain.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QDir>
#include <QFile>
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
    void aPluginContributesAChainStepKind();
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
    void aPluginLoadedFromDiskIsToldToShutDown();
    void aSymlinkedPluginLoads();
    void aPluginThatThrowsWhileRegisteringIsReportedAndSkipped();
    void aPluginThatThrowsWhenAskedWhatItIsIsReportedAndSkipped();
    void aHostWithNowhereToPutSomethingSaysSoWithoutCallingItAFault();
    void aPluginBuiltAgainstAnotherApiIsRefusedWithoutBeingAsked();
    void aPluginBuiltAgainstAShorterServicesReadsTheFieldsItKnows();

private:
    PluginManager* makeManager();
    /// One fixture plugin, copied into a directory of its own -- the loader takes
    /// a directory, so two fixtures in one place would be one case asking two
    /// questions. Empty when it was not built, which is a core-only build.
    QString fixtureAlone(const QString& name);

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
    ChainRegistry m_chains;
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
    m_services.chains = &m_chains;
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

QString TestPluginManager::fixtureAlone(const QString& name)
{
    const QDir built(QStringLiteral(MOLE_TEST_FIXTURE_PLUGIN_DIR));
    const QString library = built.filePath(QStringLiteral("libmole_test_plugin_%1.so").arg(name));
    if (!QFile::exists(library))
        return {};

    const QString alone = QDir(m_dir->path()).filePath(name);
    if (!QDir().mkpath(alone))
        return {};
    const QString copy = QDir(alone).filePath(QStringLiteral("libfixture.so"));
    if (QFile::exists(copy))
        QFile::remove(copy);
    if (!QFile::copy(library, copy))
        return {};
    return alone;
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

void TestPluginManager::aPluginContributesAChainStepKind()
{
    // Where this lives, and why it is not in tst_Chain.cpp with the rest of the
    // vocabulary: the claim is that a plugin's step kind is offered *exactly* like
    // a built-in one, and the only way to assert that rather than inspect it is to
    // send it through the manager -- which is here, with every other "a plugin
    // contributes" case. See MOLE-164, and tst_Chain for the vocabulary itself.
    PluginManager* manager = makeManager();

    FakePlugin::Config config;
    config.chainStepKinds = { { QStringLiteral("org.test.transcode"), StepRole::Transform },
        { QStringLiteral("org.test.publish"), StepRole::Sink } };
    QVERIFY(manager->addBuiltIn(std::make_unique<FakePlugin>(config)));

    QCOMPARE(m_chains.kinds().size(), 2);
    IChainStepKind* transcode = m_chains.kind(QStringLiteral("org.test.transcode"));
    QVERIFY(transcode != nullptr);
    QVERIFY(transcode->role() == StepRole::Transform);
    QCOMPARE(transcode->parameters().size(), 1);
    QVERIFY(transcode->parameters().first().required);

    // And it is a step like any other: a chain built from a plugin's kinds is
    // judged by the same rules, including the one about where a sink may sit.
    Chain chain;
    ChainStep first;
    first.kind = QStringLiteral("org.test.publish");
    ChainStep second;
    second.kind = QStringLiteral("org.test.transcode");
    chain.steps = { first, second };
    QString why;
    QVERIFY2(!m_chains.isRunnable(chain, &why), "a plugin's sink was allowed to sit first");
    QVERIFY2(why.contains(QStringLiteral("step 1 of 2")), qPrintable(why));
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

void TestPluginManager::aPluginLoadedFromDiskIsToldToShutDown()
{
    // shutdown() was called on built-ins only -- the destructor walked
    // m_ownedPlugins, which only addBuiltIn() writes -- so a plugin from a shared
    // library was never told to let go of anything, against what PluginApi.h
    // promises. The network plugin keeps libcurl handle pools and SMB serialises
    // behind a global Samba context; neither ever heard. See MOLE-365.
    //
    // Asserted through the real plugin on disk, because that is the path that had
    // no coverage. What can be observed from out here is that the plugin the
    // manager kept is the one it will call: the flag trick the built-in case uses
    // needs a plugin the test built.
    const QString pluginDir = QStringLiteral(MOLE_TEST_PLUGIN_DIR);
    if (!QDir(pluginDir).exists())
        QSKIP("no plugin was built to load from disk");

    PluginManager* manager = makeManager();
    QVERIFY(manager->loadFromDirectory(pluginDir) >= 1);
    for (const PluginManager::LoadedPlugin& plugin : manager->loaded()) {
        QVERIFY2(plugin.plugin != nullptr,
            qPrintable(
                QStringLiteral("%1 was loaded and cannot be told to shut down").arg(plugin.metadata.id)));
    }

    // And it survives being destroyed, which is where shutdown() now runs.
    delete manager;
}

void TestPluginManager::aSymlinkedPluginLoads()
{
    // QDir::NoSymLinks dropped every link before QLibrary::isLibrary was asked,
    // so a developer linking a build-tree .so into MOLE_PLUGIN_PATH -- the
    // documented escape hatch -- and a distribution installing a versioned .so
    // with an unversioned link beside it both got no plugin, no line in errors()
    // and nothing under --plugins. The worst kind of loader failure, one function
    // above the comment that says so. See MOLE-365.
    const QDir pluginDir(QStringLiteral(MOLE_TEST_PLUGIN_DIR));
    const QStringList libraries = pluginDir.entryList({ QStringLiteral("*.so") }, QDir::Files);
    if (libraries.isEmpty())
        QSKIP("no plugin was built to link to");

    const QString target = pluginDir.absoluteFilePath(libraries.first());
    const QString link = QDir(m_dir->path()).filePath(QStringLiteral("linked_plugin.so"));
    if (!QFile::link(target, link))
        QSKIP("this filesystem would not make a symbolic link");

    PluginManager* manager = makeManager();
    const int loaded = manager->loadFromDirectory(m_dir->path());
    QVERIFY2(loaded >= 1,
        qPrintable(QStringLiteral("a symlinked plugin was skipped in silence: %1")
                       .arg(manager->errors().join(QLatin1Char(' ')))));
    QCOMPARE(manager->loaded().front().filePath, link);

    // And a link to something that is not a library is still refused, by
    // isLibrary() rather than by dropping links -- so nothing is lost by looking.
    QFile note(QDir(m_dir->path()).filePath(QStringLiteral("notes.txt")));
    QVERIFY(note.open(QIODevice::WriteOnly));
    note.write("not a library");
    note.close();
    QVERIFY(QFile::link(note.fileName(), QDir(m_dir->path()).filePath(QStringLiteral("notes.link"))));
    PluginManager* second = makeManager();
    QCOMPARE(second->loadFromDirectory(m_dir->path()), 1);
}

void TestPluginManager::aPluginThatThrowsWhileRegisteringIsReportedAndSkipped()
{
    // There was no try anywhere in PluginManager.cpp, so an exception went
    // through acceptPlugin, loadFromDirectory, AppController::initialise and
    // main: one third-party plugin with a bug stopped Mole from opening at all.
    // Task::run() and ReadMetadataTask both catch what plugin code throws and
    // turn it into a reported failure; the loader was the one place that did not.
    PluginManager* manager = makeManager();

    FakePlugin::Config broken;
    broken.id = QStringLiteral("test.broken");
    broken.throwOnRegister = true;
    QVERIFY2(!manager->addBuiltIn(std::make_unique<FakePlugin>(broken)), "a plugin that threw was accepted");

    QVERIFY(manager->loaded().empty());
    QVERIFY2(manager->errors().join(QLatin1Char(' ')).contains(QStringLiteral("threw during registration")),
        qPrintable(manager->errors().join(QLatin1Char(' '))));
    QVERIFY(manager->errors().join(QLatin1Char(' ')).contains(QStringLiteral("test.broken")));

    // And the next plugin loads, which is the half that matters: one bad plugin
    // costs its own contributions and nobody else's.
    FakePlugin::Config sound;
    sound.id = QStringLiteral("test.sound");
    sound.featureIds = { QStringLiteral("works") };
    QVERIFY(manager->addBuiltIn(std::make_unique<FakePlugin>(sound)));
    QVERIFY(m_features->feature(QStringLiteral("works")) != nullptr);
}

void TestPluginManager::aPluginThatThrowsWhenAskedWhatItIsIsReportedAndSkipped()
{
    // metadata() is the first plugin code the loader runs, and it can throw too.
    PluginManager* manager = makeManager();

    FakePlugin::Config broken;
    broken.throwOnMetadata = true;
    QVERIFY(!manager->addBuiltIn(std::make_unique<FakePlugin>(broken)));
    QVERIFY(manager->loaded().empty());
    QVERIFY2(manager->errors()
                 .join(QLatin1Char(' '))
                 .contains(QStringLiteral("threw while being asked what it is")),
        qPrintable(manager->errors().join(QLatin1Char(' '))));
}

void TestPluginManager::aHostWithNowhereToPutSomethingSaysSoWithoutCallingItAFault()
{
    // mole-tasks wires only the drives, and every feature, preview, reader and
    // thumbnailer of every plugin it loaded produced "rejected a null feature" --
    // a fault the plugin did not have, printed when a --drive failed for an
    // unrelated reason. Two situations had one message.
    PluginManager::Destinations onlyDrives;
    onlyDrives.vfs = m_vfs;
    auto* manager = new PluginManager(m_services, onlyDrives, this);

    FakePlugin::Config config;
    config.id = QStringLiteral("test.contributes");
    config.schemes = { QStringLiteral("takeable") };
    config.featureIds = { QStringLiteral("nowhere") };
    config.previewIds = { QStringLiteral("nowhere.preview") };
    QVERIFY(manager->addBuiltIn(std::make_unique<FakePlugin>(config)));

    // The drive was taken; the rest had nowhere to go and that is not an error.
    QVERIFY(m_vfs->factoryFor(QStringLiteral("takeable")) != nullptr);
    QVERIFY2(manager->errors().isEmpty(), qPrintable(manager->errors().join(QLatin1Char(' '))));

    const QString said = manager->notes().join(QLatin1Char('\n'));
    QVERIFY2(said.contains(QStringLiteral("nowhere to put a feature")), qPrintable(said));
    QVERIFY2(said.contains(QStringLiteral("nowhere to put a preview provider")), qPrintable(said));
    QVERIFY2(!said.contains(QStringLiteral("null")), qPrintable(said));
}

void TestPluginManager::aPluginBuiltAgainstAnotherApiIsRefusedWithoutBeingAsked()
{
    // **The check that decided whether a plugin could be spoken to was made by
    // speaking to it.** `plugin->metadata()` is a virtual call through the
    // plugin's own vtable, returning a struct by value whose `apiVersion` was the
    // last field after five QStrings -- so a plugin built against another version
    // was asked a question in a shape it did not have, and had IPlugin ever gained
    // a virtual before `metadata()`, the call would have gone somewhere else
    // entirely. Neither of the two mechanisms built for this was doing anything:
    // the interface identifier said `/1.0` while the version went 8, 9, 10, 11,
    // and QPluginLoader::metaData() -- readable before anything in the library
    // runs -- was never consulted.
    //
    // The fixture declares `…Plugin/1` and aborts if it is asked what it is or
    // told to register, so **this case reaching its assertions at all is the
    // assertion**: a version read from the library's metadata means nothing in the
    // library ran. See MOLE-366 and ADR-0098.
    const QString alone = fixtureAlone(QStringLiteral("stale_api"));
    if (alone.isEmpty())
        QSKIP("the stale-API fixture plugin was not built");

    PluginManager* manager = makeManager();
    QCOMPARE(manager->loadFromDirectory(alone), 0);
    QVERIFY(manager->loaded().empty());

    const QString said = manager->errors().join(QLatin1Char('\n'));
    QVERIFY2(said.contains(QStringLiteral("built against plugin API 1,")), qPrintable(said));
    QVERIFY2(said.contains(QString::number(kPluginApiVersion)), qPrintable(said));
}

void TestPluginManager::aPluginBuiltAgainstAShorterServicesReadsTheFieldsItKnows()
{
    // PluginServices says fields may be appended without a version bump, and it
    // has been appended to twice since the version last moved. The fixture is
    // compiled against the struct as it was two appends ago -- twelve pointers
    // where the host has fourteen -- so it is in the position a plugin built
    // against an older SDK is in.
    //
    // It reports the address it read for every field it knows about, and this
    // compares each one with what the host was handed. Addresses rather than
    // which-ones-were-set, because a *reordering* leaves the same fields non-null
    // and moves them: only the values say so. That is what "append-only" has to
    // mean to be worth writing down, and what passing PluginServices to a
    // plugin-implemented virtual by reference rather than by value makes true
    // rather than accidental. See MOLE-366 and ADR-0098.
    const QString alone = fixtureAlone(QStringLiteral("short_services"));
    if (alone.isEmpty())
        QSKIP("the shorter-services fixture plugin was not built");

    PluginManager* manager = makeManager();
    QCOMPARE(manager->loadFromDirectory(alone), 1);

    const QString said = manager->errors().join(QLatin1Char('\n'));
    // The twelve fields the fixture's header knows about, in its order, with the
    // addresses this test handed the host.
    const QList<QPair<QString, const void*>> expected = {
        { QStringLiteral("vfs"), m_services.vfs },
        { QStringLiteral("tasks"), m_services.tasks },
        { QStringLiteral("index"), m_services.index },
        { QStringLiteral("events"), m_services.events },
        { QStringLiteral("previews"), m_services.previews },
        { QStringLiteral("metadata"), m_services.metadata },
        { QStringLiteral("thumbnails"), m_services.thumbnails },
        { QStringLiteral("scheduler"), m_services.scheduler },
        { QStringLiteral("alerts"), m_services.alerts },
        { QStringLiteral("reports"), m_services.reports },
        { QStringLiteral("sets"), m_services.sets },
        { QStringLiteral("preferences"), m_services.preferences },
    };
    for (const auto& field : expected) {
        const QString wanted = QStringLiteral("%1=%2").arg(
            field.first, QString::number(reinterpret_cast<quintptr>(field.second), 16));
        QVERIFY2(said.contains(wanted),
            qPrintable(QStringLiteral("expected %1, and it reported: %2").arg(wanted, said)));
    }

    // And it is a plugin the host accepted, not one it tolerated.
    QCOMPARE(manager->loaded().size(), 1);
    QCOMPARE(manager->loaded().front().metadata.id, QStringLiteral("test.short-services"));
}

MOLE_TEST_MAIN(TestPluginManager)
#include "tst_PluginManager.moc"
