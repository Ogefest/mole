#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>

using namespace mole;
using namespace mole::test;

class TestLocalFileSystem : public QObject
{
    Q_OBJECT

private slots:
    /// The shared contract. If this passes, the backend behaves like every
    /// other backend as far as the rest of the application is concerned.
    void conformance();

    void reportsHiddenFiles();
    void listsRootWithoutCrashing();
    void factoryProducesUsableBackend();
};

void TestLocalFileSystem::conformance()
{
    TempTree tree;
    QVERIFY(tree.isValid());

    ConformanceContext context;
    context.fileSystem = std::make_shared<LocalFileSystem>();
    context.root = tree.rootUri();
    context.seedFile
        = [&tree](const QString& path, const QByteArray& data) { return tree.writeFile(path, data); };
    context.seedDir = [&tree](const QString& path) { return tree.makeDirs(path); };

    runFileSystemConformance(context);
}

void TestLocalFileSystem::reportsHiddenFiles()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral(".hidden")));
    QVERIFY(tree.writeFile(QStringLiteral("visible")));

    LocalFileSystem fs;
    Result<FileEntryList> listing = fs.list(tree.rootUri(), CancelToken());
    QVERIFY(listing.ok());
    QCOMPARE(listing.value().size(), 2);

    for (const FileEntry& entry : listing.value())
        QCOMPARE(entry.isHidden, entry.name.startsWith(QLatin1Char('.')));
}

void TestLocalFileSystem::listsRootWithoutCrashing()
{
    LocalFileSystem fs;
    Result<FileEntryList> listing = fs.list(VfsUri::fromLocalPath(QDir::rootPath()), CancelToken());
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QVERIFY(!listing.value().isEmpty());
}

void TestLocalFileSystem::factoryProducesUsableBackend()
{
    LocalFileSystemFactory factory;
    QCOMPARE(factory.scheme(), QStringLiteral("file"));

    QString error;
    FileSystemPtr fs = factory.create({}, &error);
    QVERIFY2(fs != nullptr, qPrintable(error));
    QCOMPARE(fs->scheme(), QStringLiteral("file"));
}

MOLE_TEST_MAIN(TestLocalFileSystem)
#include "tst_LocalFileSystem.moc"
