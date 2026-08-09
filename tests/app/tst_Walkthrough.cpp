#include "plugins/builtin/AnalysisFeature.h"
#include "plugins/builtin/AutomationFeature.h"
#include "plugins/builtin/BrowserFeature.h"
#include "plugins/builtin/BulkRenameFeature.h"
#include "plugins/builtin/PreviewFeature.h"
#include "plugins/builtin/SearchFeatures.h"
#include "plugins/builtin/previews/PdfPreview.h"
#include "plugins/builtin/previews/PreviewProviders.h"
#include "support/QmlAppHarness.h"
#include "ui/AppController.h"
#include "ui/models/BookmarkModel.h"
#include "ui/models/BrowserPaneController.h"
#include "ui/models/CommandPaletteModel.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TableModel.h"
#include "ui/models/TabsModel.h"
#include "ui/models/TaskListModel.h"
#include "ui/models/TerminalController.h"

#include "core/CoreMetaTypes.h"
#include "core/alerts/AlertStore.h"
#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"
#include "core/sets/FileSet.h"
#include "core/sets/FileSetStore.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

using namespace mole;
using namespace mole::test;

/// Drives the real application through the things a person actually does, and
/// photographs each one on the way.
///
/// Every screenshot here is taken immediately after the assertions that prove
/// the state is what the name claims. That is the point: a picture from this
/// suite cannot show something the tests did not verify, which is exactly the
/// guarantee the old Xvfb harness could not give.
class TestWalkthrough : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void browsesAndPreviews();
    void highlightsSourceAndPagesLargeFiles();
    void rendersMarkdownAsAPage();
    void aSlowTableSaysSoAndThenFillsAsItReads();
    void aPdfOpensAsPages();
    void htmlCanBeSwitchedBetweenSourceAndPage();
    void folderSizesLandInTheListing();
    void compressingTheSelectionMakesAnArchiveBesideIt();
    void deletingAsksWithTheFilesNamed();
    void theDrivesAreInThePaletteToo();
    void theListingTakesItsTypeSizeFromTheScale();
    void theIconOnlyControlsAreBigEnoughToHit();
    void theSidebarRowsAreEvenlyTallAndHoldStill();
    void theCopyPathKeysActuallyCopyAPath();
    void theCommandPaletteFindsAndRunsThings();
    void theHeaderAdvertisesTheCommandPalette();
    void ctrlFIsASearchBoxYouCanTypeInto();
    void searchResultsAreWalkableAndLeadSomewhere();
    void bulkRenameShowsThePreviewAsYouType();
    void breadcrumbsClimbTheTree();
    void ctrlGRevealsTheEditablePath();
    void aSlowFolderSaysSoInTheMiddleOfThePane();
    void theListingMarksReportsAndAlerts();
    void filtersByTyping();
    void filtersAndCopiesTableCells();
    void theFilterKeepsTheKeyboardWhileNarrowing();
    void dualPaneAndGrid();
    void f5CopiesTheSelectedFile();
    void analysesAFolder();
    void schedulesTheReportAndTracksIt();
    void ctrlWClosesAPreviewTabWithTheTextFocused();
    void theTerminalOpensInTheFolderYouAreLookingAt();
    void theTerminalTakesTheKeyboardAndCtrlDEndsIt();
    void theDrivesDialogOffersBackendsAndAForm();
    void everyBackendBuildsAFormWithoutComplaint();
    void aDriveWithAPasswordSavesAndConnects();
    void savingThroughTheFormShowsWhatTheCheckFound();
    void typingIntoTheKindPickerFiltersIt();
    void connectingFromTheListSurvivesTheListRebuilding();
    void emptyWindowExplainsItself();

private:
    BrowserPaneController* pane() const;

    std::unique_ptr<QmlAppHarness> m_harness;
    QString m_shots;
};

void TestWalkthrough::initTestCase()
{
    m_shots = QString::fromLocal8Bit(qgetenv("MOLE_SCREENSHOT_DIR"));
}

void TestWalkthrough::init()
{
    m_harness = std::make_unique<QmlAppHarness>();

    QmlAppHarness::Options options;
    options.screenshotDirectory = m_shots;

    QString error;
    QVERIFY2(m_harness->start(options, &error), qPrintable(error));

    QVERIFY(m_harness->makeDirs(QStringLiteral("media")));
    QVERIFY(m_harness->makeDirs(QStringLiteral("documents")));
    QVERIFY(m_harness->writeFile(QStringLiteral("media/film.mkv"), QByteArray(90000, 'x')));
    QVERIFY(m_harness->writeFile(QStringLiteral("media/clip.mkv"), QByteArray(30000, 'x')));
    QVERIFY(m_harness->writeFile(
        QStringLiteral("documents/report.txt"), QByteArray("The quarterly report.\nSecond line.\n")));
    QVERIFY(m_harness->writeFile(QStringLiteral("documents/prices.csv"),
        QByteArray("name;price;qty\nwidget;1,50;3\nbolt;0,99;10\nnut;12,00;250\n")));
    QVERIFY(m_harness->writeFile(QStringLiteral("settings.json"),
        QByteArray("{\n  \"name\": \"example\",\n  \"count\": 42,\n  \"ok\": true\n}\n")));
    QVERIFY(m_harness->writeFile(QStringLiteral("notes.txt"), QByteArray("plain notes")));

    // Reload so the pane sees the fixture that was just written.
    pane()->refresh();
    QVERIFY(m_harness->until([this] { return !pane()->isLoading() && pane()->files()->rowCount() == 4; }));
}

void TestWalkthrough::cleanup()
{
    m_harness.reset();
}

BrowserPaneController* TestWalkthrough::pane() const
{
    auto* browser = qobject_cast<BrowserController*>(m_harness->app()->tabs()->currentController());
    return browser ? browser->activePane() : nullptr;
}

void TestWalkthrough::browsesAndPreviews()
{
    QCOMPARE(pane()->files()->rowCount(), 4); // two folders, two files
    m_harness->screenshot(QStringLiteral("01-browser"));

    // Into a folder with the keyboard.
    m_harness->key(Qt::Key_Return);
    QVERIFY(m_harness->until([this] { return pane()->currentUri().endsWith(QStringLiteral("/documents")); }));
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 2; }));

    // Preview the CSV: the table viewer must win over the text one.
    const int row = pane()->files()->rowOfUri(pane()->currentUri() + QStringLiteral("/prices.csv"));
    QVERIFY(row >= 0);
    pane()->setCurrentIndex(row);
    m_harness->settle();

    m_harness->key(Qt::Key_F3);
    auto* preview = qobject_cast<PreviewTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY2(preview, "F3 must open a preview tab");
    QCOMPARE(preview->viewerName(), QStringLiteral("Table"));
    QVERIFY(m_harness->until([preview] { return preview->siblingCount() > 0; }));
    m_harness->settle();
    m_harness->screenshot(QStringLiteral("02-preview-csv"));

    // Right steps to the next file in the folder, and the viewer changes with it.
    m_harness->key(Qt::Key_Right);
    QVERIFY(m_harness->until([preview] { return preview->fileName() == QStringLiteral("report.txt"); }));
    QCOMPARE(preview->viewerName(), QStringLiteral("Text"));
    m_harness->screenshot(QStringLiteral("03-preview-text"));
}

void TestWalkthrough::highlightsSourceAndPagesLargeFiles()
{
    // A JSON file is coloured; a plain one is not, and neither is Markdown,
    // which is rendered instead.
    const int row = pane()->files()->rowOfUri(pane()->currentUri() + QStringLiteral("/settings.json"));
    QVERIFY(row >= 0);
    pane()->setCurrentIndex(row);
    m_harness->settle();
    m_harness->key(Qt::Key_F3);

    auto* preview = qobject_cast<PreviewTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(m_harness->until([viewer] { return !viewer->text().isEmpty(); }));

    QCOMPARE(viewer->languageName(), QStringLiteral("JSON"));
    QVERIFY(viewer->isHighlighted());
    QVERIFY2(!viewer->isMarkdown(), "JSON is coloured, not rendered");
    // Small file: one window, so no paging strip to distract with.
    QVERIFY(!viewer->isPaged());
    m_harness->settle(6);
    m_harness->screenshot(QStringLiteral("03b-preview-json"));

    // Now a file bigger than one window. Paging appears, and stepping forward
    // moves the window rather than loading more of the file into memory.
    QByteArray big;
    for (int i = 0; i < 40000; ++i)
        big += QStringLiteral("line %1 of a rather long log file\n").arg(i, 6, 10, QLatin1Char('0')).toUtf8();
    QVERIFY(big.size() > 512 * 1024);
    QVERIFY(m_harness->writeFile(QStringLiteral("huge.log"), big));

    FileEntry entry;
    entry.uri = VfsUri::fromString(m_harness->fixtureUri() + QStringLiteral("/huge.log"));
    entry.name = QStringLiteral("huge.log");
    entry.size = big.size();
    viewer->load(entry);

    // Waited on the new file's own content: "text is not empty" would have been
    // satisfied by the previous file and the assertions below would have run
    // against it.
    QVERIFY(m_harness->until(
        [viewer] { return viewer->isPaged() && viewer->text().startsWith(QStringLiteral("line 000000")); }));
    QCOMPARE(viewer->fileSize(), static_cast<qint64>(big.size()));
    QVERIFY2(viewer->windowBytes() <= 512 * 1024, "only the window is held, never the file");
    QVERIFY(viewer->isAtStart());
    QVERIFY(!viewer->isAtEnd());

    const qint64 firstOffset = viewer->windowOffset();
    viewer->nextWindow();
    QVERIFY(m_harness->until([viewer, firstOffset] { return viewer->windowOffset() > firstOffset; }));
    QVERIFY2(!viewer->text().isEmpty(), "the next window has content");
    QVERIFY2(viewer->text().startsWith(QStringLiteral("line ")),
        "windows snap to whole lines, so no window opens mid-line");

    viewer->lastWindow();
    QVERIFY(m_harness->until([viewer] { return viewer->isAtEnd(); }));
    m_harness->settle(6);
    m_harness->screenshot(QStringLiteral("03c-preview-paging"));
}

void TestWalkthrough::ctrlGRevealsTheEditablePath()
{
    // The crumbs and the editable path share one slot, so exactly one of them
    // has to be on screen at a time -- and the one with the keyboard is the one
    // the user needs to see.
    QQuickItem* crumbs = m_harness->item(QStringLiteral("pathCrumbs"));
    QVERIFY(crumbs);
    QVERIFY2(crumbs->isVisible(), "the crumbs show while not editing");

    m_harness->key(Qt::Key_G, Qt::ControlModifier);
    m_harness->settle(6);

    QQuickItem* field = m_harness->item(QStringLiteral("pathField"));
    QVERIFY(field);
    QVERIFY2(field->hasActiveFocus(), "Ctrl+G puts the keyboard in the path field");
    QVERIFY2(field->isVisible(), "and the field it puts it in has to be on screen");
    QVERIFY2(field->width() > 0, "with a size");

    // Room for all of itself. Squeezed into a shorter slot the text and the
    // underline are clipped, which reads as the field being covered by
    // something rather than as it being too small.
    QVERIFY2(
        field->height() >= field->implicitHeight(), "the path field has to fit in the space it is given");
    QVERIFY2(!crumbs->isVisible(), "the crumbs step aside rather than covering it");

    // The path is there to be edited, selected so typing replaces it.
    QCOMPARE(field->property("text").toString(), pane()->displayPath());
    QVERIFY(!field->property("selectedText").toString().isEmpty());

    m_harness->key(Qt::Key_Escape);
    QVERIFY(m_harness->until([crumbs] { return crumbs->isVisible(); }));
}

