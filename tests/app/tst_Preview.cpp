#include "host/PreviewRegistry.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/PreviewFeature.h"
#include "plugins/builtin/previews/PreviewProviders.h"
#include "plugins/builtin/previews/SyntaxHighlighter.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// Which viewer a file gets, and what the preview tab does around it.
class TestPreview : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // --- picking a viewer ---
    void picksAViewer_data();
    void picksAViewer();
    void everyFileGetsSomething();
    void tableOutranksTextForCsv();
    void directoriesGetNothing();
    void imageProviderOnlyClaimsWhatQtCanDecode();

    // --- syntax highlighting ---
    void recognisesHighlightableLanguages_data();
    void recognisesHighlightableLanguages();

    // --- the tab ---
    void loadsTextContent();
    void parsesCsvWithADetectedSeparator();
    void separatorCanBeOverridden();
    void reportsFactsForAnUnknownFile();
    void arrowsStepThroughFilesOnly();
    void survivesAFileThatVanished();
    void remembersItsFileAcrossRestart();

private:
    IPreviewProvider* providerFor(const QString& relativePath) const;
    PreviewTabController* openPreview(const QString& relativePath);

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
};

void TestPreview::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

void TestPreview::init()
{
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));

    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->writeFile(QStringLiteral("notes.txt"), QByteArray("hello preview")));
    QVERIFY(m_tree->writeFile(QStringLiteral("config.json"), QByteArray("{\"a\": 1}")));
    QVERIFY(m_tree->writeFile(
        QStringLiteral("prices.csv"), QByteArray("name;price;qty\nwidget;1,50;3\nbolt;0,99;10\n")));
    QVERIFY(m_tree->writeFile(QStringLiteral("data.tsv"), QByteArray("a\tb\n1\t2\n")));
    QVERIFY(m_tree->writeFile(QStringLiteral("mystery.bin"), QByteArray(64, '\x01')));
    QVERIFY(m_tree->makeDirs(QStringLiteral("subfolder")));

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));

    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));
}

void TestPreview::cleanup()
{
    m_app.reset();
    m_tree.reset();
}

IPreviewProvider* TestPreview::providerFor(const QString& relativePath) const
{
    FileEntry entry;
    entry.name = relativePath;
    entry.uri = m_tree->rootUri().child(relativePath);
    entry.size = 64;
    return m_app->previews()->providerFor(entry);
}

PreviewTabController* TestPreview::openPreview(const QString& relativePath)
{
    m_app->previewFile(m_tree->rootUri().child(relativePath).toString());
    return qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
}

// ------------------------------------------------------------ picking one

void TestPreview::picksAViewer_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("providerId");

    QTest::newRow("plain text") << "notes.txt" << "mole.preview.text";
    QTest::newRow("json") << "config.json" << "mole.preview.text";
    QTest::newRow("xml") << "doc.xml" << "mole.preview.text";
    QTest::newRow("source code") << "main.cpp" << "mole.preview.text";
    QTest::newRow("csv") << "prices.csv" << "mole.preview.table";
    QTest::newRow("tsv") << "data.tsv" << "mole.preview.table";
    QTest::newRow("png") << "photo.png" << "mole.preview.image";
    QTest::newRow("jpeg") << "photo.jpg" << "mole.preview.image";
    QTest::newRow("unknown binary") << "mystery.bin" << "mole.preview.fileinfo";
    // The shared MIME database knows COPYING is text even without a suffix,
    // and the text viewer is right to claim it.
    QTest::newRow("known name, no suffix") << "COPYING" << "mole.preview.text";
    QTest::newRow("unknown, no suffix") << "blob8842" << "mole.preview.fileinfo";
}

void TestPreview::picksAViewer()
{
    QFETCH(QString, fileName);
    QFETCH(QString, providerId);

    IPreviewProvider* provider = providerFor(fileName);
    QVERIFY2(provider, qPrintable(fileName));
    QCOMPARE(provider->id(), providerId);
}

void TestPreview::everyFileGetsSomething()
{
    // The fallback exists precisely so that "nothing happens" is never the
    // answer. Anything it cannot render, it describes.
    for (const QString& name : { QStringLiteral("a.weird-extension"), QStringLiteral("x"),
             QStringLiteral("archive.tar.zst"), QStringLiteral(".hidden") }) {
        QVERIFY2(providerFor(name) != nullptr, qPrintable(name));
    }
}

