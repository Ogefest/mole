#include "host/PreviewRegistry.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/PreviewFeature.h"
#include "plugins/builtin/previews/MarkdownStyle.h"
#include "plugins/builtin/previews/PdfPreview.h"
#include "plugins/builtin/previews/PreviewProviders.h"
#include "plugins/builtin/previews/SyntaxHighlighter.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>

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
    void pdfProviderClaimsPdfsOnlyWhenItCanRenderThem();
    void pdfPreviewRendersPagesOnDemand();
    void htmlCanBeShownAsSourceOrAsAPage();
    void aRenderedPageReachesForNothingOffTheDisk();
    void theChoiceIsRememberedPerFileType();

    // --- syntax highlighting ---
    void recognisesHighlightableLanguages_data();
    void recognisesHighlightableLanguages();

    // --- markdown typography ---
    void markdownHeadingsGetRoomAndScale();
    void markdownProseIsNotSetSolid();
    void markdownCodeBlocksGetAReadableSlab();
    void markdownInlineCodeMatchesTheTextAroundIt();
    void markdownQuotesKeepTheirNesting();
    void markdownRulesAndTablesGetRoom();
    void markdownStylingIsIdempotent();
    void markdownStylingGivesThePageMoreRoomThanTheImporter();
    void markdownRestylingWaitsForTheImporterToFinish();
    void markdownRestylingIsDroppedWhenTheDocumentChanges();

    // --- the tab ---
    void loadsTextContent();
    void parsesCsvWithADetectedSeparator();
    void tableFillsWhileTheImportIsStillRunning();
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

void TestPreview::pdfProviderClaimsPdfsOnlyWhenItCanRenderThem()
{
    PdfPreviewProvider provider(m_app->services());

    FileEntry pdf;
    pdf.name = QStringLiteral("manual.pdf");
    pdf.uri = m_tree->rootUri().child(QStringLiteral("manual.pdf"));

    // Stated both ways round, so a build without Qt6::Pdf is a green build rather
    // than a skipped one: with the module the provider claims the file, without it
    // it claims nothing and the information viewer picks the file up instead.
    QCOMPARE(provider.canPreview(pdf), PdfPreviewProvider::isAvailable());

    if (!PdfPreviewProvider::isAvailable()) {
        QVERIFY(m_app->previews()->providerFor(pdf) != nullptr);
        return;
    }

    // And it outranks the text viewer, which would otherwise show a PDF as bytes.
    QVERIFY(provider.priority() > TextPreviewProvider(m_app->services()).priority());
    IPreviewProvider* chosen = m_app->previews()->providerFor(pdf);
    QVERIFY(chosen);
    QCOMPARE(chosen->id(), QStringLiteral("mole.preview.pdf"));

    FileEntry folder;
    folder.name = QStringLiteral("subfolder");
    folder.uri = m_tree->rootUri().child(QStringLiteral("subfolder"));
    folder.isDir = true;
    QVERIFY(!provider.canPreview(folder));
}