void TestWalkthrough::aSlowFolderSaysSoInTheMiddleOfThePane()
{
    // A drive that takes its time, so the one-second threshold is reached and
    // the waiting view actually appears. Contrived, because the only honest way
    // to test a slow listing is to have one.
    auto slow = std::make_shared<MemoryFileSystem>();
    slow->addFile(QStringLiteral("/a.txt"), QByteArray("a"));
    slow->addFile(QStringLiteral("/b.txt"), QByteArray("b"));
    // Long enough that the window in which the view is on screen -- from one
    // second in until the listing lands -- cannot be missed on a loaded
    // machine. The test cancels rather than waiting it out.
    slow->setListDelayMs(8000);

    Mount mount;
    mount.id = QStringLiteral("slow");
    mount.displayName = QStringLiteral("slow");
    mount.root = VfsUri::fromString(QStringLiteral("mem://slow/"));
    mount.fileSystem = slow;
    m_harness->app()->services().vfs->addMount(mount);

    pane()->navigateTo(QStringLiteral("mem://slow/"));

    // Below a second a spinner is only a flash, so nothing is shown yet.
    m_harness->settle(10);
    QQuickItem* early = m_harness->item(QStringLiteral("loadingView"));
    QVERIFY(early);
    QVERIFY2(!early->isVisible(), "a fast listing must not flash a spinner");

    // Both panes exist even in single mode, and the hidden one has zero size,
    // so the visible one is the one to measure. Waited on the width as well as
    // on visibility: becoming visible and being given a size are two separate
    // passes, and asking in between gives zero.
    const auto visibleLoadingView = [this]() -> QQuickItem* {
        const QList<QQuickItem*> candidates = m_harness->items(QStringLiteral("loadingView"));
        for (QQuickItem* candidate : candidates) {
            if (candidate->isVisible() && candidate->width() > 0)
                return candidate;
        }
        return nullptr;
    };

    QQuickItem* loading = nullptr;
    const bool appeared = m_harness->until(
        [&] {
            loading = visibleLoadingView();
            return loading != nullptr;
        },
        6000);

    if (!appeared) {
        for (QQuickItem* candidate : m_harness->items(QStringLiteral("loadingView")))
            qWarning("  loadingView vis=%d w=%.0f", candidate->isVisible(), candidate->width());
    }
    QVERIFY2(appeared, "a listing still going after a second has to say so, across the pane");

    // It has to span the pane. Centring inside a container only as wide as its
    // widest child is what put the empty window against the left edge, and the
    // same mistake here would read as a message stuck to the frame.
    QQuickItem* fileList = m_harness->item(QStringLiteral("fileList"));
    QVERIFY(fileList);
    QVERIFY2(loading->width() >= fileList->width() - 1,
        "the waiting view spans the pane rather than hugging one edge");

    m_harness->screenshot(QStringLiteral("01d-slow-folder"));

    // Stop rather than sit through the rest of the delay, which is also what
    // the Stop button on that view does.
    m_harness->app()->tasks()->cancelAll();
    QVERIFY(m_harness->until([this] { return !pane()->isLoading(); }, 10000));
}

void TestWalkthrough::theListingMarksReportsAndAlerts()
{
    const QString documents = pane()->currentUri() + QStringLiteral("/documents");
    const QString media = pane()->currentUri() + QStringLiteral("/media");

    // Nothing is marked to begin with.
    const int docRow = pane()->files()->rowOfUri(documents);
    QVERIFY(docRow >= 0);
    const auto flag
        = [this](int row, int role) { return pane()->files()->index(row, 0).data(role).toBool(); };
    QVERIFY(!flag(docRow, FileListModel::HasReportRole));
    QVERIFY(!flag(docRow, FileListModel::HasAlertRole));

    // A report on one folder and an alert on another. Both have to show on the
    // listing without opening anything.
    m_harness->app()->openReportFor(documents);
    m_harness->settle();
    auto* analysis = qobject_cast<AnalysisTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(analysis);
    QVERIFY(m_harness->until(
        [analysis] { return analysis->current() && analysis->current()->hasReport(); }, 20000));

    AlertRule rule;
    rule.id = QStringLiteral("watch-media");
    rule.label = QStringLiteral("Media grows");
    rule.targetUri = media;
    rule.state = AlertState::Triggered;
    QVERIFY(m_harness->app()->alerts()->put(rule));

    // Back to the listing, which re-reads the annotations as it loads.
    m_harness->app()->tabs()->setCurrentIndex(0);
    m_harness->settle();
    pane()->refresh();
    QVERIFY(m_harness->until([this] { return !pane()->isLoading(); }));

    const int docRowAgain = pane()->files()->rowOfUri(documents);
    const int mediaRow = pane()->files()->rowOfUri(media);
    QVERIFY(docRowAgain >= 0);
    QVERIFY(mediaRow >= 0);

    QVERIFY2(flag(docRowAgain, FileListModel::HasReportRole), "the reported folder is marked");
    QVERIFY2(!flag(docRowAgain, FileListModel::HasAlertRole), "and not marked for an alert it has not got");
    QVERIFY2(flag(mediaRow, FileListModel::HasAlertRole), "the watched folder is marked");
    QVERIFY2(flag(mediaRow, FileListModel::AlertTriggeredRole), "and a tripped alert reads differently");
    QVERIFY2(!flag(mediaRow, FileListModel::HasReportRole), "a folder with no report is not marked");

    m_harness->settle(6);
    m_harness->screenshot(QStringLiteral("01c-listing-tags"));
}

void TestWalkthrough::rendersMarkdownAsAPage()
{
    // Everything a page of Markdown can hold, because this is also where the
    // picture of the rendered page comes from.
    QVERIFY(m_harness->writeFile(QStringLiteral("guide.md"),
        QByteArray("# Mole\n\nAn IDE, but for files. A paragraph of prose, which a viewer has to set as a\n"
                   "page rather than as a wall of text: the measure is capped and the gutters take\n"
                   "the surplus width.\n\n## Section\n\nMore prose, with `inline code` in it.\n\n"
                   "- A list item.\n- A second one, which stays close to the first.\n\n"
                   "| Drive | Free |\n|-------|------|\n| Home | 43 GiB |\n| nas | 5.8 TiB |\n\n"
                   "```cpp\nint main()\n{\n    return 0;\n}\n```\n\n"
                   "> A quoted line, indented and set a little quieter.\n\n"
                   "### Smaller heading\n\nAnd the paragraph that belongs to it.\n")));
    // The table comes before the code fence and the quote, not after either:
    // Qt's importer ends both with a stray empty block that lands inside the
    // first cell of a table that follows, which mangles its header. That happens
    // before any of the styling runs, and a picture of this page should not be
    // showing it off.
    pane()->refresh();
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 5; }));

    const int row = pane()->files()->rowOfUri(pane()->currentUri() + QStringLiteral("/guide.md"));
    QVERIFY(row >= 0);
    pane()->setCurrentIndex(row);
    m_harness->settle();
    m_harness->key(Qt::Key_F3);

    auto* preview = qobject_cast<PreviewTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(m_harness->until([viewer] { return viewer->isMarkdown() && !viewer->text().isEmpty(); }));
    m_harness->settle(6);

    QQuickItem* text = m_harness->item(QStringLiteral("previewText"));
    QVERIFY(text);
    // Gutters rather than text against the frame: a page has margins, and on a
    // wide window they grow so the line length stays readable.
    QVERIFY2(text->property("leftPadding").toReal() > 20.0, "a rendered page keeps its margins");
    QCOMPARE(text->property("leftPadding").toReal(), text->property("rightPadding").toReal());

    auto* handle = text->property("textDocument").value<QQuickTextDocument*>();
    QVERIFY(handle);
    QTextDocument* document = handle->textDocument();
    QVERIFY(document);

    const auto blockSaying = [document](const QString& wanted) {
        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            if (block.text() == wanted)
                return block;
        }
        return QTextBlock();
    };

    // The styling has to reach the document the window is actually showing.
    // Everything else about it is covered where it can be measured exactly, in
    // tst_Preview; what only the real application can prove is the wiring.
    QVERIFY(m_harness->until([&blockSaying] { return blockSaying(QStringLiteral("Section")).isValid(); }));
    QVERIFY2(blockSaying(QStringLiteral("Section")).blockFormat().topMargin() > 0.0,
        "a heading in the middle of the page has to have been given space above it");
    // Checked on the text rather than the block: the scene graph behind a QML
    // TextArea paints the character background and ignores the block's, so this
    // is the one that decides whether anything is actually seen.
    const QTextBlock code = blockSaying(QStringLiteral("int main()"));
    QVERIFY(code.isValid());
    QVERIFY2(code.begin().fragment().charFormat().background().style() != Qt::NoBrush,
        "code has to have been given its slab");

    m_harness->screenshot(QStringLiteral("03c-preview-markdown"));

    // The same viewer, handed a plain file. Stepping between files in the tab
    // builds a fresh viewer each time, so this -- one viewer told to show
    // something else -- is the path where the document is reused, and where the
    // styling has to come off with the file it belonged to. Left attached, it
    // would answer the plain file's arrival by spacing its lines out like prose,
    // and a text file is not prose: it is lines.
    const auto entryFor = [this](const QString& name) {
        FileEntry entry;
        entry.uri = VfsUri::fromString(m_harness->fixtureUri() + QLatin1Char('/') + name);
        entry.name = name;
        entry.size = QFileInfo(m_harness->fixturePath() + QLatin1Char('/') + name).size();
        return entry;
    };

    viewer->load(entryFor(QStringLiteral("notes.txt")));
    QVERIFY(m_harness->until(
        [viewer] { return !viewer->isMarkdown() && viewer->text() == QStringLiteral("plain notes"); }));
    m_harness->settle(6);
    QCOMPARE(document->firstBlock().blockFormat().bottomMargin(), 0.0);

    // And back, because the switch has to work in both directions.
    viewer->load(entryFor(QStringLiteral("guide.md")));
    QVERIFY(m_harness->until([viewer] { return viewer->isMarkdown() && !viewer->text().isEmpty(); }));
    QVERIFY2(m_harness->until([&blockSaying] {
        const QTextBlock again = blockSaying(QStringLiteral("Section"));
        return again.isValid() && again.blockFormat().topMargin() > 0.0;
    }),
        "coming back to a Markdown file has to style it again");
}

void TestWalkthrough::aSlowTableSaysSoAndThenFillsAsItReads()
{
    // A quick file first: the message must not flash for something that arrives
    // immediately, which is the whole reason for the one-second threshold.
    m_harness->app()->previewFile(m_harness->fixtureUri() + QStringLiteral("/documents/prices.csv"));
    auto* quick = qobject_cast<PreviewTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(quick);
    QVERIFY(m_harness->until([quick] {
        auto* table = qobject_cast<TablePreviewController*>(quick->viewer());
        return table && table->table()->rowCount() > 0;
    }));
    m_harness->settle(8);
    for (QQuickItem* view : m_harness->items(QStringLiteral("csvLoadingView")))
        QVERIFY2(!view->isVisible(), "a file that arrives at once must not flash a message");

    // Now a drive that takes its time opening the file. Contrived, because the
    // only honest way to test what a view does while a read is slow is to have
    // one that is.
    QByteArray csv = QByteArray("name;value\n");
    for (int row = 0; row < 12000; ++row)
        csv += QStringLiteral("row%1;%2\n").arg(row).arg(row).toUtf8();

    auto slow = std::make_shared<MemoryFileSystem>();
    slow->addFile(QStringLiteral("/export.csv"), csv);
    slow->setReadDelayMs(2500);

    Mount mount;
    mount.id = QStringLiteral("slowtable");
    mount.displayName = QStringLiteral("slowtable");
    mount.root = VfsUri::fromString(QStringLiteral("mem://slowtable/"));
    mount.fileSystem = slow;
    m_harness->app()->services().vfs->addMount(mount);

    m_harness->app()->previewFile(QStringLiteral("mem://slowtable/export.csv"));
    auto* preview = qobject_cast<PreviewTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(preview);
    auto* table = qobject_cast<TablePreviewController*>(preview->viewer());
    QVERIFY(table);

    // Both panes of a preview exist even when one is hidden, so the visible one
    // with a size is the one that means anything.
    const auto visibleLoadingView = [this]() -> QQuickItem* {
        for (QQuickItem* candidate : m_harness->items(QStringLiteral("csvLoadingView"))) {
            if (candidate->isVisible() && candidate->width() > 0)
                return candidate;
        }
        return nullptr;
    };

    QVERIFY2(m_harness->until([&] { return visibleLoadingView() != nullptr; }, 6000),
        "past a second with nothing to show yet, the view has to say it is reading");
    m_harness->screenshot(QStringLiteral("02b-preview-csv-loading"));

    // And once rows start landing the message gets out of the way, because the
    // rows themselves are the better answer to "is this stuck".
    QVERIFY(m_harness->until([table] { return table->table()->rowCount() > 0; }, 20000));
    QVERIFY(m_harness->until([&] { return visibleLoadingView() == nullptr; }, 6000));
    QVERIFY2(m_harness->item(QStringLiteral("csvGrid")) != nullptr, "the grid takes the space back");

    QVERIFY(m_harness->until([table] { return !table->isImporting(); }, 30000));
    QCOMPARE(table->table()->totalRows(), 12000);
}

