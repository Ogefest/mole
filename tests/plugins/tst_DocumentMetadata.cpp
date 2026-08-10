#include "plugins/builtin/previews/DocumentMetadata.h"
#include "plugins/builtin/previews/PdfPreview.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "support/ZipFixtures.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>

using namespace mole;
using namespace mole::test;

namespace {

QByteArray officeCore()
{
    return QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-"
        "properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
        "<dc:title>Quarterly report</dc:title>"
        "<dc:creator>Ada Lovelace</dc:creator>"
        "<cp:lastModifiedBy>Charles Babbage</cp:lastModifiedBy>"
        "<cp:revision>7</cp:revision>"
        "<dcterms:created xsi:type=\"dcterms:W3CDTF\">2026-03-14T09:31:00Z</dcterms:created>"
        "<dcterms:modified xsi:type=\"dcterms:W3CDTF\">2026-03-15T18:02:00Z</dcterms:modified>"
        "</cp:coreProperties>");
}

QByteArray officeApp()
{
    return QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
        "<Application>Microsoft Office Word</Application>"
        "<Company>Analytical Engines Ltd</Company>"
        "<Pages>12</Pages>"
        "<Words>3400</Words>"
        "<Characters>19000</Characters>"
        "<TotalTime>95</TotalTime>"
        "</Properties>");
}

QByteArray openDocumentMeta()
{
    return QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<office:meta>"
        "<meta:initial-creator>Grace Hopper</meta:initial-creator>"
        "<meta:creation-date>2026-01-08T11:00:00</meta:creation-date>"
        "<dc:title>Compiler notes</dc:title>"
        "<meta:generator>LibreOffice/24.2</meta:generator>"
        "<meta:document-statistic meta:page-count=\"4\" meta:word-count=\"812\" "
        "meta:character-count=\"5200\"/>"
        "</office:meta></office:document-meta>");
}

QString factNamed(const QList<FileFact>& facts, const QString& label)
{
    for (const FileFact& fact : facts) {
        if (fact.label == label)
            return fact.value;
    }
    return {};
}

} // namespace

/// Who wrote a document -- from the front of a container, and from a PDF.
class TestDocumentMetadata : public QObject
{
    Q_OBJECT

private slots:
    void anOfficeDocumentNamesItsAuthorAndCounts();
    void anOpenDocumentNamesTheOdfEquivalents();
    void aPlainZipContributesNothing();
    void propertiesPastThePrefixAreNotFetched();
    void anEntityNamingALocalFileIsNotResolved();
    void aPdfNamesItsTitleAuthorAndPages();
    void aPdfThatCannotBeReadSaysSoOnOneRow();
};

void TestDocumentMetadata::anOfficeDocumentNamesItsAuthorAndCounts()
{
    if (!DocumentMetadataReader::isAvailable())
        QSKIP("built without libarchive");

    StoredZip zip;
    zip.add("[Content_Types].xml", "<Types/>");
    zip.add("docProps/core.xml", officeCore());
    zip.add("docProps/app.xml", officeApp());
    zip.add("word/document.xml", QByteArray(4000, 'w'));

    const QList<FileFact> facts = DocumentMetadataReader::factsFor(zip.build());

    QCOMPARE(factNamed(facts, QStringLiteral("Author")), QStringLiteral("Ada Lovelace"));
    QCOMPARE(factNamed(facts, QStringLiteral("Last saved by")), QStringLiteral("Charles Babbage"));
    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Quarterly report"));
    QCOMPARE(factNamed(facts, QStringLiteral("Revision")), QStringLiteral("7"));
    QCOMPARE(factNamed(facts, QStringLiteral("Application")), QStringLiteral("Microsoft Office Word"));
    QCOMPARE(factNamed(facts, QStringLiteral("Company")), QStringLiteral("Analytical Engines Ltd"));
    QCOMPARE(factNamed(facts, QStringLiteral("Words")), QStringLiteral("3400"));
    QCOMPARE(factNamed(facts, QStringLiteral("Pages")), QStringLiteral("12"));

    // A date is said the way a reader says one, not as the document stores it.
    QVERIFY2(factNamed(facts, QStringLiteral("Created")).contains(QStringLiteral("2026")),
        qPrintable(factNamed(facts, QStringLiteral("Created"))));
    QVERIFY(!factNamed(facts, QStringLiteral("Created")).contains(QLatin1Char('T')));
    QVERIFY(!factNamed(facts, QStringLiteral("Modified")).isEmpty());
}

void TestDocumentMetadata::anOpenDocumentNamesTheOdfEquivalents()
{
    if (!DocumentMetadataReader::isAvailable())
        QSKIP("built without libarchive");

    StoredZip zip;
    zip.add("mimetype", "application/vnd.oasis.opendocument.text");
    zip.add("meta.xml", openDocumentMeta());
    zip.add("content.xml", QByteArray(2000, 'c'));

    const QList<FileFact> facts = DocumentMetadataReader::factsFor(zip.build());

    QCOMPARE(factNamed(facts, QStringLiteral("Author")), QStringLiteral("Grace Hopper"));
    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Compiler notes"));
    QCOMPARE(factNamed(facts, QStringLiteral("Application")), QStringLiteral("LibreOffice/24.2"));
    QCOMPARE(factNamed(facts, QStringLiteral("Pages")), QStringLiteral("4"));
    QCOMPARE(factNamed(facts, QStringLiteral("Words")), QStringLiteral("812"));
    QVERIFY(factNamed(facts, QStringLiteral("Created")).contains(QStringLiteral("2026")));
}