void TestPreview::pdfPreviewRendersPagesOnDemand()
{
    if (!PdfPreviewProvider::isAvailable())
        QSKIP("this build cannot render a PDF");

    // Written by the test rather than committed as a fixture: a binary blob in the
    // tree is one nobody can review, and QPdfWriter is right here.
    const QString path = QDir(m_tree->path()).filePath(QStringLiteral("manual.pdf"));
    {
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        QPainter painter(&writer);
        QVERIFY(painter.isActive());
        painter.drawText(QRect(0, 0, 2000, 400), Qt::AlignCenter, QStringLiteral("Page one"));
        QVERIFY(writer.newPage());
        painter.drawText(QRect(0, 0, 2000, 400), Qt::AlignCenter, QStringLiteral("Page two"));
    }
    QVERIFY(QFileInfo::exists(path));

    PreviewTabController* preview = openPreview(QStringLiteral("manual.pdf"));
    QVERIFY(preview);
    QCOMPARE(preview->viewerName(), QStringLiteral("Document"));

    auto* viewer = qobject_cast<PdfPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return viewer->pageCount() > 0; }, 10000));

    QCOMPARE(viewer->pageCount(), 2);
    QVERIFY2(viewer->errorText().isEmpty(), qPrintable(viewer->errorText()));
    QVERIFY2(viewer->positionText().contains(QStringLiteral("of 2")), qPrintable(viewer->positionText()));

    // A4 upright, so taller than it is wide. The delegate reserves its height from
    // this before any image exists, which is what stops the list jumping about.
    QVERIFY(viewer->pageAspect(0) > 1.0);

    // Rendered when asked for, not up front, and written where it can be read.
    const QString first = viewer->pageImage(0, 320);
    QVERIFY2(!first.isEmpty(), "the first page has to render");
    const QString firstPath = QUrl(first).toLocalFile();
    QVERIFY(QFileInfo::exists(firstPath));

    QImage rendered(firstPath);
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.width(), 320);
    QVERIFY2(rendered.height() > rendered.width(), "an A4 page comes out upright");

    // Not blank: a renderer that quietly produced white paper would pass every
    // assertion above.
    bool anyInk = false;
    for (int y = 0; y < rendered.height() && !anyInk; ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            if (qGray(rendered.pixel(x, y)) < 128) {
                anyInk = true;
                break;
            }
        }
    }
    QVERIFY2(anyInk, "the rendered page has something on it");

    // Asking twice at the same width reuses the file rather than rendering again.
    QCOMPARE(viewer->pageImage(0, 320), first);
    // A different width is a different file, because scaling a small render up
    // would just look soft.
    QVERIFY(viewer->pageImage(0, 480) != first);

    // The second page renders too, and only when it is asked for.
    QVERIFY(!viewer->pageImage(1, 320).isEmpty());
    // Past the end is nothing, not a crash.
    QVERIFY(viewer->pageImage(5, 320).isEmpty());
}

void TestPreview::htmlCanBeShownAsSourceOrAsAPage()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("page.html"),
        QByteArray("<html><body><h1>Title</h1><p>Some prose.</p></body></html>")));

    // The viewer says what can be chosen; nothing else in the shell knows that HTML
    // has a mode at all.
    TextPreviewProvider provider(m_app->services());
    FileEntry html;
    html.name = QStringLiteral("page.html");
    html.uri = m_tree->rootUri().child(QStringLiteral("page.html"));

    const QList<ViewerOption> options = provider.options(html);
    QCOMPARE(options.size(), 1);
    QCOMPARE(options.first().key, QStringLiteral("mode"));
    QCOMPARE(options.first().choices, QStringList({ QStringLiteral("Source"), QStringLiteral("Rendered") }));
    QVERIFY2(options.first().defaultChoice == QStringLiteral("Source"),
        "a file manager shows what is in a file until asked for something else");

    // And a file with nothing to choose offers nothing, rather than an empty picker.
    FileEntry log;
    log.name = QStringLiteral("notes.txt");
    log.uri = m_tree->rootUri().child(QStringLiteral("notes.txt"));
    QVERIFY(provider.options(log).isEmpty());

    PreviewTabController* preview = openPreview(QStringLiteral("page.html"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));

    // Source by default: what is shown is what is in the file, tags and all.
    QVERIFY(!viewer->isRenderedHtml());
    QVERIFY(viewer->text().contains(QStringLiteral("<h1>")));
    QCOMPARE(preview->viewerOptions().size(), 1);
    QCOMPARE(preview->viewerOptions().first().toMap().value(QStringLiteral("chosen")).toString(),
        QStringLiteral("Source"));

    // Choosing the page shows it as one, from the bytes already read rather than by
    // going back to the drive.
    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Rendered"));
    QVERIFY(viewer->isRenderedHtml());
    QCOMPARE(preview->viewerOptions().first().toMap().value(QStringLiteral("chosen")).toString(),
        QStringLiteral("Rendered"));

    // Back again, because a choice that cannot be undone is a trap.
    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Source"));
    QVERIFY(!viewer->isRenderedHtml());
    QVERIFY(viewer->text().contains(QStringLiteral("<h1>")));
}