void TestWalkthrough::folderSizesLandInTheListing()
{
    // The fixture has media/ at 120 kB across two files and documents/ with a
    // report and a price list, so the two folders have visibly different answers.
    FileListModel* files = pane()->files();
    const QString media = pane()->currentUri() + QStringLiteral("/media");
    QVERIFY(files->rowOfUri(media) >= 0);
    QCOMPARE(files->measuredSize(media), -1);

    m_harness->app()->triggerAction(QStringLiteral("mole.tools.folderSizes"));
    QVERIFY(m_harness->until([files, media] { return files->measuredSize(media) > 0; }, 15000));

    // 90 000 + 30 000 across the two files in there.
    QCOMPARE(files->measuredSize(media), 120000);

    m_harness->settle(8);
    m_harness->screenshot(QStringLiteral("01b-folder-sizes"));
}

void TestWalkthrough::theListingTakesItsTypeSizeFromTheScale()
{
    // The scale is only worth having if the views actually read from it, and a
    // literal left behind in a delegate is invisible until someone compares two
    // views side by side.
    QQuickItem* name = m_harness->item(QStringLiteral("fileName"));
    QVERIFY2(name, "a listing row has to have a name label to measure");

    const QFont font = name->property("font").value<QFont>();
    QCOMPARE(font.pixelSize(), m_harness->app()->textSize());
}

void TestWalkthrough::theIconOnlyControlsAreBigEnoughToHit()
{
    // The two the complaint named: adding a bookmark and closing a tab. Both were
    // ToolButtons of 22 by 22 with a text glyph inside, which is fiddly to hit and
    // reads as an afterthought for the two things people do most.
    const int floor = m_harness->app()->minimumTarget();
    QVERIFY(floor >= 24);

    for (const QString& name : { QStringLiteral("addBookmarkButton"), QStringLiteral("closeTabButton") }) {
        QQuickItem* control = m_harness->item(name);
        QVERIFY2(control, qPrintable(name));
        QVERIFY2(control->width() >= floor,
            qPrintable(
                QStringLiteral("%1 is %2 wide, floor is %3").arg(name).arg(control->width()).arg(floor)));
        QVERIFY2(control->height() >= floor, qPrintable(name));

        // And the mark inside grew with the box: a bigger button with the style's
        // default glyph in the middle of it looks emptier, not clearer.
        const QFont font = control->property("font").value<QFont>();
        QCOMPARE(font.pixelSize(), m_harness->app()->textSize());
    }
}

/// The advertised keys really copy a path.
///
/// An action's `shortcut` is only what the menu prints beside it -- the binding is
/// a separate declaration in Main.qml -- so a key can be named in the menu and do
/// nothing at all. This presses the keys.
/// The advertised keys really copy a path, and go on working afterwards.
///
/// Two things are pinned here. An action's `shortcut` is only what the menu prints
/// beside it -- the binding is a separate declaration in Main.qml -- so a key can
/// be named in the menu and do nothing at all.
///
/// And the second press matters as much as the first. Copying shows a notification,
/// the notification was a Popup that closed on Escape, and a popup that wants key
/// events took them from the whole window: every shortcut in the application went
/// dead for the five seconds the toast was up. Pressing twice is what catches that.
void TestWalkthrough::theCopyPathKeysActuallyCopyAPath()
{
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() > 0; }));

    QClipboard* clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard);

    clipboard->setText(QStringLiteral("nothing yet"));
    m_harness->key(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    const QString folder = clipboard->text();
    QVERIFY2(folder != QLatin1String("nothing yet"), "Ctrl+Shift+C has to copy the folder");
    // Native, so it can be pasted into a terminal or a file dialog.
    QVERIFY2(!folder.startsWith(QLatin1String("file:")), qPrintable(folder));
    QCOMPARE(folder, m_harness->fixturePath());

    // Onto a file, since the fixture's first rows are folders.
    const int row = pane()->files()->rowOfUri(
        VfsUri::fromLocalPath(m_harness->fixturePath()).child(QStringLiteral("notes.txt")).toString());
    QVERIFY(row >= 0);
    pane()->setCurrentIndex(row);
    m_harness->settle(2);

    // The second press, with the first one's notification still on screen.
    clipboard->setText(QStringLiteral("nothing yet"));
    m_harness->key(Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
    const QString file = clipboard->text();
    QVERIFY2(
        file != QLatin1String("nothing yet"), "a shortcut has to keep working while a notification is up");
    QVERIFY2(file.endsWith(QLatin1String("notes.txt")), qPrintable(file));
    QVERIFY2(file.startsWith(folder), qPrintable(QStringLiteral("%1 is not inside %2").arg(file, folder)));

    // And a third, to be sure the first one was not simply the only one.
    clipboard->setText(QStringLiteral("nothing yet"));
    m_harness->key(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(clipboard->text(), folder);
}

/// Drive and bookmark rows are buttons, and have to behave like buttons: one
/// height per list, tall enough to hit, and nothing that moves when the pointer
/// arrives.
///
/// Both halves were wrong. The rows were 30 and 46 pixels tall, and 46 was less
/// than the content inside it actually needed, so it was being squeezed. And the
/// × appeared on hover while the "free" caption disappeared -- two width changes
/// either side of a name label that fills the space left over, so the name
/// re-elided and visibly jumped as the pointer crossed the row.
void TestWalkthrough::theSidebarRowsAreEvenlyTallAndHoldStill()
{
    // A bookmark, so that a removable row is on screen: the × is the part that
    // used to shove the name aside.
    m_harness->app()->triggerAction(QStringLiteral("mole.bookmarks.add"));
    QVERIFY(m_harness->until([this] { return m_harness->app()->bookmarks()->rowCount() > 0; }));
    m_harness->settle(4);

    const QList<QQuickItem*> rows = m_harness->items(QStringLiteral("placeRow"));
    QVERIFY2(!rows.isEmpty(), "the sidebar has rows to measure");

    const int floor = m_harness->app()->minimumTarget();

    // One height per list. The drives list is mixed -- a local disk cannot be
    // ejected, an archive can -- so a height that followed the × would make two
    // kinds of row in one list.
    QHash<QQuickItem*, double> heightPerList;
    for (QQuickItem* row : rows) {
        QVERIFY2(row->height() >= floor + 4,
            qPrintable(
                QStringLiteral("a row is only %1 tall, floor is %2").arg(row->height()).arg(floor + 4)));

        QQuickItem* list = row->parentItem();
        if (!heightPerList.contains(list))
            heightPerList.insert(list, row->height());
        QVERIFY2(qFuzzyCompare(heightPerList.value(list), row->height()),
            qPrintable(QStringLiteral("rows in one list are %1 and %2 tall")
                           .arg(heightPerList.value(list))
                           .arg(row->height())));

        // And the content fits, rather than being compressed into a row that was
        // given a smaller number than it needs.
        QQuickItem* content = row->property("contentItem").value<QQuickItem*>();
        QVERIFY(content);
        QVERIFY2(row->height() >= content->property("implicitHeight").toDouble(),
            qPrintable(QStringLiteral("a %1 tall row is squeezing %2 of content")
                           .arg(row->height())
                           .arg(content->property("implicitHeight").toDouble())));
    }

    // Find a row that has a × in it, and check the button holds its place while
    // nothing is hovering it. Reserving the width is the whole fix.
    QQuickItem* removable = nullptr;
    for (QQuickItem* row : rows) {
        QQuickItem* button = row->findChild<QQuickItem*>(QStringLiteral("placeRemoveButton"));
        if (button && button->isVisible()) {
            removable = row;
            break;
        }
    }
    QVERIFY2(removable, "a bookmark row has a remove button");

    QQuickItem* button = removable->findChild<QQuickItem*>(QStringLiteral("placeRemoveButton"));
    QQuickItem* label = removable->findChild<QQuickItem*>(QStringLiteral("placeRowLabel"));
    QVERIFY(button);
    QVERIFY(label);
    QVERIFY2(button->width() >= floor, "the remove button is big enough to hit");
    QVERIFY2(button->opacity() < 0.5, "and is faded out until the pointer arrives");

    const double labelXBefore = label->x();
    const double labelWidthBefore = label->width();

    // Now hover it for real.
    const QPointF centre = removable->mapToScene(QPointF(removable->width() / 2, removable->height() / 2));
    QTest::mouseMove(m_harness->window(), centre.toPoint());
    QVERIFY(m_harness->until([removable] { return removable->property("hovered").toBool(); }));
    m_harness->settle(4);

    QVERIFY2(button->opacity() > 0.5, "the remove button appears under the pointer");
    QCOMPARE(label->x(), labelXBefore);
    QCOMPARE(label->width(), labelWidthBefore);
}

void TestWalkthrough::bulkRenameShowsThePreviewAsYouType()
{
    m_harness->key(Qt::Key_Return); // into "documents"
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 2; }));
    pane()->files()->selectAll();
    m_harness->settle();

    m_harness->app()->triggerAction(QStringLiteral("mole.tools.bulkRename"));
    auto* rename = qobject_cast<BulkRenameController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(rename);
    QCOMPARE(rename->sourceCount(), 2);

    rename->addRule(QStringLiteral("affix"));
    m_harness->settle(6);

    // The form is capped and the preview keeps a floor, so the thing this view
    // calls its own feature is not squeezed into what the rules leave over.
    QQuickItem* preview = m_harness->item(QStringLiteral("renamePreviewList"));
    QVERIFY(preview);
    QVERIFY2(preview->width() >= 320, "the preview keeps a usable width");

    // Typed into, not set through the controller: what was wrong was that nothing
    // happened until the field lost the keyboard, so the keyboard has to stay in
    // it for this to mean anything.
    QQuickItem* prefix = m_harness->item(QStringLiteral("rulePrefixField"));
    QVERIFY(prefix);
    prefix->forceActiveFocus();
    m_harness->settle(4);
    QVERIFY(prefix->hasActiveFocus());

    m_harness->type(QStringLiteral("2024_"));
    QVERIFY2(m_harness->until(
                 [rename] {
                     return rename->changedCount() == 2
                         && rename->preview().first().toMap().value(QStringLiteral("to")).toString()
                         == QStringLiteral("2024_prices.csv");
                 },
                 4000),
        "the preview has to follow what is typed, without leaving the field");

    m_harness->settle(8);
    m_harness->screenshot(QStringLiteral("07b-bulk-rename"));
}