void TestPreview::tableOutranksTextForCsv()
{
    // Both accept .csv. Priority is what decides, and a grid beats raw text.
    TextPreviewProvider text(m_app->services());
    TablePreviewProvider table(m_app->services());

    FileEntry csv;
    csv.name = QStringLiteral("prices.csv");
    csv.uri = m_tree->rootUri().child(QStringLiteral("prices.csv"));

    QVERIFY(text.canPreview(csv));
    QVERIFY(table.canPreview(csv));
    QVERIFY2(table.priority() > text.priority(), "the grid must win for a .csv");
}

void TestPreview::directoriesGetNothing()
{
    FileEntry folder;
    folder.name = QStringLiteral("subfolder");
    folder.uri = m_tree->rootUri().child(QStringLiteral("subfolder"));
    folder.isDir = true;

    QVERIFY(m_app->previews()->providerFor(folder) == nullptr);
}

void TestPreview::imageProviderOnlyClaimsWhatQtCanDecode()
{
    // Claiming a format this build has no plugin for would show an empty frame
    // instead of the file's details, so the list comes from Qt itself.
    const QStringList supported = ImagePreviewProvider::imageSuffixes();
    QVERIFY(!supported.isEmpty());
    QVERIFY2(supported.contains(QStringLiteral("png")), "PNG is built into QtGui");

    ImagePreviewProvider provider(m_app->services());
    FileEntry exotic;
    exotic.name = QStringLiteral("scan.heic");
    exotic.uri = m_tree->rootUri().child(QStringLiteral("scan.heic"));
    QCOMPARE(provider.canPreview(exotic), supported.contains(QStringLiteral("heic")));
}

// ---------------------------------------------------------- highlighting

void TestPreview::recognisesHighlightableLanguages_data()
{
    QTest::addColumn<QString>("suffix");
    QTest::addColumn<bool>("highlighted");

    QTest::newRow("json") << "json" << true;
    QTest::newRow("JSON uppercase") << "JSON" << true;
    QTest::newRow("geojson") << "geojson" << true;
    QTest::newRow("xml") << "xml" << true;
    QTest::newRow("svg") << "svg" << true;
    QTest::newRow("html") << "html" << true;
    QTest::newRow("plain text") << "txt" << false;
    QTest::newRow("markdown") << "md" << false;
    QTest::newRow("nothing") << "" << false;
}

void TestPreview::recognisesHighlightableLanguages()
{
    QFETCH(QString, suffix);
    QFETCH(bool, highlighted);
    QCOMPARE(SourceHighlighter::isSupported(suffix), highlighted);
}

// -------------------------------------------------------------- the tab

void TestPreview::loadsTextContent()
{
    PreviewTabController* preview = openPreview(QStringLiteral("notes.txt"));
    QVERIFY(preview);
    QCOMPARE(preview->fileName(), QStringLiteral("notes.txt"));
    QCOMPARE(preview->viewerName(), QStringLiteral("Text"));

    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));
    QCOMPARE(viewer->text(), QStringLiteral("hello preview"));
    QVERIFY(!viewer->isHighlighted());
}

void TestPreview::parsesCsvWithADetectedSeparator()
{
    PreviewTabController* preview = openPreview(QStringLiteral("prices.csv"));
    QVERIFY(preview);

    auto* viewer = qobject_cast<TablePreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return viewer->table()->rowCount() > 0; }, 5000));

    // Semicolons separate, commas are decimal points. Getting this wrong would
    // give five columns of nonsense.
    QCOMPARE(viewer->separator(), QStringLiteral(";"));
    QCOMPARE(viewer->table()->columnCount(), 3);
    QCOMPARE(viewer->table()->rowCount(), 2);
    QCOMPARE(viewer->table()->headerAt(1), QStringLiteral("price"));
    QCOMPARE(viewer->table()->index(0, 1).data(TableModel::CellRole).toString(), QStringLiteral("1,50"));
}