void TestPreview::aRenderedPageReachesForNothingOffTheDisk()
{
    // The rule that matters most here: previewing a file must put nothing on the
    // network. Qt's rich text engine resolves what a document names, so a page could
    // otherwise tell whoever wrote it that a file had been looked at.
    const QString hostile
        = QStringLiteral("<html><head><link rel='stylesheet' href='http://example.invalid/x.css'>"
                         "<script src='https://example.invalid/x.js'></script></head>"
                         "<body onload='fetch(\"http://example.invalid/beacon\")'>"
                         "<h1>Heading</h1><img src='http://example.invalid/pixel.png' onerror='alert(1)'>"
                         "<iframe src='http://example.invalid/frame'></iframe>"
                         "<p>Text that must survive.</p></body></html>");

    const QString safe = TextPreviewController::withoutExternalReferences(hostile);

    QVERIFY2(!safe.contains(QStringLiteral("http"), Qt::CaseInsensitive),
        qPrintable(QStringLiteral("something could still be fetched: %1").arg(safe)));
    QVERIFY(!safe.contains(QStringLiteral("<img"), Qt::CaseInsensitive));
    QVERIFY(!safe.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
    QVERIFY(!safe.contains(QStringLiteral("<iframe"), Qt::CaseInsensitive));
    QVERIFY(!safe.contains(QStringLiteral("onload"), Qt::CaseInsensitive));
    QVERIFY(!safe.contains(QStringLiteral("onerror"), Qt::CaseInsensitive));

    // What the document actually says still gets shown -- this strips references,
    // not content.
    QVERIFY(safe.contains(QStringLiteral("Heading")));
    QVERIFY(safe.contains(QStringLiteral("Text that must survive.")));
    QVERIFY(safe.contains(QStringLiteral("<h1>")));
}

void TestPreview::theChoiceIsRememberedPerFileType()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("one.html"), QByteArray("<p>one</p>")));
    QVERIFY(m_tree->writeFile(QStringLiteral("two.html"), QByteArray("<p>two</p>")));
    QVERIFY(m_tree->writeFile(QStringLiteral("markup.xhtml"), QByteArray("<p>three</p>")));

    PreviewTabController* first = openPreview(QStringLiteral("one.html"));
    QVERIFY(first);
    first->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Rendered"));

    // The next .html opens the way the last one was left, which is the whole point
    // of remembering it.
    PreviewTabController* second = openPreview(QStringLiteral("two.html"));
    QVERIFY(second);
    QCOMPARE(second->viewerOptions().first().toMap().value(QStringLiteral("chosen")).toString(),
        QStringLiteral("Rendered"));
    auto* secondViewer = qobject_cast<TextPreviewController*>(second->viewer());
    QVERIFY(secondViewer);
    QVERIFY2(secondViewer->isRenderedHtml(), "and it is applied before the file is read, not after");

    // A different suffix is untouched: one text viewer serves several kinds of
    // markup, and choosing for one must not answer for the others.
    PreviewTabController* other = openPreview(QStringLiteral("markup.xhtml"));
    QVERIFY(other);
    QCOMPARE(other->viewerOptions().first().toMap().value(QStringLiteral("chosen")).toString(),
        QStringLiteral("Source"));
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

// ------------------------------------------------------ markdown typography

namespace {

MarkdownStyle::Metrics markdownMetrics()
{
    MarkdownStyle::Metrics metrics;
    // A round number, so a ratio that lands on .5 cannot make a test look like
    // it is asserting the wrong thing.
    metrics.bodyPixelSize = 16;
    return metrics;
}

/// Imports Markdown the way the viewer does and styles it. The text width is set
/// because half of what these tests check is what the layout does afterwards.
void loadStyledMarkdown(QTextDocument& document, const QString& markdown)
{
    QFont base;
    base.setPixelSize(markdownMetrics().bodyPixelSize);
    document.setDefaultFont(base);
    document.setMarkdown(markdown);
    document.setTextWidth(600);
    MarkdownStyle::applyTo(&document, markdownMetrics());
}

QTextBlock blockSaying(const QTextDocument& document, const QString& text)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (block.text() == text)
            return block;
    }
    return {};
}

/// The format of a block's first fragment, which is where a heading's size and a
/// quote's colour actually live.
QTextCharFormat firstFragmentFormat(const QTextBlock& block)
{
    for (QTextBlock::iterator it = block.begin(); it != block.end(); ++it) {
        if (it.fragment().isValid())
            return it.fragment().charFormat();
    }
    return {};
}

} // namespace

