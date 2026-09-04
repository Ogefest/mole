#include "host/MetadataRegistry.h"
#include "host/PluginManager.h"
#include "host/PreviewRegistry.h"
#include "host/ThumbnailRegistry.h"
#include "support/FakePlugin.h"
#include "support/MoleTestMain.h"

#include <QColor>
#include <QDir>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The three winner-takes-most registries, and how a plugin path is read.
///
/// Written because none of it had a host-level test. `tst_PluginManager` covers
/// what happens *around* a plugin and the feature registry has its own suite;
/// the preview, metadata and thumbnail registries were reached only through the
/// application, where a duplicate id or two providers of equal priority is a
/// behaviour nobody asserts. Each of these is a rule the extension points make
/// to plugin authors -- "your id must be unique", "the highest priority wins",
/// "MOLE_PLUGIN_PATH takes a list" -- and a promise made to somebody outside the
/// project deserves a test. See MOLE-365.
class TestRegistries : public QObject
{
    Q_OBJECT

private slots:
    void aPreviewProviderIdIsTakenOnlyOnce();
    void aMetadataReaderIdIsTakenOnlyOnce();
    void aThumbnailerIdIsTakenOnlyOnce();

    void theHighestPriorityProviderWins();
    void twoProvidersOfEqualPriorityKeepTheOrderTheyArrivedIn();
    void everyMetadataReaderThatClaimsAFileIsAsked();
    void onlyTheWinningThumbnailerIsAsked();

    void thePluginPathTakesAListAndKeepsItsOrder();

private:
    static FileEntry entryFor(const QString& name);
};

FileEntry TestRegistries::entryFor(const QString& name)
{
    FileEntry entry;
    entry.name = name;
    entry.uri = VfsUri::fromString(QStringLiteral("mem:///") + name);
    return entry;
}

// ------------------------------------------------ an id is taken once

void TestRegistries::aPreviewProviderIdIsTakenOnlyOnce()
{
    PreviewRegistry registry;
    QVERIFY(registry.addProvider(
        std::make_unique<FakePreviewProvider>(QStringLiteral("test.viewer"), QString(), 10)));
    // Refused, and the one already there is untouched -- which is the half that
    // matters: a second plugin claiming an id must not replace the first.
    QVERIFY(!registry.addProvider(
        std::make_unique<FakePreviewProvider>(QStringLiteral("test.viewer"), QString(), 90)));
    QCOMPARE(registry.providers().size(), 1);
    QCOMPARE(registry.provider(QStringLiteral("test.viewer"))->priority(), 10);
}

void TestRegistries::aMetadataReaderIdIsTakenOnlyOnce()
{
    MetadataRegistry registry;
    QVERIFY(registry.addReader(
        std::make_unique<FakeMetadataReader>(QStringLiteral("test.reader"), QList<FileFact> {}, 10)));
    QVERIFY(!registry.addReader(
        std::make_unique<FakeMetadataReader>(QStringLiteral("test.reader"), QList<FileFact> {}, 90)));
    QCOMPARE(registry.readers().size(), 1);
    QCOMPARE(registry.reader(QStringLiteral("test.reader"))->priority(), 10);
}

void TestRegistries::aThumbnailerIdIsTakenOnlyOnce()
{
    ThumbnailRegistry registry;
    QVERIFY(registry.addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("test.tiles"), QColor(Qt::red), 10)));
    QVERIFY(!registry.addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("test.tiles"), QColor(Qt::blue), 90)));
    QCOMPARE(registry.thumbnailers().size(), 1);
    QCOMPARE(registry.thumbnailer(QStringLiteral("test.tiles"))->priority(), 10);
}

// --------------------------------------------- who is asked, and in what order

void TestRegistries::theHighestPriorityProviderWins()
{
    // The promise the extension point makes: a plugin can override a built-in
    // viewer by outranking it, and nothing else is needed.
    PreviewRegistry registry;
    QVERIFY(registry.addProvider(
        std::make_unique<FakePreviewProvider>(QStringLiteral("builtin"), QString(), 10)));
    QVERIFY(
        registry.addProvider(std::make_unique<FakePreviewProvider>(QStringLiteral("plugin"), QString(), 50)));

    const FileEntry entry = entryFor(QStringLiteral("notes.txt"));
    QCOMPARE(registry.providerFor(entry)->id(), QStringLiteral("plugin"));
    // And the rung below it, which is what a viewer that gives up falls to.
    QCOMPARE(registry.providerBelow(entry, registry.providerFor(entry))->id(), QStringLiteral("builtin"));
    QVERIFY(registry.providerBelow(entry, registry.provider(QStringLiteral("builtin"))) == nullptr);
}