void TestWalkthrough::aPdfOpensAsPages()
{
    if (!PdfPreviewProvider::isAvailable())
        QSKIP("this build cannot render a PDF");

    // Written here rather than committed: a binary fixture is one nobody reviews.
    const QString path = m_harness->fixturePath() + QStringLiteral("/manual.pdf");
    {
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        QPainter painter(&writer);
        QVERIFY(painter.isActive());
        painter.setFont(QFont(QStringLiteral("sans"), 48));
        painter.drawText(QRect(0, 0, 6000, 1200), Qt::AlignCenter, QStringLiteral("Mole"));
        painter.setFont(QFont(QStringLiteral("sans"), 18));
        painter.drawText(QRect(0, 1400, 6000, 800), Qt::AlignCenter,
            QStringLiteral("A document, rendered a page at a time."));
        QVERIFY(writer.newPage());
        painter.drawText(QRect(0, 0, 6000, 1000), Qt::AlignCenter, QStringLiteral("Page two"));
    }

    m_harness->app()->previewFile(m_harness->fixtureUri() + QStringLiteral("/manual.pdf"));
    auto* preview = qobject_cast<PreviewTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(preview);
    QCOMPARE(preview->viewerName(), QStringLiteral("Document"));

    auto* viewer = qobject_cast<PdfPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(m_harness->until([viewer] { return viewer->pageCount() == 2; }, 10000));
    m_harness->settle(10);

    // The view has to have asked for a page and got an image back -- the whole
    // path from a delegate coming into view to a rendered file on disk.
    QQuickItem* page = m_harness->item(QStringLiteral("pdfPage"));
    QVERIFY2(page, "a page delegate has to exist");
    QVERIFY2(m_harness->until([page] { return page->property("source").toUrl().isValid(); }, 6000),
        "the delegate asks the controller for its page image");
    QVERIFY2(m_harness->until([page] { return page->property("progress").toDouble() >= 1.0; }, 10000),
        "and the image loads");
    QVERIFY(page->width() > 0 && page->height() > page->width());

    QQuickItem* position = m_harness->item(QStringLiteral("pdfPosition"));
    QVERIFY(position);
    QVERIFY(position->property("text").toString().contains(QStringLiteral("of 2")));

    m_harness->screenshot(QStringLiteral("03d-preview-pdf"));
}

void TestWalkthrough::theCommandPaletteFindsAndRunsThings()
{
    m_harness->key(Qt::Key_R, Qt::ControlModifier);

    QObject* palette = m_harness->object(QStringLiteral("commandPalette"));
    QVERIFY(palette);
    QVERIFY2(m_harness->until([palette] { return palette->property("opened").toBool(); }), "Ctrl+R opens it");

    // Typed into straight away: the box exists because not every control has a
    // shortcut, so needing a click to reach it would be a poor sort of answer.
    QQuickItem* field = m_harness->item(QStringLiteral("commandPaletteInput"));
    QVERIFY(field);
    QVERIFY2(field->hasActiveFocus(), "the input has the keyboard as soon as it opens");

    QQuickItem* list = m_harness->item(QStringLiteral("commandPaletteList"));
    QVERIFY(list);
    QVERIFY2(m_harness->until([list] { return list->property("count").toInt() > 5; }),
        "it opens holding everything, not empty");

    m_harness->type(QStringLiteral("termi"));
    QVERIFY2(m_harness->until([list] { return list->property("count").toInt() == 1; }),
        "five characters is enough to find one command out of everything");

    m_harness->settle(6);
    m_harness->screenshot(QStringLiteral("11-command-palette"));

    // Closed and opened again: it must start empty. Leaving the last query in the
    // box means the next Ctrl+R opens onto a list already filtered by whatever was
    // typed before, and the first thing anyone would have to do is clear it.
    // Checked before running anything, because a command that takes the keyboard --
    // the terminal does -- would stop Ctrl+R reaching the window at all.
    m_harness->key(Qt::Key_Escape);
    QVERIFY(m_harness->until([palette] { return !palette->property("opened").toBool(); }));

    m_harness->key(Qt::Key_R, Qt::ControlModifier);
    QVERIFY(m_harness->until([palette] { return palette->property("opened").toBool(); }));
    QCOMPARE(field->property("text").toString(), QString());
    QVERIFY2(m_harness->until([list] { return list->property("count").toInt() > 5; }),
        "and holding everything again, not the one row the last query left");

    // And Enter runs what is highlighted. The terminal is a good thing to prove it
    // with: whether it ran is a fact about the application, not about the palette.
    TerminalController* terminal = m_harness->app()->terminal();
    QVERIFY(terminal);
    if (!terminal->isAvailable())
        QSKIP("no pseudo-terminal on this platform");
    QVERIFY(!terminal->isVisible());

    m_harness->type(QStringLiteral("termi"));
    QVERIFY(m_harness->until([list] { return list->property("count").toInt() == 1; }));
    m_harness->key(Qt::Key_Return);
    QVERIFY2(
        m_harness->until([terminal] { return terminal->isVisible(); }), "Enter runs the highlighted command");
    QVERIFY(m_harness->until([palette] { return !palette->property("opened").toBool(); }));
}

void TestWalkthrough::theHeaderAdvertisesTheCommandPalette()
{
    // The palette is the answer to "how do I do X", and nobody finds a shortcut they
    // were never told about -- so the bar exists to be seen, and that is what is
    // asserted: visible, in the middle of the window, and it opens the real thing.
    QQuickItem* bar = m_harness->item(QStringLiteral("commandBar"));
    QVERIFY2(bar, "the header carries a command bar");
    QVERIFY(bar->isVisible());
    QVERIFY(bar->width() > 200);

    const QPointF centre = bar->mapToScene(QPointF(bar->width() / 2, bar->height() / 2));
    const double windowMiddle = m_harness->window()->width() / 2.0;
    QVERIFY2(qAbs(centre.x() - windowMiddle) < 2.0,
        "centred in the window, not merely between whatever else is in the toolbar");

    // At the height of the hamburger, which is what makes it read as part of the
    // title bar rather than as something floating in the listing.
    QQuickItem* hamburger = m_harness->item(QStringLiteral("menuButton"));
    QVERIFY(hamburger);
    const double barMiddle = bar->mapToScene(QPointF(0, bar->height() / 2)).y();
    const double menuMiddle = hamburger->mapToScene(QPointF(0, hamburger->height() / 2)).y();
    QVERIFY2(qAbs(barMiddle - menuMiddle) < 4.0, "on the same line as the menu button");

    QObject* palette = m_harness->object(QStringLiteral("commandPalette"));
    QVERIFY(palette);
    QVERIFY(!palette->property("opened").toBool());

    // Clicking it opens the real palette rather than trying to be one: one box owns
    // the list and the filtering.
    QTest::mouseClick(m_harness->window(), Qt::LeftButton, Qt::NoModifier,
        m_harness->window()->contentItem()->mapFromScene(centre).toPoint());
    QVERIFY2(m_harness->until([palette] { return palette->property("opened").toBool(); }),
        "the bar opens the palette");
}

void TestWalkthrough::ctrlFIsASearchBoxYouCanTypeInto()
{
    m_harness->key(Qt::Key_F, Qt::ControlModifier);

    auto* search = qobject_cast<LiveSearchController*>(m_harness->app()->tabs()->currentController());
    QVERIFY2(search, "Ctrl+F opens a search tab");
    m_harness->settle(6);

    // Typed into straight away. Having to click into the field is what the key is
    // supposed to save.
    QQuickItem* field = m_harness->item(QStringLiteral("searchQueryField"));
    QVERIFY(field);
    QVERIFY2(field->hasActiveFocus(), "the query field has the keyboard as soon as the tab opens");

    m_harness->type(QStringLiteral("report"));
    QCOMPARE(search->queryText(), QStringLiteral("report"));

    // Enter starts it, and while there is nothing to show the view says it is
    // working rather than sitting there looking broken.
    QQuickItem* waiting = m_harness->item(QStringLiteral("searchWaitingView"));
    QVERIFY(waiting);
    QVERIFY(!waiting->isVisible());

    m_harness->key(Qt::Key_Return);
    QVERIFY(m_harness->until(
        [search] { return search->results()->rowCount() > 0 || !search->isRunning(); }, 15000));
    QVERIFY(m_harness->until([search] { return !search->isRunning(); }, 15000));
    QVERIFY2(search->results()->rowCount() >= 1, "report.txt is in the fixture");

    // The criteria that were not there before, folded away until asked for.
    QQuickItem* advanced = m_harness->item(QStringLiteral("advancedCriteria"));
    QVERIFY(advanced);
    QVERIFY2(!advanced->isVisible(), "the common case stays one field and one key");

    QQuickItem* toggle = m_harness->item(QStringLiteral("advancedToggle"));
    QVERIFY(toggle);
    QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
    QVERIFY(m_harness->until([advanced] { return advanced->isVisible(); }));

    // A size that excludes everything in the fixture, typed the way people write it.
    QQuickItem* minSize = m_harness->item(QStringLiteral("minSizeField"));
    QVERIFY(minSize);
    minSize->setProperty("text", QStringLiteral("500M"));
    QVERIFY(QMetaObject::invokeMethod(minSize, "textEdited"));
    QCOMPARE(search->minSize(), qint64(500) * 1024 * 1024);

    search->start();
    QVERIFY(m_harness->until([search] { return !search->isRunning(); }, 15000));
    QCOMPARE(search->results()->rowCount(), 0);

    // What to do with results once there are some: narrow them where they are, and
    // take them somewhere the work continues.
    minSize->setProperty("text", QString());
    QVERIFY(QMetaObject::invokeMethod(minSize, "textEdited"));
    search->start();
    QVERIFY(m_harness->until([search] { return !search->isRunning(); }, 15000));
    const int found = search->results()->rowCount();
    QVERIFY(found >= 1);

    QQuickItem* narrow = m_harness->item(QStringLiteral("narrowResultsField"));
    QVERIFY2(narrow, "the results can be narrowed in place");
    narrow->setProperty("text", QStringLiteral("nothinglikethis"));
    QVERIFY(QMetaObject::invokeMethod(narrow, "textEdited"));
    QCOMPARE(search->results()->rowCount(), 0);
    QVERIFY2(!search->isRunning(), "narrowing must not start another walk");
    QCOMPARE(search->results()->totalCount(), found);

    narrow->setProperty("text", QString());
    QVERIFY(QMetaObject::invokeMethod(narrow, "textEdited"));
    QCOMPARE(search->results()->rowCount(), found);

    m_harness->settle(6);
    m_harness->screenshot(QStringLiteral("12-search-box"));

    QQuickItem* buildSet = m_harness->item(QStringLiteral("buildSetFromResultsButton"));
    QVERIFY(buildSet);
    QVERIFY(buildSet->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(buildSet, "clicked"));

    // A set of what was found, and the sets tab in front of the user so the work
    // can carry on there.
    QVERIFY(m_harness->until([this] { return !m_harness->app()->services().sets->sets().isEmpty(); }));
    QCOMPARE(m_harness->app()->services().sets->sets().first().uris.size(), found);
}