void TestPreview::markdownHeadingsGetRoomAndScale()
{
    QTextDocument document;
    loadStyledMarkdown(
        document, QStringLiteral("# Title\n\nOpening paragraph.\n\n## Section\n\nMore prose.\n"));

    const QTextBlock title = blockSaying(document, QStringLiteral("Title"));
    const QTextBlock section = blockSaying(document, QStringLiteral("Section"));
    const QTextBlock prose = blockSaying(document, QStringLiteral("Opening paragraph."));
    QVERIFY(title.isValid() && section.isValid() && prose.isValid());

    // What the importer leaves out entirely: a heading arrives with no space
    // above or below it, flush against the paragraph it belongs to.
    QVERIFY2(section.blockFormat().topMargin() > prose.blockFormat().bottomMargin(),
        "a heading needs more space above it than an ordinary paragraph break");
    QVERIFY2(section.blockFormat().topMargin() > section.blockFormat().bottomMargin(),
        "a heading belongs to the text under it, and spacing is what says so");
    // The first block has the view's own top padding above it already.
    QCOMPARE(title.blockFormat().topMargin(), 0.0);

    const int body = markdownMetrics().bodyPixelSize;
    QVERIFY(firstFragmentFormat(title).font().pixelSize() > firstFragmentFormat(section).font().pixelSize());
    QVERIFY(firstFragmentFormat(section).font().pixelSize() > body);
    QVERIFY2(firstFragmentFormat(title).fontWeight() >= QFont::Bold,
        "Qt leaves a level-one heading at normal weight, which reads as a mistake");
    QVERIFY2(!firstFragmentFormat(prose).hasProperty(QTextFormat::FontPixelSize),
        "prose keeps the size the view set, rather than one chosen here");
}

void TestPreview::markdownProseIsNotSetSolid()
{
    QTextDocument document;
    loadStyledMarkdown(document, QStringLiteral("First paragraph.\n\nSecond paragraph.\n"));

    const QTextBlock first = blockSaying(document, QStringLiteral("First paragraph."));
    QVERIFY(first.isValid());

    QCOMPARE(static_cast<int>(first.blockFormat().lineHeightType()),
        static_cast<int>(QTextBlockFormat::ProportionalHeight));
    QVERIFY2(first.blockFormat().lineHeight() >= 140, "prose set solid is what made this look cramped");
    // The importer's own paragraph spacing is six pixels, at any text size.
    QVERIFY2(first.blockFormat().bottomMargin() > 6.0, "a paragraph break has to be visible as one");
}

void TestPreview::markdownCodeBlocksGetAReadableSlab()
{
    QTextDocument document;
    loadStyledMarkdown(
        document, QStringLiteral("Before.\n\n```cpp\nint main()\n{\n    return 0;\n}\n```\n\nAfter.\n"));

    const QTextBlock opening = blockSaying(document, QStringLiteral("int main()"));
    const QTextBlock closing = blockSaying(document, QStringLiteral("}"));
    QVERIFY(opening.isValid() && closing.isValid());

    const MarkdownStyle::Metrics metrics = markdownMetrics();
    const QTextCharFormat code = firstFragmentFormat(opening);
    // Nine points is what the importer sets code to, whatever the body size is.
    QVERIFY2(
        !code.hasProperty(QTextFormat::FontPointSize), "the importer's point size has to go, or it wins");
    QVERIFY2(code.font().pixelSize() >= metrics.bodyPixelSize * 0.8,
        "code sized well under the prose around it reads as a mistake");
    QCOMPARE(code.fontFamilies().toStringList().value(0), metrics.monospaceFamily);

    // A fence arrives as one block per line, and the run is spaced as a whole:
    // margins between the lines would break the band into stripes.
    QCOMPARE(opening.blockFormat().background().color(), metrics.codeBackground);
    QCOMPARE(closing.blockFormat().background().color(), metrics.codeBackground);
    QVERIFY(opening.blockFormat().topMargin() > 0.0);
    QCOMPARE(opening.blockFormat().bottomMargin(), 0.0);
    QVERIFY(closing.blockFormat().bottomMargin() > 0.0);
    QVERIFY2(opening.blockFormat().leftMargin() > 0.0, "the slab is inset from the prose around it");
    // The slab is painted behind the glyphs, so leading between the lines would
    // fall outside it and cut the band into stripes.
    QVERIFY2(opening.blockFormat().lineHeight() <= 100, "code is set solid so its slab stays one band");
    QCOMPARE(firstFragmentFormat(opening).background().color(), metrics.codeBackground);
}