void TestDocumentMetadata::aPlainZipContributesNothing()
{
    if (!DocumentMetadataReader::isAvailable())
        QSKIP("built without libarchive");

    // The honest outcome for the case the reader cannot avoid claiming: OOXML,
    // ODF and a bag of holiday photographs are all zips to a magic rule.
    StoredZip zip;
    zip.add("holiday/one.txt", "not a document");
    zip.add("holiday/two.txt", "nor this");

    QVERIFY(DocumentMetadataReader::factsFor(zip.build()).isEmpty());
    QVERIFY(DocumentMetadataReader::factsFor(QByteArray("not even a zip")).isEmpty());
    QVERIFY(DocumentMetadataReader::factsFor(QByteArray()).isEmpty());
}

void TestDocumentMetadata::propertiesPastThePrefixAreNotFetched()
{
    if (!DocumentMetadataReader::isAvailable())
        QSKIP("built without libarchive");

    // A container with half a megabyte of body before its properties. Every
    // writer we know of puts them at the front; one that does not costs its
    // author's name rather than a hundred megabytes of transfer.
    StoredZip zip;
    zip.addFiller(DocumentMetadataReader::kPrefixBytes);
    zip.add("docProps/core.xml", officeCore());

    const QByteArray whole = zip.build();
    QVERIFY(whole.size() > DocumentMetadataReader::kPrefixBytes);

    const QList<FileFact> fromPrefix
        = DocumentMetadataReader::factsFor(whole.left(DocumentMetadataReader::kPrefixBytes));
    QVERIFY2(factNamed(fromPrefix, QStringLiteral("Author")).isEmpty(),
        "the prefix does not reach the properties, so there is nothing to report");

    // And from the whole file the same reader finds them, which is what proves
    // the container itself is fine and only the bound stopped it.
    QCOMPARE(factNamed(DocumentMetadataReader::factsFor(whole), QStringLiteral("Author")),
        QStringLiteral("Ada Lovelace"));
}

void TestDocumentMetadata::anEntityNamingALocalFileIsNotResolved()
{
    if (!DocumentMetadataReader::isAvailable())
        QSKIP("built without libarchive");

    // The XML equivalent of an <img> pointing at a remote host: an entity that
    // names a file on this machine. Previewing a document must not read it.
    TempTree tree;
    QVERIFY(tree.isValid());
    const QString secretPath = tree.absolute(QStringLiteral("secret.txt"));
    QVERIFY(tree.writeFile(QStringLiteral("secret.txt"), QByteArray("TOP-SECRET-CONTENTS")));

    QByteArray hostile = QByteArrayLiteral("<?xml version=\"1.0\"?>\n"
                                           "<!DOCTYPE coreProperties [<!ENTITY leak SYSTEM \"file://");
    hostile += secretPath.toUtf8();
    hostile += QByteArrayLiteral("\">]>\n"
                                 "<cp:coreProperties xmlns:cp=\"x\" xmlns:dc=\"y\">"
                                 "<dc:creator>&leak;</dc:creator>"
                                 "<dc:title>Harmless title</dc:title>"
                                 "</cp:coreProperties>");

    StoredZip zip;
    zip.add("docProps/core.xml", hostile);
    const QList<FileFact> facts = DocumentMetadataReader::factsFor(zip.build());

    for (const FileFact& fact : facts) {
        QVERIFY2(!fact.value.contains(QStringLiteral("TOP-SECRET-CONTENTS")),
            qPrintable(QStringLiteral("%1 = %2").arg(fact.label, fact.value)));
    }
    QVERIFY2(factNamed(facts, QStringLiteral("Author")).isEmpty(),
        "an unresolved entity costs its own row rather than leaking a file");
}

void TestDocumentMetadata::aPdfNamesItsTitleAuthorAndPages()
{
    if (!PdfPreviewProvider::isAvailable())
        QSKIP("built without Qt Pdf");

    QByteArray pdf;
    {
        QBuffer buffer(&pdf);
        buffer.open(QIODevice::WriteOnly);
        QPdfWriter writer(&buffer);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setTitle(QStringLiteral("Quarterly report"));
        writer.setCreator(QStringLiteral("Mole test suite"));

        QPainter painter(&writer);
        painter.drawText(QRect(0, 0, 4000, 800), Qt::AlignCenter, QStringLiteral("Page one"));
        writer.newPage();
        painter.drawText(QRect(0, 0, 4000, 800), Qt::AlignCenter, QStringLiteral("Page two"));
        painter.end();
    }

    const QList<FileFact> facts = PdfMetadataReader::factsForBytes(pdf);
    QCOMPARE(factNamed(facts, QStringLiteral("Title")), QStringLiteral("Quarterly report"));
    QCOMPARE(factNamed(facts, QStringLiteral("Created with")), QStringLiteral("Mole test suite"));
    QCOMPARE(factNamed(facts, QStringLiteral("Pages")), QStringLiteral("2"));

    // A4 to the nearest millimetre, which is how a page size is said.
    QCOMPARE(factNamed(facts, QStringLiteral("Page size")), QStringLiteral("210 × 297 mm"));
}

void TestDocumentMetadata::aPdfThatCannotBeReadSaysSoOnOneRow()
{
    if (!PdfPreviewProvider::isAvailable())
        QSKIP("built without Qt Pdf");

    // Eight empty rows would say the document has no title, no author and no
    // pages. One row says the true thing.
    const QList<FileFact> facts
        = PdfMetadataReader::factsForBytes(QByteArrayLiteral("%PDF-1.7\nthis is not a document"));
    QCOMPARE(facts.size(), 1);
    QCOMPARE(facts.first().label, QStringLiteral("Document"));
    QVERIFY2(facts.first().value.contains(QStringLiteral("cannot be read")), qPrintable(facts.first().value));
}

// QPdfWriter wants a font database, which wants a GUI application. Offscreen,
// like every other test here that draws anything.
int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestDocumentMetadata testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}
#include "tst_DocumentMetadata.moc"