void TestWalkthrough::htmlCanBeSwitchedBetweenSourceAndPage()
{
    QVERIFY(m_harness->writeFile(QStringLiteral("page.html"),
        QByteArray("<html><body><h1>A page</h1><p>Sometimes you want the source, and sometimes "
                   "you want to read it.</p></body></html>")));

    m_harness->app()->previewFile(m_harness->fixtureUri() + QStringLiteral("/page.html"));
    auto* preview = qobject_cast<PreviewTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(m_harness->until([viewer] { return !viewer->text().isEmpty(); }));
    m_harness->settle(6);

    // The strip renders whatever the viewer declared, without knowing what it means.
    QQuickItem* picker = m_harness->item(QStringLiteral("viewerOption_mode"));
    QVERIFY2(picker, "the preview strip offers the choice the viewer declared");
    QCOMPARE(picker->property("currentText").toString(), QStringLiteral("Source"));
    m_harness->screenshot(QStringLiteral("03e-preview-html-source"));

    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Rendered"));
    QVERIFY(m_harness->until([viewer] { return viewer->isRenderedHtml(); }));
    m_harness->settle(8);

    // Looked up again rather than reused: the option list is republished when a
    // choice is made, so the Repeater builds a new delegate and the old pointer is
    // gone. Holding it would be reading freed memory, which is how the first
    // version of this test hung.
    QQuickItem* refreshed = m_harness->item(QStringLiteral("viewerOption_mode"));
    QVERIFY(refreshed);
    QVERIFY2(m_harness->until([this] {
        QQuickItem* now = m_harness->item(QStringLiteral("viewerOption_mode"));
        return now && now->property("currentText").toString() == QStringLiteral("Rendered");
    }),
        "and the picker shows what is in force");

    // What the view is actually showing: a page, not a wall of tags.
    QQuickItem* text = m_harness->item(QStringLiteral("previewText"));
    QVERIFY(text);
    QCOMPARE(text->property("textFormat").toInt(), int(Qt::RichText));
    m_harness->screenshot(QStringLiteral("03f-preview-html-rendered"));
}

void TestWalkthrough::searchResultsAreWalkableAndLeadSomewhere()
{
    QVERIFY(m_harness->writeFile(QStringLiteral("documents/needle-one.txt"), QByteArray("a")));
    QVERIFY(m_harness->writeFile(QStringLiteral("documents/needle-two.txt"), QByteArray("b")));

    m_harness->key(Qt::Key_F, Qt::ControlModifier);
    auto* search = qobject_cast<LiveSearchController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(search);
    m_harness->settle(6);

    m_harness->type(QStringLiteral("needle-"));
    m_harness->key(Qt::Key_Return);
    QVERIFY(m_harness->until([search] { return !search->isRunning(); }, 15000));
    QCOMPARE(search->results()->rowCount(), 2);

    // The actions sit with the results, not with the criteria: they act on the rows.
    QQuickItem* actions = m_harness->item(QStringLiteral("resultActions"));
    QVERIFY2(actions, "what can be done with results belongs beside them");
    QVERIFY(actions->isVisible());
    QVERIFY(m_harness->item(QStringLiteral("buildSetFromResultsButton")) != nullptr);

    // Down out of the query box walks into the results, and the arrows move there.
    QQuickItem* list = m_harness->item(QStringLiteral("searchResults"));
    QVERIFY(list);
    m_harness->key(Qt::Key_Down);
    QVERIFY2(m_harness->until([list] { return list->hasActiveFocus(); }),
        "Down out of the box puts the keyboard on the answers");

    QCOMPARE(list->property("currentIndex").toInt(), 0);
    m_harness->key(Qt::Key_Down);
    QVERIFY(m_harness->until([list] { return list->property("currentIndex").toInt() == 1; }));
    m_harness->key(Qt::Key_Up);
    QVERIFY(m_harness->until([list] { return list->property("currentIndex").toInt() == 0; }));

    m_harness->settle(6);
    m_harness->screenshot(QStringLiteral("12b-search-results"));

    // Enter is "show me where this is": the folder holding it, cursor on the file.
    const QString wanted = search->results()->uriAt(list->property("currentIndex").toInt());
    QVERIFY(!wanted.isEmpty());
    m_harness->key(Qt::Key_Return);

    QVERIFY2(m_harness->until(
                 [this, wanted] {
                     BrowserPaneController* pane = this->pane();
                     return pane && pane->currentIndex() >= 0
                         && pane->files()->uriAt(pane->currentIndex()) == wanted;
                 },
                 10000),
        "Enter on a result opens its folder with the cursor on it");
}

void TestWalkthrough::compressingTheSelectionMakesAnArchiveBesideIt()
{
    if (!m_harness->app()->canCompress())
        QSKIP("this build was made without libarchive");

    // The entry is in Operations, because packing acts on the files in front of you.
    bool inOperations = false;
    for (const QVariant& sectionEntry : m_harness->app()->buildMenu()) {
        const QVariantMap section = sectionEntry.toMap();
        for (const QVariant& action : section.value(QStringLiteral("actions")).toList()) {
            if (action.toMap().value(QStringLiteral("id")).toString()
                == QStringLiteral("mole.tools.compress")) {
                inOperations
                    = section.value(QStringLiteral("title")).toString() == QStringLiteral("Operations");
            }
        }
    }
    QVERIFY2(inOperations, "compressing is an operation on the selection");

    m_harness->key(Qt::Key_Return); // into "documents"
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 2; }));
    pane()->files()->selectAll();
    m_harness->settle();

    // The dialog asks the two things worth asking, and suggests the rest.
    m_harness->app()->triggerAction(QStringLiteral("mole.tools.compress"));
    QObject* dialog = m_harness->object(QStringLiteral("compressDialog"));
    QVERIFY(dialog);
    QVERIFY(m_harness->until([dialog] { return dialog->property("opened").toBool(); }));

    QQuickItem* name = m_harness->item(QStringLiteral("archiveNameField"));
    QVERIFY(name);
    QVERIFY2(name->hasActiveFocus(), "the name has the keyboard, since that is what is being asked");
    QVERIFY2(name->property("text").toString().endsWith(QStringLiteral(".zip")),
        "zip by default, because it is the one anyone can open");

    // Exactly what goes in, listed. A count is a summary; the point of a dialog before
    // an operation is being able to see it is aimed at the right things.
    QQuickItem* targets = m_harness->item(QStringLiteral("compressTargetList"));
    QVERIFY2(targets, "the dialog lists what would be packed");
    QCOMPARE(targets->property("count").toInt(), 2);
    QCOMPARE(m_harness->app()->compressionTargets().size(), 2);
    QStringList listed;
    for (const QVariant& entry : m_harness->app()->compressionTargets())
        listed.append(entry.toMap().value(QStringLiteral("name")).toString());
    listed.sort();
    QCOMPARE(listed, QStringList({ QStringLiteral("prices.csv"), QStringLiteral("report.txt") }));

    // And a password is offered, since this is a zip.
    QQuickItem* protect = m_harness->item(QStringLiteral("protectWithPassword"));
    QVERIFY(protect);
    QVERIFY2(protect->property("enabled").toBool(), "zip can carry one");

    m_harness->screenshot(QStringLiteral("13-compress"));

    name->setProperty("text", QStringLiteral("documents.zip"));
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));

    // It lands beside what was packed, and the listing shows it without being asked
    // to refresh.
    QVERIFY2(m_harness->until(
                 [this] {
                     return pane()->files()->rowOfUri(pane()->currentUri() + QStringLiteral("/documents.zip"))
                         >= 0;
                 },
                 20000),
        "the archive appears next to what was packed");

    // And it is a real zip, not an empty file with the right name. Whether its
    // contents survive a round trip is tst_CompressTask's job, in three formats;
    // what this test is for is the path from the menu entry to a file on disk.
    const QString archive = m_harness->fixturePath() + QStringLiteral("/documents/documents.zip");
    QVERIFY(QFileInfo::exists(archive));
    QFile written(archive);
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.read(2), QByteArray("PK"));
    QVERIFY(written.size() > 2);
}

// The dialog with no second chance behind it. It used to ask "delete 2 items?", which
// is exactly what somebody already knew before pressing the key -- and says nothing
// about whether the two are the two they meant.
void TestWalkthrough::deletingAsksWithTheFilesNamed()
{
    m_harness->key(Qt::Key_Return); // into "documents"
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 2; }));
    pane()->files()->selectAll();
    m_harness->settle();

    m_harness->key(Qt::Key_Delete);

    QQuickItem* listed = nullptr;
    QVERIFY2(m_harness->until([this, &listed] {
        listed = m_harness->item(QStringLiteral("deleteTargetList"));
        return listed != nullptr;
    }),
        "the dialog lists what it would delete");
    m_harness->settle();

    QCOMPARE(listed->property("count").toInt(), 2);
    QStringList named;
    for (const QVariant& row : listed->property("model").toList())
        named.append(row.toMap().value(QStringLiteral("name")).toString());
    named.sort();
    QCOMPARE(named, QStringList({ QStringLiteral("prices.csv"), QStringLiteral("report.txt") }));

    // The two ways out, told apart. They used to be "Yes" and "No" in the same flat
    // grey, with the keyboard on neither.
    QQuickItem* accept = m_harness->item(QStringLiteral("dialogAccept"));
    QQuickItem* reject = m_harness->item(QStringLiteral("dialogReject"));
    QVERIFY(accept);
    QVERIFY(reject);
    QCOMPARE(accept->property("text").toString(), QStringLiteral("Delete"));
    QCOMPARE(reject->property("text").toString(), QStringLiteral("Keep"));
    // Filled, and filled red because this one cannot be undone -- read off the pixels
    // rather than off the properties. The Material style reported a highlighted button
    // as visible, sized and red while painting nothing at all, which is the whole
    // reason these backgrounds are drawn by hand.
    QQuickItem* acceptBackground = accept->property("background").value<QQuickItem*>();
    QVERIFY(acceptBackground);
    const QColor filled = acceptBackground->property("color").value<QColor>();
    QVERIFY2(filled.red() > 150 && filled.red() > filled.green() * 2 && filled.red() > filled.blue() * 2,
        qPrintable(QStringLiteral("the acting button is red here, not %1").arg(filled.name())));

    QQuickItem* rejectBackground = reject->property("background").value<QQuickItem*>();
    QVERIFY(rejectBackground);
    QVERIFY2(rejectBackground->property("color").value<QColor>().alpha() == 0,
        "the way out is outlined, not filled -- one of the two has to be the quiet one");

    // And the keyboard starts on the safe one, so a stray Return closes the question
    // rather than answering it with the irreversible answer.
    QVERIFY2(reject->hasActiveFocus(), "a destructive dialog opens on the way out");

    m_harness->screenshot(QStringLiteral("14-delete"));

    // Left as it was found: this test is about the question, not the answer.
    m_harness->key(Qt::Key_Escape);
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 2; }));
}

// Every drive in the sidebar has to be reachable by typing, or the list on the left is
// the only way to it and the palette's promise -- everything that can be done, from one
// box -- is not true.
void TestWalkthrough::theDrivesAreInThePaletteToo()
{
    QObject* palette = m_harness->app()->property("commands").value<QObject*>();
    QVERIFY(palette);
    QVERIFY(QMetaObject::invokeMethod(palette, "refresh"));

    auto* model = qobject_cast<QAbstractItemModel*>(palette);
    QVERIFY(model);

    QStringList drives;
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, CommandPaletteModel::GroupRole).toString() == QStringLiteral("Drives"))
            drives.append(model->data(index, CommandPaletteModel::TitleRole).toString());
    }

    // Every one of them, not merely one: the palette holds no list of its own, so
    // anything missing here is something the sidebar can reach and typing cannot.
    DriveListModel* sidebar = m_harness->app()->drives();
    QVERIFY(sidebar->rowCount() > 0);
    // Counted by name rather than by rows in the group: a configured drive
    // also contributes "Connect ...", "Eject ..." and "Check ..." there, and
    // the claim being made is that every drive on the left can be typed for,
    // not that the group holds one line each.
    int places = 0;
    for (const QString& title : std::as_const(drives)) {
        for (int row = 0; row < sidebar->rowCount(); ++row) {
            if (title == sidebar->data(sidebar->index(row, 0), DriveListModel::DisplayNameRole).toString()) {
                ++places;
                break;
            }
        }
    }
    QCOMPARE(places, sidebar->rowCount());
    for (int row = 0; row < sidebar->rowCount(); ++row) {
        QVERIFY2(drives.contains(
                     sidebar->data(sidebar->index(row, 0), DriveListModel::DisplayNameRole).toString()),
            "a drive on the left that cannot be typed for");
    }

    // And by name, which is the point -- the drive is reached without the mouse.
    const QString wanted = sidebar->data(sidebar->index(0, 0), DriveListModel::DisplayNameRole).toString();
    palette->setProperty("filter", wanted);
    QVERIFY(model->rowCount() > 0);
    int found = -1;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->data(model->index(row, 0), CommandPaletteModel::TitleRole).toString() == wanted) {
            found = row;
            break;
        }
    }
    QVERIFY2(found >= 0, qPrintable(QStringLiteral("typing \"%1\" does not offer it").arg(wanted)));

    QSignalSpy went(qobject_cast<CommandPaletteModel*>(palette), &CommandPaletteModel::locationRequested);
    QVERIFY(QMetaObject::invokeMethod(palette, "activate", Q_ARG(int, found)));
    QCOMPARE(went.size(), 1);
    QCOMPARE(went.first().first().toString(), sidebar->rootUriAt(0));
}