void TestPreview::markdownInlineCodeMatchesTheTextAroundIt()
{
    QTextDocument document;
    loadStyledMarkdown(
        document, QStringLiteral("Run `mole --help` to see the options.\n\n`only a code span`\n"));

    const QTextBlock prose = blockSaying(document, QStringLiteral("Run mole --help to see the options."));
    QVERIFY(prose.isValid());

    QTextCharFormat span;
    for (QTextBlock::iterator it = prose.begin(); it != prose.end(); ++it) {
        if (it.fragment().isValid() && it.fragment().text() == QStringLiteral("mole --help"))
            span = it.fragment().charFormat();
    }
    QVERIFY2(span.isValid(), "the code span has to be found for the rest of this to mean anything");
    QVERIFY(!span.hasProperty(QTextFormat::FontPointSize));
    QVERIFY2(span.font().pixelSize() >= markdownMetrics().bodyPixelSize * 0.8,
        "nine-point code in the middle of a sentence looks like a rendering fault");

    // A paragraph that is nothing but a code span is still a paragraph. Deciding
    // otherwise from the fragments would give it a slab of its own.
    const QTextBlock alone = blockSaying(document, QStringLiteral("only a code span"));
    QVERIFY(alone.isValid());
    QCOMPARE(alone.blockFormat().background().style(), Qt::NoBrush);
    QCOMPARE(alone.blockFormat().leftMargin(), 0.0);
}

void TestPreview::markdownQuotesKeepTheirNesting()
{
    QTextDocument document;
    loadStyledMarkdown(document, QStringLiteral("Prose.\n\n> quoted line\n>\n> > deeper still\n\nAfter.\n"));

    const QTextBlock quoted = blockSaying(document, QStringLiteral("quoted line"));
    const QTextBlock deeper = blockSaying(document, QStringLiteral("deeper still"));
    QVERIFY(quoted.isValid() && deeper.isValid());

    QVERIFY(quoted.blockFormat().leftMargin() > 0.0);
    QVERIFY2(deeper.blockFormat().leftMargin() > quoted.blockFormat().leftMargin(),
        "a quote inside a quote has to stay further in");
    // Nothing draws a border down the side of a block in Qt's rich text, so the
    // indent and a quieter colour are what mark a quote as one.
    QCOMPARE(firstFragmentFormat(quoted).foreground().color(), markdownMetrics().mutedText);
}

void TestPreview::markdownRulesAndTablesGetRoom()
{
    QTextDocument document;
    loadStyledMarkdown(
        document, QStringLiteral("Prose.\n\n---\n\n| name | price |\n|------|-------|\n| bolt | 0.99 |\n"));

    bool foundRule = false;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (!block.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth))
            continue;
        foundRule = true;
        QVERIFY2(block.blockFormat().topMargin() > 6.0, "a rule divides sections, so it needs room to do it");
        QVERIFY(block.blockFormat().bottomMargin() > 6.0);
    }
    QVERIFY2(foundRule, "the rule has to be found for the assertions above to have run");

    QTextTable* table = nullptr;
    const QList<QTextFrame*> frames = document.rootFrame()->childFrames();
    for (QTextFrame* frame : frames) {
        if (auto* candidate = qobject_cast<QTextTable*>(frame))
            table = candidate;
    }
    QVERIFY2(table, "the table has to be found for the assertions below to mean anything");
    QVERIFY2(table->format().cellPadding() > 0.0, "the importer leaves cell text touching the rules");
    QCOMPARE(table->format().cellSpacing(), 0.0);
}