void TestRegistries::twoProvidersOfEqualPriorityKeepTheOrderTheyArrivedIn()
{
    // Two plugins that both said 50 is an ordinary thing to happen, and the
    // answer has to be stable or which viewer opens a file depends on the order
    // a directory listing happened to hand out .so files.
    PreviewRegistry registry;
    QVERIFY(
        registry.addProvider(std::make_unique<FakePreviewProvider>(QStringLiteral("first"), QString(), 50)));
    QVERIFY(
        registry.addProvider(std::make_unique<FakePreviewProvider>(QStringLiteral("second"), QString(), 50)));

    const FileEntry entry = entryFor(QStringLiteral("notes.txt"));
    QCOMPARE(registry.providerFor(entry)->id(), QStringLiteral("first"));
    QCOMPARE(registry.providers().first()->id(), QStringLiteral("first"));
    QCOMPARE(registry.providers().last()->id(), QStringLiteral("second"));
}

void TestRegistries::everyMetadataReaderThatClaimsAFileIsAsked()
{
    // The one registry that is not winner-takes-all: a container and its
    // contents are two sets of facts about one file and both are right at once.
    // Priority decides the order the facts are shown in, not the winner.
    MetadataRegistry registry;
    QVERIFY(registry.addReader(
        std::make_unique<FakeMetadataReader>(QStringLiteral("low"), QList<FileFact> {}, 10)));
    QVERIFY(registry.addReader(
        std::make_unique<FakeMetadataReader>(QStringLiteral("high"), QList<FileFact> {}, 90)));

    const QList<IMetadataReader*> claiming = registry.readersFor(entryFor(QStringLiteral("notes.txt")));
    QCOMPARE(claiming.size(), 2);
    QCOMPARE(claiming.first()->id(), QStringLiteral("high"));
    QCOMPARE(claiming.last()->id(), QStringLiteral("low"));
}

void TestRegistries::onlyTheWinningThumbnailerIsAsked()
{
    // And the one that is: a file has one picture.
    ThumbnailRegistry registry;
    QVERIFY(registry.addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("low"), QColor(Qt::red), 10)));
    QVERIFY(registry.addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("high"), QColor(Qt::blue), 90)));

    QCOMPARE(registry.thumbnailerFor(entryFor(QStringLiteral("photo.jpg")))->id(), QStringLiteral("high"));
}

// ----------------------------------------------------- where plugins come from

void TestRegistries::thePluginPathTakesAListAndKeepsItsOrder()
{
    // The documented escape hatch, and it was never read by a test. It is a list
    // in the platform's own separator, and the order matters: the first copy of a
    // plugin id wins, so a developer putting a build tree in front of an
    // installed one has to get the build tree.
    const QString first = QDir::toNativeSeparators(QStringLiteral("/tmp/mole-first"));
    const QString second = QDir::toNativeSeparators(QStringLiteral("/tmp/mole-second"));
    qputenv("MOLE_PLUGIN_PATH", (first + QDir::listSeparator() + second).toLocal8Bit());
    const QStringList withPath = PluginManager::defaultSearchPaths();
    qunsetenv("MOLE_PLUGIN_PATH");

    QVERIFY2(withPath.contains(first), qPrintable(withPath.join(QLatin1Char(' '))));
    QVERIFY2(withPath.contains(second), qPrintable(withPath.join(QLatin1Char(' '))));
    QVERIFY2(withPath.indexOf(first) < withPath.indexOf(second), "the list lost its order");

    // An empty entry is not a directory, and asking about "" would be asking
    // about the working directory.
    qputenv("MOLE_PLUGIN_PATH", (QDir::listSeparator() + first + QDir::listSeparator()).toLocal8Bit());
    const QStringList withGaps = PluginManager::defaultSearchPaths();
    qunsetenv("MOLE_PLUGIN_PATH");
    QVERIFY(withGaps.contains(first));
    QVERIFY2(!withGaps.contains(QString()), "an empty entry became a search path");

    // And with nothing set, the built-in places and nothing else.
    const QStringList withoutPath = PluginManager::defaultSearchPaths();
    QVERIFY(!withoutPath.contains(first));
    QVERIFY(!withoutPath.isEmpty());
}

MOLE_TEST_MAIN(TestRegistries)
#include "tst_Registries.moc"