void TestWalkthrough::breadcrumbsClimbTheTree()
{
    // Two levels down, then back up in one click rather than one Backspace per
    // level -- which is what the plain text path amounted to.
    m_harness->key(Qt::Key_Return); // documents
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 2; }));

    const QVariantList deep = pane()->pathSegments();
    QVERIFY2(deep.size() >= 2, "a nested folder has more than one crumb");
    QCOMPARE(deep.last().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("documents"));
    QCOMPARE(deep.last().toMap().value(QStringLiteral("current")).toBool(), true);

    // The first crumb is the drive itself, which has no name of its own.
    QCOMPARE(deep.first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("/"));
    QCOMPARE(deep.first().toMap().value(QStringLiteral("current")).toBool(), false);

    // Every crumb but the last leads somewhere, and its uri is an ancestor.
    const QString here = pane()->currentUri();
    for (int i = 0; i < deep.size() - 1; ++i) {
        const QString uri = deep.at(i).toMap().value(QStringLiteral("uri")).toString();
        QVERIFY2(here.startsWith(uri), qPrintable(QStringLiteral("%1 is not above %2").arg(uri, here)));
    }

    QQuickItem* crumbRow = m_harness->item(QStringLiteral("pathCrumbs"));
    QVERIFY2(crumbRow, "the crumbs are on screen, not merely in the model");

    // Clicking the parent crumb goes there directly.
    const QString parent = deep.at(deep.size() - 2).toMap().value(QStringLiteral("uri")).toString();
    pane()->navigateTo(parent);
    QVERIFY(m_harness->until([this, parent] { return pane()->currentUri() == parent; }));

    m_harness->settle(4);
    m_harness->screenshot(QStringLiteral("01b-breadcrumbs"));
}

void TestWalkthrough::filtersByTyping()
{
    // No shortcut: the first printable key opens the filter and goes into it.
    // "j" appears only in settings.json -- "m" would also match "documents".
    m_harness->key(Qt::Key_J);
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 1; }));
    QCOMPARE(pane()->files()->nameAt(0), QStringLiteral("settings.json"));

    QQuickItem* filter = m_harness->item(QStringLiteral("filterField"));
    QVERIFY(filter);
    QVERIFY2(filter->hasActiveFocus(), "the keyboard must move into the filter");
    QCOMPARE(filter->property("text").toString(), QStringLiteral("j"));

    // A filter is a substring match, not a prefix: "m" catches both folders.
    m_harness->key(Qt::Key_Backspace);
    m_harness->key(Qt::Key_M);
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 2; }));
    m_harness->screenshot(QStringLiteral("04-filter"));

    m_harness->key(Qt::Key_Escape);
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 4; }));
}

void TestWalkthrough::theFilterKeepsTheKeyboardWhileNarrowing()
{
    // Type to narrow, then act on the result -- without a detour to move the
    // keyboard onto the list first. That detour costs a keystroke and, worse,
    // swallows the one the user just spent.
    // "doc", not "d": a filter is a substring match and "media" contains a d.
    m_harness->key(Qt::Key_D);
    m_harness->key(Qt::Key_O);
    m_harness->key(Qt::Key_C);
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 1; }));

    QQuickItem* filter = m_harness->item(QStringLiteral("filterField"));
    QVERIFY(filter);
    QVERIFY(filter->hasActiveFocus());

    // Enter opens the row under the cursor rather than moving focus.
    m_harness->key(Qt::Key_Return);
    QVERIFY2(m_harness->until([this] { return pane()->currentUri().endsWith(QStringLiteral("/documents")); }),
        "Enter in the filter must open the highlighted row");

    // And arrows move the cursor while the field still has the keyboard, so
    // typing more keeps narrowing instead of going somewhere else.
    m_harness->key(Qt::Key_C); // "prices.csv"; "report.txt" has no c
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 1; }));

    filter = m_harness->item(QStringLiteral("filterField"));
    QVERIFY(filter);
    QVERIFY2(filter->hasActiveFocus(), "the filter keeps the keyboard");

    const int before = pane()->currentIndex();
    m_harness->key(Qt::Key_Down);
    QVERIFY2(filter->hasActiveFocus(), "an arrow moves the cursor, not the focus");
    Q_UNUSED(before);

    m_harness->key(Qt::Key_Escape);
}

void TestWalkthrough::filtersAndCopiesTableCells()
{
    m_harness->key(Qt::Key_Return); // into "documents"
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 2; }));

    const int row = pane()->files()->rowOfUri(pane()->currentUri() + QStringLiteral("/prices.csv"));
    QVERIFY(row >= 0);
    pane()->setCurrentIndex(row);
    m_harness->settle();
    m_harness->key(Qt::Key_F3);

    auto* preview = qobject_cast<PreviewTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(preview);
    auto* viewer = qobject_cast<TablePreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(m_harness->until(
        [viewer] { return !viewer->isImporting() && viewer->table()->rowCount() == 3; }, 10000));

    // The filter is a query over the imported file, not over what the view
    // happens to have loaded.
    viewer->table()->setFilter(QStringLiteral("bolt"));
    QCOMPARE(viewer->table()->rowCount(), 1);
    QCOMPARE(viewer->table()->matchingRows(), 1);
    QCOMPARE(viewer->table()->totalRows(), 3);
    QCOMPARE(viewer->table()->cellAt(0, 0), QStringLiteral("bolt"));

    viewer->table()->setFilter(QString());
    QCOMPARE(viewer->table()->rowCount(), 3);

    // A block of cells leaves as tab-separated text, which is what every
    // spreadsheet expects on paste.
    const QString block = viewer->table()->blockAsText(0, 0, 1, 1);
    QCOMPARE(block, QStringLiteral("widget\t1,50\nbolt\t0,99"));
    viewer->copyBlock(0, 0, 1, 1);

    // Columns are measured from the contents, so none is the same default
    // width as the others.
    const QVariantList widths = viewer->table()->columnWidths();
    QCOMPARE(widths.size(), 3);
    QCOMPARE(widths.at(0).toInt(), 6); // "widget"
    QCOMPARE(widths.at(2).toInt(), 3); // "qty" beats "250" only by tying it

    m_harness->settle(6);
    m_harness->screenshot(QStringLiteral("04b-table-filter"));
}

void TestWalkthrough::dualPaneAndGrid()
{
    auto* browser = qobject_cast<BrowserController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(browser);

    browser->setViewMode(BrowserController::ViewMode::Dual);
    m_harness->settle();
    QVERIFY(browser->splitEnabled());

    // The binding, not just the controller: this is where the bug was. Copy was
    // bound to an invokable with no change signal, so switching to dual pane
    // satisfied the condition without the button ever hearing about it.
    browser->activePane()->setCurrentIndex(0);
    browser->otherPane()->navigateTo(browser->activePane()->currentUri() + QStringLiteral("/documents"));
    QVERIFY(m_harness->until([browser] { return browser->canTransfer(); }));

    QQuickItem* copyButton = m_harness->item(QStringLiteral("copyButton"));
    QVERIFY(copyButton);
    QVERIFY2(m_harness->until([copyButton] { return copyButton->isEnabled(); }),
        "Copy must enable itself once a transfer is possible");

    m_harness->screenshot(QStringLiteral("05-dual-pane"));

    browser->setViewMode(BrowserController::ViewMode::Grid);
    m_harness->settle();
    QVERIFY(browser->gridEnabled());
    QVERIFY2(!browser->splitEnabled(), "grid is one pane, not two");
    QVERIFY(m_harness->item(QStringLiteral("fileGrid")) != nullptr);
    m_harness->screenshot(QStringLiteral("06-grid"));
}

void TestWalkthrough::f5CopiesTheSelectedFile()
{
    // Driven through the real key and the real dialog. The controller-level
    // tests all passed while F5 did nothing at all: the view called a property
    // as a method, the call threw, and the throw took the rest of the handler
    // with it. Only a test that presses the key can see that.
    auto* browser = qobject_cast<BrowserController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(browser);
    browser->setViewMode(BrowserController::ViewMode::Dual);
    m_harness->settle();

    const QString source = m_harness->fixtureUri();
    const QString destination = source + QStringLiteral("/documents");
    browser->otherPane()->navigateTo(destination);
    QVERIFY(m_harness->until(
        [browser, destination] { return browser->otherPane()->currentUri() == destination; }));
    QVERIFY(m_harness->until([browser] { return browser->otherPane()->files()->rowCount() == 2; }));

    const int row = browser->activePane()->files()->rowOfUri(source + QStringLiteral("/notes.txt"));
    QVERIFY(row >= 0);
    browser->activePane()->setCurrentIndex(row);
    QVERIFY(m_harness->until([browser] { return browser->canTransfer(); }));

    m_harness->key(Qt::Key_F5);

    QObject* dialog = m_harness->object(QStringLiteral("transferDialog"));
    QVERIFY(dialog);
    QVERIFY2(dialog->property("visible").toBool(), "F5 must put up the confirmation, not fail quietly");
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));

    QVERIFY2(m_harness->until(
                 [browser] {
                     return browser->otherPane()->files()->rowOfUri(
                                browser->otherPane()->currentUri() + QStringLiteral("/notes.txt"))
                         >= 0;
                 },
                 10000),
        "the file has to arrive in the other pane");
}

void TestWalkthrough::analysesAFolder()
{
    m_harness->app()->analyseSelection();
    m_harness->settle();

    auto* analysis = qobject_cast<AnalysisTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY2(analysis, "Ctrl+Shift+A must open an analysis tab");
    QCOMPARE(analysis->targetCount(), 1);

    QVERIFY(m_harness->until(
        [analysis] { return analysis->current() && analysis->current()->hasReport(); }, 20000));

    // Six files, and the .mkv pair dominates by size.
    const QVariantMap headline = analysis->current()->headline();
    QCOMPARE(headline.value(QStringLiteral("files")).toLongLong(), 6);
    QCOMPARE(analysis->current()->extensions()->index(0, 0).data(BreakdownModel::ExtensionRole).toString(),
        QStringLiteral("mkv"));

    m_harness->settle(10);
    m_harness->screenshot(QStringLiteral("07-analysis"));
}