void TestPreview::markdownStylingIsIdempotent()
{
    QTextDocument document;
    loadStyledMarkdown(document,
        QStringLiteral("# Title\n\nProse with `code`.\n\n## Section\n\n- one\n- two\n\n> quoted\n>\n"
                       "> > deeper\n\n```\ncode line\n```\n\n---\n\n| a | b |\n|---|---|\n| 1 | 2 |\n"));

    const auto snapshot = [&document] {
        QStringList out;
        for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
            const QTextBlockFormat format = block.blockFormat();
            out << QStringLiteral("%1/%2/%3/%4/%5/%6")
                       .arg(format.topMargin())
                       .arg(format.bottomMargin())
                       .arg(format.leftMargin())
                       .arg(format.lineHeight())
                       .arg(block.charFormat().font().pixelSize())
                       .arg(block.text());
        }
        return out;
    };

    const QStringList once = snapshot();
    // This runs again on every change to the document, including the changes it
    // makes itself, so a second pass has to be a no-op. Reading a quote's depth
    // back out of the margin that the first pass replaced is how that goes wrong.
    MarkdownStyle::applyTo(&document, markdownMetrics());
    QCOMPARE(snapshot(), once);
    MarkdownStyle::applyTo(&document, markdownMetrics());
    QCOMPARE(snapshot(), once);
}

void TestPreview::markdownStylingGivesThePageMoreRoomThanTheImporter()
{
    const QString markdown
        = QStringLiteral("# Title\n\nFirst paragraph.\n\n## Section\n\nSecond paragraph.\n");

    QFont base;
    base.setPixelSize(markdownMetrics().bodyPixelSize);

    QTextDocument bare;
    bare.setDefaultFont(base);
    bare.setMarkdown(markdown);
    bare.setTextWidth(600);

    QTextDocument styled;
    loadStyledMarkdown(styled, markdown);

    // The whole point, measured rather than asserted: the same blocks, given
    // room, take up more of the page than the importer gave them.
    QCOMPARE(styled.blockCount(), bare.blockCount());
    QVERIFY2(styled.size().height() > bare.size().height() * 1.2,
        "the styled page has to be noticeably taller than the one the importer produced");

    // And a title is now visibly a title rather than body text in bold.
    const QTextBlock title = blockSaying(styled, QStringLiteral("Title"));
    const QTextBlock prose = blockSaying(styled, QStringLiteral("First paragraph."));
    QVERIFY(title.isValid() && prose.isValid());
    QVERIFY(styled.documentLayout()->blockBoundingRect(title).height()
        > styled.documentLayout()->blockBoundingRect(prose).height());
}

namespace {

/// Enough of everything that the importer builds it in a great many separate
/// insertions -- headings, prose, a fence, a quote, a list and a table -- which
/// is what turns "styles too early" from a wasted pass into a crash.
QString markdownWithSomethingOfEverything(int sections)
{
    QString out;
    for (int i = 0; i < sections; ++i) {
        out += QStringLiteral("# Heading %1\n\nProse with `inline code` and **bold** in it.\n\n").arg(i);
        out += QStringLiteral("```\nfenced line one\nfenced line two\n```\n\n");
        out += QStringLiteral("> quoted line\n>\n> second quoted line\n\n");
        out += QStringLiteral("- item one\n- item two\n\n");
        out += QStringLiteral("| left | right |\n|---|---|\n| 1 | 2 |\n\n");
    }
    return out;
}

/// Whether the styling has been over this block. The importer leaves the line
/// height alone, and every branch of applyTo() sets one, so this says
/// "styled or not" without depending on any particular measurement.
bool looksStyled(const QTextBlock& block)
{
    return block.isValid() && block.blockFormat().lineHeight() > 0.0;
}

} // namespace