void TestPreview::separatorCanBeOverridden()
{
    PreviewTabController* preview = openPreview(QStringLiteral("prices.csv"));
    auto* viewer = qobject_cast<TablePreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return viewer->table()->rowCount() > 0; }, 5000));

    // Detection is a guess; the user gets to correct it and see the result.
    // Changing it re-imports, which is asynchronous -- it has to be, since the
    // same path serves a file too large to hold.
    const auto reimported = [viewer](int columns) {
        return waitFor(
            [viewer, columns] { return !viewer->isImporting() && viewer->table()->columnCount() == columns; },
            5000);
    };

    // On this file a comma is wrong, and the grid shows that plainly: the
    // header becomes one column while "1,50" splits into two.
    viewer->setSeparator(QStringLiteral(","));
    QCOMPARE(viewer->separator(), QStringLiteral(","));
    QVERIFY(reimported(2));
    QCOMPARE(viewer->table()->headerAt(0), QStringLiteral("name;price;qty"));

    viewer->setSeparator(QStringLiteral(";"));
    QVERIFY(reimported(3));

    viewer->setFirstRowIsHeader(false);
    QVERIFY(waitFor([viewer] { return !viewer->isImporting() && viewer->table()->rowCount() == 3; }, 5000));
    // No header row means spreadsheet-style letters rather than blanks.
    QCOMPARE(viewer->table()->headerAt(0), QStringLiteral("A"));
}

void TestPreview::reportsFactsForAnUnknownFile()
{
    PreviewTabController* preview = openPreview(QStringLiteral("mystery.bin"));
    QVERIFY(preview);
    QCOMPARE(preview->viewerName(), QStringLiteral("File information"));

    auto* viewer = qobject_cast<FileInfoPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QCOMPARE(viewer->headline(), QStringLiteral("mystery.bin"));
    QVERIFY2(viewer->facts().size() >= 5, "an unknown file still has plenty to say about it");
}

void TestPreview::arrowsStepThroughFilesOnly()
{
    PreviewTabController* preview = openPreview(QStringLiteral("config.json"));
    QVERIFY(preview);
    QVERIFY(waitFor([preview] { return preview->siblingCount() > 0; }, 5000));

    // Five files, one directory. Stepping into a folder from a preview means
    // nothing, so folders are not in the list.
    QCOMPARE(preview->siblingCount(), 5);
    QCOMPARE(preview->position(), 1); // config.json sorts first

    preview->next();
    QCOMPARE(preview->fileName(), QStringLiteral("data.tsv"));
    preview->next();
    QCOMPARE(preview->fileName(), QStringLiteral("mystery.bin"));

    preview->previous();
    QCOMPARE(preview->fileName(), QStringLiteral("data.tsv"));

    // The viewer changes with the file, not just the name.
    QCOMPARE(preview->viewerName(), QStringLiteral("Table"));
}

void TestPreview::survivesAFileThatVanished()
{
    PreviewTabController* preview = openPreview(QStringLiteral("notes.txt"));
    QVERIFY(preview);
    QVERIFY(waitFor([preview] { return preview->siblingCount() > 0; }, 5000));

    QVERIFY(QFile::remove(m_tree->absolute(QStringLiteral("notes.txt"))));

    // Opening something that is no longer there must report, not crash.
    preview->open(m_tree->rootUri().child(QStringLiteral("gone.txt")).toString());
    QCOMPARE(preview->fileName(), QStringLiteral("gone.txt"));
    QVERIFY(preview->viewer() != nullptr); // the fallback still describes it
}

void TestPreview::remembersItsFileAcrossRestart()
{
    openPreview(QStringLiteral("config.json"));
    m_app->saveSessionNow();

    const QString expected = m_tree->rootUri().child(QStringLiteral("config.json")).toString();
    m_app.reset();
    drainEvents();

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));

    PreviewTabController* restored = nullptr;
    for (int row = 0; row < m_app->tabs()->rowCount(); ++row) {
        if (auto* candidate = qobject_cast<PreviewTabController*>(m_app->tabs()->controllerAt(row)))
            restored = candidate;
    }
    QVERIFY2(restored, "the preview tab must come back");
    QCOMPARE(restored->currentUri(), expected);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestPreview testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_Preview.moc"