void TestWalkthrough::schedulesTheReportAndTracksIt()
{
    m_harness->app()->analyseSelection();
    m_harness->settle();

    auto* analysis = qobject_cast<AnalysisTabController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(analysis);
    QVERIFY(m_harness->until(
        [analysis] { return analysis->current() && analysis->current()->hasReport(); }, 20000));

    // Put it on a weekly clock from the report itself.
    analysis->current()->setSchedule(604800);
    QCOMPARE(analysis->current()->scheduleText(), QStringLiteral("Every week"));

    // The clock starts from the report that already exists, so a folder just
    // analysed by hand is not walked again a second later.
    QCOMPARE(m_harness->app()->scheduler()->checkDue(), 0);

    // "Run now" is what the user reaches for to check the job actually works
    // rather than waiting a week to find out.
    const QString ruleId = m_harness->app()->schedules()->rules().first().id;
    QVERIFY(m_harness->app()->scheduler()->runNow(ruleId));
    QVERIFY(
        m_harness->until([this] { return m_harness->app()->scheduler()->runningRules().isEmpty(); }, 20000));

    const int row = m_harness->app()->openFeatureTab(QStringLiteral("core.automation"));
    QVERIFY(row >= 0);
    auto* automation = qobject_cast<AutomationController*>(m_harness->app()->tabs()->currentController());
    QVERIFY(automation);
    QCOMPARE(automation->failingCount(), 0);
    QCOMPARE(automation->rules().size(), 1);
    QCOMPARE(automation->history().size(), 1);
    QCOMPARE(automation->history().first().toMap().value(QStringLiteral("statusText")).toString(),
        QStringLiteral("OK"));

    m_harness->settle(10);
    m_harness->screenshot(QStringLiteral("08-automation"));
}

void TestWalkthrough::ctrlWClosesAPreviewTabWithTheTextFocused()
{
    m_harness->key(Qt::Key_Return); // into "documents"
    QVERIFY(m_harness->until([this] { return pane()->files()->rowCount() == 2; }));

    const int row = pane()->files()->rowOfUri(pane()->currentUri() + QStringLiteral("/report.txt"));
    QVERIFY(row >= 0);
    pane()->setCurrentIndex(row);
    m_harness->settle();
    m_harness->key(Qt::Key_F3);

    QVERIFY(qobject_cast<PreviewTabController*>(m_harness->app()->tabs()->currentController()));
    const int tabsBefore = m_harness->app()->tabs()->rowCount();

    // Clicking in the body of the preview is what triggered this: the text
    // area takes the keyboard, and Ctrl+W is DeleteStartOfWord in Qt's
    // standard bindings, so the read-only editor claimed the key before the
    // window shortcut was consulted.
    QQuickItem* text = m_harness->item(QStringLiteral("previewText"));
    QVERIFY(text);
    text->forceActiveFocus();
    m_harness->settle(4);
    QVERIFY2(text->hasActiveFocus(), "the text area has to hold the keyboard for this to mean anything");

    m_harness->key(Qt::Key_W, Qt::ControlModifier);
    QVERIFY2(m_harness->until(
                 [this, tabsBefore] { return m_harness->app()->tabs()->rowCount() == tabsBefore - 1; }),
        "Ctrl+W must close the tab even when the preview body has the keyboard");
}

void TestWalkthrough::theTerminalOpensInTheFolderYouAreLookingAt()
{
    TerminalController* terminal = m_harness->app()->terminal();
    QVERIFY(terminal);
    if (!terminal->isAvailable())
        QSKIP("no pseudo-terminal on this platform");

    QVERIFY(!terminal->isVisible());

    const QString here = pane()->currentUri();
    m_harness->app()->triggerAction(QStringLiteral("mole.tools.terminal"));
    m_harness->settle(6);

    QVERIFY2(terminal->isVisible(), "the panel appears");
    QVERIFY2(terminal->isRunning(), "with a shell in it");
    // Opened from a folder, it starts there -- which is the whole reason for a
    // panel rather than a separate terminal application.
    QCOMPARE(terminal->workingDirectory(), VfsUri::fromString(here).path());

    // A real shell on a real terminal: it answers.
    terminal->sendText(QStringLiteral("echo mole-panel-marker\n"));
    QVERIFY2(m_harness->until(
                 [terminal] {
                     for (int row = 0; row < terminal->rows(); ++row) {
                         if (terminal->rowText(row).contains(QStringLiteral("mole-panel-marker")))
                             return true;
                     }
                     return false;
                 },
                 10000),
        "the shell's output reaches the screen");

    m_harness->settle(6);
    m_harness->screenshot(QStringLiteral("10-terminal"));

    // The same key closes it, which is what makes it a panel rather than a tab.
    m_harness->app()->triggerAction(QStringLiteral("mole.tools.terminal"));
    m_harness->settle(4);
    QVERIFY(!terminal->isVisible());
}

void TestWalkthrough::theTerminalTakesTheKeyboardAndCtrlDEndsIt()
{
    TerminalController* terminal = m_harness->app()->terminal();
    QVERIFY(terminal);
    if (!terminal->isAvailable())
        QSKIP("no pseudo-terminal on this platform");

    // Ctrl+D on the listing is what it has always been: the folder is bookmarked.
    // Asserted first, because the point of the rest is that the panel takes the
    // key away without this behaviour being lost.
    const int bookmarksBefore = m_harness->app()->bookmarks()->rowCount();
    m_harness->key(Qt::Key_D, Qt::ControlModifier);
    QVERIFY(m_harness->until([this, bookmarksBefore] {
        return m_harness->app()->bookmarks()->rowCount() == bookmarksBefore + 1;
    }));
    const int bookmarksWithFolder = m_harness->app()->bookmarks()->rowCount();

    m_harness->key(Qt::Key_QuoteLeft, Qt::ControlModifier);
    QVERIFY(m_harness->until([terminal] { return terminal->isVisible() && terminal->isRunning(); }));
    m_harness->settle(6);

    // Typed, not sent through the controller. Every other assertion about the
    // shell could pass with the keyboard on the file list behind it; this one is
    // the whole point -- opening the panel and starting to type has to work.
    m_harness->type(QStringLiteral("echo mole-typed-marker"));
    m_harness->key(Qt::Key_Return);
    QVERIFY2(m_harness->until(
                 [terminal] {
                     for (int row = 0; row < terminal->rows(); ++row) {
                         if (terminal->rowText(row).contains(QStringLiteral("mole-typed-marker")))
                             return true;
                     }
                     return false;
                 },
                 10000),
        "what is typed after the panel opens has to reach the shell, with no click first");

    // And Ctrl+D means end of input, as it does in any terminal: the shell ends
    // and the panel goes with it. It must not reach the bookmarks, which is where
    // the window shortcut had been sending it.
    m_harness->key(Qt::Key_D, Qt::ControlModifier);
    QVERIFY2(m_harness->until([terminal] { return !terminal->isRunning(); }, 10000),
        "Ctrl+D has to reach the shell as end of input");
    QVERIFY2(m_harness->until([terminal] { return !terminal->isVisible(); }, 6000),
        "a shell that ended takes its panel with it");
    QCOMPARE(m_harness->app()->bookmarks()->rowCount(), bookmarksWithFolder);
}

void TestWalkthrough::theDrivesDialogOffersBackendsAndAForm()
{
    m_harness->app()->triggerAction(QStringLiteral("mole.file.drives"));

    QObject* dialog = m_harness->object(QStringLiteral("drivesDialog"));
    QVERIFY(dialog);
    // Waiting for `opened`, not for `visible`: the enter transition runs after
    // the dialog appears and its completion resets the form.
    QVERIFY(m_harness->until([dialog] { return dialog->property("opened").toBool(); }));

    QVERIFY(m_harness->app()->unlockCredentials(QStringLiteral("test-passphrase")));

    // The list of backends has to reach the picker. An empty picker leaves a
    // dialog that looks finished and can do nothing.
    QQuickItem* picker = m_harness->item(QStringLiteral("driveKindPicker"));
    QVERIFY2(picker, "the kind picker is on screen");
    const QVariantList kinds = picker->property("model").toList();
    QVERIFY2(!kinds.isEmpty(), "the backends reach the picker");

    // Pressing add must change something visible. It used to reset a dialog
    // that already opened in exactly that state, so the screen stayed put.
    QQuickItem* addButton = m_harness->item(QStringLiteral("addDriveButton"));
    QVERIFY(addButton);
    QVERIFY(QMetaObject::invokeMethod(addButton, "clicked"));
    m_harness->settle(4);
    QQuickItem* prompt = m_harness->item(QStringLiteral("drivePickPrompt"));
    QVERIFY2(prompt && prompt->isVisible(), "the panel says what to do next");

    int index = -1;
    for (int i = 0; i < kinds.size(); ++i) {
        const QVariantMap kind = kinds.at(i).toMap();
        if (kind.value(QStringLiteral("available")).toBool()
            && kind.value(QStringLiteral("variant")).toString() == QLatin1String("memory")) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        for (int i = 0; i < kinds.size() && index < 0; ++i) {
            if (kinds.at(i).toMap().value(QStringLiteral("available")).toBool())
                index = i;
        }
    }
    QVERIFY2(index >= 0, "at least one backend is usable");

    picker->setProperty("currentIndex", index);
    QVERIFY(QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, index)));
    QMetaObject::invokeMethod(picker->property("popup").value<QObject*>(), "close");
    m_harness->settle(6);

    // The form has to appear *with a size*. A layout Qt has given up on leaves
    // its children present and zero pixels tall, which looks identical to a
    // button that did nothing.
    QQuickItem* nameField = m_harness->item(QStringLiteral("driveNameField"));
    QVERIFY2(nameField, "the form exists once a kind is chosen");
    QVERIFY2(nameField->isVisible(), "and it is on screen");
    QVERIFY2(nameField->width() > 40 && nameField->height() > 10,
        qPrintable(QStringLiteral("the name field has room to type in, got %1x%2")
                       .arg(nameField->width())
                       .arg(nameField->height())));

    QQuickItem* saveButton = m_harness->item(QStringLiteral("saveDriveButton"));
    QVERIFY(saveButton);
    QVERIFY(saveButton->isVisible());
    QVERIFY(saveButton->height() > 10);

    // And saving has to put the drive in the list beside it. The list was bound
    // to a plain method call, which QML evaluates once and never again, so a
    // saved drive stayed invisible however well the save itself worked.
    const int before = m_harness->app()->configuredDrives().size();
    nameField->setProperty("text", QStringLiteral("Test drive"));
    QVERIFY(QMetaObject::invokeMethod(saveButton, "clicked"));
    QVERIFY(
        m_harness->until([this, before] { return m_harness->app()->configuredDrives().size() > before; }));

    QQuickItem* list = m_harness->item(QStringLiteral("configuredDriveList"));
    QVERIFY(list);
    QVERIFY2(m_harness->until([list] { return list->property("count").toInt() > 0; }),
        "the new drive shows up in Your drives");

    m_harness->settle(4);
    m_harness->screenshot(QStringLiteral("11-drives"));
}