/// The preview used to segfault on opening a Markdown file. `contentsChanged`
/// is emitted by QTextDocumentPrivate::finishEdit(), and QTextMarkdownImporter
/// makes an edit for every piece of text it parses -- so restyling on that
/// signal walked blocks and fragments of a document that was still being built,
/// and died dereferencing one that was not there yet.
///
/// It was a performance fault too: the whole styling pass ran once per
/// insertion, so previewing a file cost the document walk multiplied by the
/// number of edits the import took.
void TestPreview::markdownRestylingWaitsForTheImporterToFinish()
{
    QTextDocument document;
    QFont base;
    base.setPixelSize(markdownMetrics().bodyPixelSize);
    document.setDefaultFont(base);

    MarkdownStyle style;
    style.setMetrics(markdownMetrics());
    style.attachTo(&document);

    document.setMarkdown(markdownWithSomethingOfEverything(40));
    document.setTextWidth(600);

    // Nothing yet: the pass is waiting for the loop to turn, which is the whole
    // fix. Reaching the assertion at all is most of the test -- without it the
    // process is gone by here.
    QVERIFY2(!looksStyled(blockSaying(document, QStringLiteral("Heading 3"))),
        "restyling during the import is what the crash was");

    int passes = 0;
    const QMetaObject::Connection counter
        = QObject::connect(&document, &QTextDocument::contentsChanged, [&passes] { ++passes; });
    QCoreApplication::processEvents();
    QObject::disconnect(counter);

    // One pass, however many edits the import took: the queued restyle
    // coalesces, so a document of a thousand blocks is walked once.
    QCOMPARE(passes, 1);
    QVERIFY2(looksStyled(blockSaying(document, QStringLiteral("Heading 3"))),
        "the styling still has to happen, just not during the import");
}

/// A queued pass outlives the document it was asked for. The viewer moves the
/// styling from one file's document to the next one's -- and off it entirely
/// when the next file is not Markdown -- so a pass left over from the last file
/// must not land afterwards.
void TestPreview::markdownRestylingIsDroppedWhenTheDocumentChanges()
{
    QFont base;
    base.setPixelSize(markdownMetrics().bodyPixelSize);

    QTextDocument first;
    first.setDefaultFont(base);
    QTextDocument second;
    second.setDefaultFont(base);

    MarkdownStyle style;
    style.setMetrics(markdownMetrics());
    style.attachTo(&first);

    first.setMarkdown(QStringLiteral("# One\n\nProse.\n"));
    // The next file arrives before the loop turns, which is exactly what
    // happens when somebody holds a cursor key down in the file list.
    style.attachTo(&second);
    second.setMarkdown(QStringLiteral("# Two\n\nProse.\n"));

    int firstChanged = 0;
    const QMetaObject::Connection counter
        = QObject::connect(&first, &QTextDocument::contentsChanged, [&firstChanged] { ++firstChanged; });
    QCoreApplication::processEvents();
    QObject::disconnect(counter);

    QCOMPARE(firstChanged, 0);
    QVERIFY2(looksStyled(blockSaying(second, QStringLiteral("Two"))), "the document in front of the reader");
    QVERIFY2(!looksStyled(blockSaying(first, QStringLiteral("One"))), "the one that was left behind");
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

void TestPreview::tableFillsWhileTheImportIsStillRunning()
{
    // Comfortably more than the import's batch of five thousand rows, so
    // progress is reported several times and there is a middle to observe.
    QByteArray csv = QByteArray("name;value\n");
    for (int row = 0; row < 12000; ++row)
        csv += QStringLiteral("row%1;%2\n").arg(row).arg(row).toUtf8();
    QVERIFY(m_tree->writeFile(QStringLiteral("big.csv"), csv));

    PreviewTabController* preview = openPreview(QStringLiteral("big.csv"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TablePreviewController*>(preview->viewer());
    QVERIFY(viewer);

    // Sampled from the signal rather than polled. The import finishes when it
    // finishes, and a poll that arrives one turn late would be looking at the
    // final state and pass without ever seeing the middle.
    qint64 rowsVisibleMidImport = -1;
    connect(viewer, &TablePreviewController::importProgress, viewer, [&] {
        if (rowsVisibleMidImport < 0 && viewer->isImporting() && viewer->importedRows() > 0)
            rowsVisibleMidImport = viewer->table()->totalRows();
    });

    QVERIFY(waitFor([viewer] { return !viewer->isImporting() && viewer->importedRows() == 12000; }, 30000));

    // The whole promise of importing into a database instead of parsing into
    // memory: the first screen is usable long before the last row lands. An
    // empty grid until the end is what makes a big file look like a hang.
    QVERIFY2(rowsVisibleMidImport > 0,
        "the grid has to fill as rows arrive, not only once the import has finished");
    QCOMPARE(viewer->table()->totalRows(), 12000);
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