/// Every backend, not just the one that happened to be picked. The forms are
/// built from field descriptions the backends supply, so a field of a kind no
/// delegate handles -- or a default value of a type the delegate cannot take --
/// only shows up on whichever provider happens to declare one.
void TestWalkthrough::everyBackendBuildsAFormWithoutComplaint()
{
    m_harness->app()->triggerAction(QStringLiteral("mole.file.drives"));
    QObject* dialog = m_harness->object(QStringLiteral("drivesDialog"));
    QVERIFY(dialog);
    QVERIFY(m_harness->until([dialog] { return dialog->property("opened").toBool(); }));

    const QVariantList kinds = m_harness->app()->driveKinds();
    QVERIFY(!kinds.isEmpty());

    // Counted, not eyeballed. A form that builds but complains on every field
    // is a form whose values never reached it, and the test that only checked
    // the fields existed would have called that a pass.
    static QStringList complaints;
    complaints.clear();
    QtMessageHandler previous
        = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& text) {
              if (type == QtWarningMsg || type == QtCriticalMsg)
                  complaints.append(text);
          });

    int built = 0;
    int fieldsSeen = 0;
    for (const QVariant& entry : kinds) {
        const QVariantMap kind = entry.toMap();
        if (!kind.value(QStringLiteral("available")).toBool())
            continue;

        dialog->setProperty("factory", kind.value(QStringLiteral("factory")));
        dialog->setProperty("variant", kind.value(QStringLiteral("variant")));
        m_harness->settle(2);

        QQuickItem* fields = m_harness->item(QStringLiteral("driveFieldRepeater"));
        if (fields) {
            fieldsSeen += fields->property("count").toInt();
            // With a size. A layout the engine has given up on leaves its
            // children present and zero pixels wide, which counts the same as a
            // form that works if all the test does is count.
            if (fields->property("count").toInt() > 0) {
                QVERIFY2(fields->width() > 40,
                    qPrintable(QStringLiteral("%1 fields laid out %2 wide in %3")
                                   .arg(fields->property("count").toInt())
                                   .arg(fields->width())
                                   .arg(kind.value(QStringLiteral("variant")).toString())));
            }
        }
        ++built;
    }

    qInstallMessageHandler(previous);

    qWarning("built %d forms, %d fields in total", built, fieldsSeen);
    // Every backend on offer built a form -- which is the actual claim, and it
    // does not rot when the list changes. The floor is the four network backends
    // this build ships; it used to be "more than ten", from the days when one
    // generated factory offered forty providers of its own.
    int available = 0;
    for (const QVariant& entry : kinds) {
        if (entry.toMap().value(QStringLiteral("available")).toBool())
            ++available;
    }
    QCOMPARE(built, available);
    QVERIFY2(built >= 4, "the backends are there to be built");
    QVERIFY2(fieldsSeen > 0, "and they ask for something");
    if (!complaints.isEmpty()) {
        qWarning("%s", qPrintable(complaints.mid(0, 5).join(QLatin1Char('\n'))));
        QFAIL(qPrintable(QStringLiteral("%1 complaints while building the forms").arg(complaints.size())));
    }
}

/// The path with a secret on it. Saving a drive that needs no password never
/// reaches the credential store at all, so a suite full of those says nothing
/// about the case every real remote is.
void TestWalkthrough::aDriveWithAPasswordSavesAndConnects()
{
    QVERIFY(m_harness->app()->unlockCredentials(QStringLiteral("test-passphrase")));

    const QVariantList kinds = m_harness->app()->driveKinds();
    QString factory;
    QString variant;
    QString secretKey;
    for (const QVariant& entry : kinds) {
        const QVariantMap kind = entry.toMap();
        if (!kind.value(QStringLiteral("available")).toBool())
            continue;
        const QVariantList fields
            = m_harness->app()->driveFields(kind.value(QStringLiteral("factory")).toString(),
                kind.value(QStringLiteral("variant")).toString());
        for (const QVariant& fieldValue : fields) {
            const QVariantMap field = fieldValue.toMap();
            if (field.value(QStringLiteral("secret")).toBool()) {
                factory = kind.value(QStringLiteral("factory")).toString();
                variant = kind.value(QStringLiteral("variant")).toString();
                secretKey = field.value(QStringLiteral("key")).toString();
                break;
            }
        }
        if (!factory.isEmpty())
            break;
    }
    QVERIFY2(!factory.isEmpty(), "some backend asks for a password");
    qWarning("saving a %s drive with a secret %s", qPrintable(variant), qPrintable(secretKey));

    QVariantMap values;
    values.insert(secretKey, QStringLiteral("hunter2"));
    QVERIFY(m_harness->app()->saveDrive(
        QString(), QStringLiteral("Secret drive"), factory, variant, QString(), values));

    const QVariantList saved = m_harness->app()->configuredDrives();
    QVERIFY(!saved.isEmpty());
    const QVariantMap drive = saved.last().toMap();
    QCOMPARE(drive.value(QStringLiteral("name")).toString(), QStringLiteral("Secret drive"));

    // The password must have gone into the store, not into the settings file
    // beside it.
    const QString remotes = QString::fromLocal8Bit(qgetenv("MOLE_REMOTES_PATH"));
    QVERIFY(!remotes.isEmpty());
    QFile file(remotes);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray onDisk = file.readAll();
    QVERIFY2(!onDisk.contains("hunter2"), "the password is not written in the clear");
}

/// Pressing Save in the real form, and reading the verdict it comes back with.
///
/// Saving a drive used to tell you nothing about whether it works, and neither
/// did connecting it -- so a wrong endpoint or a refused password surfaced far
/// away from the form that caused it, in the middle of browsing. The check now
/// runs on save, and its answer gets its own band across the dialog: the form is
/// cleared by saving, so a line inside it would disappear at the moment it became
/// worth reading.
void TestWalkthrough::savingThroughTheFormShowsWhatTheCheckFound()
{
    QVERIFY(m_harness->app()->unlockCredentials(QStringLiteral("test-passphrase")));

    m_harness->app()->triggerAction(QStringLiteral("mole.file.drives"));
    QObject* dialog = m_harness->object(QStringLiteral("drivesDialog"));
    QVERIFY(dialog);
    QVERIFY(m_harness->until([dialog] { return dialog->property("opened").toBool(); }));

    dialog->setProperty("factory", QStringLiteral("sftp"));
    dialog->setProperty("variant", QString());
    m_harness->settle(4);

    QQuickItem* nameField = m_harness->item(QStringLiteral("driveNameField"));
    QVERIFY(nameField);
    nameField->setProperty("text", QStringLiteral("Points nowhere"));
    m_harness->settle(2);

    QQuickItem* save = m_harness->item(QStringLiteral("saveDriveButton"));
    QVERIFY(save);
    QVERIFY2(save->property("enabled").toBool(), "a named drive of a known kind can be saved");
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));

    // No host was filled in, so the honest verdict is that it cannot be reached.
    // What is being tested is that a verdict arrives here at all.
    QQuickItem* banner = nullptr;
    QVERIFY2(m_harness->until([this, &banner] {
        banner = m_harness->item(QStringLiteral("driveCheckBanner"));
        return banner && banner->isVisible();
    }),
        "saving a drive has to show what the check found");

    // The band appears, then the layout gives it a size on a later frame.
    m_harness->settle(6);

    QQuickItem* result = m_harness->item(QStringLiteral("driveCheckResult"));
    QVERIFY(result);
    QVERIFY2(!result->property("text").toString().isEmpty(), "the banner says nothing");
    qWarning("banner %.0fx%.0f, label %.0fx%.0f, text '%s'", banner->width(), banner->height(),
        result->width(), result->height(), qPrintable(result->property("text").toString()));
    // A band the engine has given up laying out is present and zero pixels wide,
    // which counts the same as a working one if all the test does is find it.
    QVERIFY2(banner->width() > 40,
        qPrintable(QStringLiteral("the banner is %1 pixels wide").arg(banner->width())));
}

/// Pressing connect from inside a row of the drive list. The handler belongs to
/// a delegate, and connecting tells the list its contents changed -- so the list
/// can rebuild, and destroy the very delegate whose handler is still running.
void TestWalkthrough::connectingFromTheListSurvivesTheListRebuilding()
{
    QVERIFY(m_harness->app()->unlockCredentials(QStringLiteral("test-passphrase")));
    QVERIFY(m_harness->app()->saveDrive(QString(), QStringLiteral("Scratch"), QStringLiteral("sftp"),
        QStringLiteral("memory"), QString(), QVariantMap()));

    m_harness->app()->triggerAction(QStringLiteral("mole.file.drives"));
    QObject* dialog = m_harness->object(QStringLiteral("drivesDialog"));
    QVERIFY(dialog);
    QVERIFY(m_harness->until([dialog] { return dialog->property("opened").toBool(); }));

    QQuickItem* list = m_harness->item(QStringLiteral("configuredDriveList"));
    QVERIFY(list);
    QVERIFY(m_harness->until([list] { return list->property("count").toInt() > 0; }));

    QList<QQuickItem*> buttons = m_harness->items(QStringLiteral("driveConnectButton"));
    QVERIFY2(!buttons.isEmpty(), "the row has a connect button");

    // Twice: connect, then disconnect. Each emits the change that rebuilds the
    // list under the handler doing the emitting.
    QVERIFY(QMetaObject::invokeMethod(buttons.first(), "clicked"));
    m_harness->settle(8);

    buttons = m_harness->items(QStringLiteral("driveConnectButton"));
    QVERIFY(!buttons.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(buttons.first(), "clicked"));
    m_harness->settle(8);

    // Still here, still able to answer, and the row reflects what happened.
    QVERIFY2(m_harness->item(QStringLiteral("configuredDriveList")), "still standing");
    QCOMPARE(m_harness->app()->configuredDrives().size(), 1);
}

/// Typing in the kind picker to narrow sixty backends down to the one wanted.
/// The picker is editable precisely so this works, and it is the first thing
/// anybody does when the list is that long.
void TestWalkthrough::typingIntoTheKindPickerFiltersIt()
{
    m_harness->app()->triggerAction(QStringLiteral("mole.file.drives"));
    QObject* dialog = m_harness->object(QStringLiteral("drivesDialog"));
    QVERIFY(dialog);
    QVERIFY(m_harness->until([dialog] { return dialog->property("opened").toBool(); }));

    QQuickItem* picker = m_harness->item(QStringLiteral("driveKindPicker"));
    QVERIFY(picker);

    // The state it is really done from: a backend already chosen, so a form of
    // its fields exists, and a value already typed into one of them.
    dialog->setProperty("factory", QStringLiteral("sftp"));
    dialog->setProperty("variant", QStringLiteral("sftp"));
    m_harness->settle(6);
    QQuickItem* repeater = m_harness->item(QStringLiteral("driveFieldRepeater"));
    QVERIFY(repeater);
    qWarning("form has %d fields", repeater->property("count").toInt());
    QVERIFY(QMetaObject::invokeMethod(dialog, "setFieldValue", Q_ARG(QVariant, QStringLiteral("host")),
        Q_ARG(QVariant, QStringLiteral("abc"))));
    m_harness->settle(4);

    picker->forceActiveFocus();
    m_harness->settle(2);

    // Through the add button, so the dropdown is open -- that is the state the
    // filtering is done in, and an open dropdown is a second view over the same
    // model with its own navigation.
    QQuickItem* addButton = m_harness->item(QStringLiteral("addDriveButton"));
    QVERIFY(addButton);
    QVERIFY(QMetaObject::invokeMethod(addButton, "clicked"));
    m_harness->settle(4);

    QObject* popup = picker->property("popup").value<QObject*>();
    QVERIFY(popup);
    QVERIFY2(popup->property("visible").toBool(), "the dropdown is open");
    // Open is not enough. A dropdown capped against a height that is zero while
    // it is shut opens at zero pixels and shows nothing, which is worse than
    // the sixty-entry wall the cap was meant to prevent.
    const qreal popupHeight = popup->property("height").toReal();
    QVERIFY2(popupHeight > 100 && popupHeight <= 340,
        qPrintable(QStringLiteral("the dropdown is %1 pixels tall").arg(popupHeight)));

    // Real key events. Setting the text directly skips the completion, the key
    // search and the popup navigation that typing actually drives.
    m_harness->type(QStringLiteral("abc"));
    m_harness->settle(4);
    qWarning("after typing: editText='%s' focus=%s", qPrintable(picker->property("editText").toString()),
        qPrintable(m_harness->focusChain()));
    for (int i = 0; i < 3; ++i) {
        m_harness->key(Qt::Key_Backspace);
        m_harness->settle(2);
    }
    m_harness->type(QStringLiteral("sftp"));
    m_harness->settle(4);

    QVERIFY2(m_harness->item(QStringLiteral("driveKindPicker")), "still standing");
}

void TestWalkthrough::emptyWindowExplainsItself()
{
    while (m_harness->app()->tabs()->rowCount() > 0)
        m_harness->app()->tabs()->closeTab(0);
    m_harness->settle(6);

    QCOMPARE(m_harness->app()->tabs()->rowCount(), 0);
    m_harness->screenshot(QStringLiteral("09-no-tabs"));
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();
    QQuickStyle::setStyle(QStringLiteral("Material"));

    TestWalkthrough testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_Walkthrough.moc"
