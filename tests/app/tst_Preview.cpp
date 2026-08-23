#include "host/MetadataRegistry.h"
#include "host/PreviewRegistry.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "plugins/builtin/PreviewFeature.h"
#include "plugins/builtin/previews/DocumentMetadata.h"
#include "plugins/builtin/previews/MarkdownStyle.h"
#include "plugins/builtin/previews/MetadataReaders.h"
#include "plugins/builtin/previews/PdfPreview.h"
#include "plugins/builtin/previews/PreviewProviders.h"
#include "plugins/builtin/previews/SyntaxHighlighter.h"
#include "plugins/builtin/previews/VideoPreview.h"
#include "support/FakePlugin.h"
#include "support/FaultyFileSystem.h"
#include "support/OfferingFileSystem.h"
#include "support/TableFixtures.h"
#include "support/TestSupport.h"
#include "support/ZipFixtures.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#ifdef MOLE_TEST_HAVE_ARCHIVE
#include "plugins/archive/ArchiveFileSystem.h"
#endif

#include "core/CoreMetaTypes.h"
#include "core/data/FileType.h"
#include "core/tasks/TaskManager.h"
#include "core/text/ImportJsonLinesTask.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextTable>

using namespace mole;
using namespace mole::test;

namespace {

/// One JSON object on a single line, `bytes` long, with no newline anywhere in
/// it -- the shape a minified export arrives in.
QByteArray minifiedJson(qsizetype bytes)
{
    QByteArray out = QByteArrayLiteral("{\"records\":[");
    for (int i = 0; out.size() < bytes; ++i) {
        out += QByteArrayLiteral("{\"id\":") + QByteArray::number(i)
            + QByteArrayLiteral(",\"name\":\"row\",\"ok\":true},");
    }
    out.truncate(bytes);
    Q_ASSERT(!out.contains('\n'));
    return out;
}

/// The same records with a line each, so a window of them is already blocks.
QByteArray linedJson(qsizetype bytes)
{
    QByteArray out;
    for (int i = 0; out.size() < bytes; ++i) {
        out += QByteArrayLiteral("  {\"id\": ") + QByteArray::number(i)
            + QByteArrayLiteral(", \"name\": \"row\", \"ok\": true},\n");
    }
    return out;
}

/// A file with no format anybody knows, and one recognisable string in it so a
/// selection has something to be about.
///
/// Not a run of one repeated byte, which is what this was first: 4096 bytes of
/// 0x01 match a TGA magic rule, so the "nothing can name this" fixture was
/// quietly a picture.
QByteArray unknownBinary(qsizetype bytes)
{
    QByteArray out("\x7f\x01\x02\x03MOLE-FIRMWARE\0\0", 19);
    while (out.size() < bytes)
        out += QByteArray::number(out.size(), 16).repeated(3) + QByteArray(5, static_cast<char>(0xa7));
    out.truncate(bytes);
    return out;
}

/// Files of three kinds Mole can name and has no viewer for. Only their headers
/// matter here: what is being tested is which viewer they reach.
QByteArray mp4File()
{
    QByteArray out(4, '\0');
    out[3] = char(0x20);
    out += QByteArrayLiteral("ftypisom") + QByteArray(4, '\0') + QByteArrayLiteral("isomiso2mp41");
    return out + QByteArray(2048, 'v');
}

QByteArray mp3File()
{
    QByteArray out = QByteArrayLiteral("ID3") + QByteArray(1, '\x04') + QByteArray(2, '\0')
        + QByteArray(4, '\0'); // an empty tag: a synchsafe zero
    QByteArray frame(4, '\0');
    frame[0] = char(0xff);
    frame[1] = char(0xfb);
    frame[2] = char(0x90);
    for (int i = 0; i < 8; ++i)
        out += frame + QByteArray(413, '\0');
    return out;
}

QByteArray docxFile()
{
    StoredZip zip;
    zip.add("[Content_Types].xml", "<Types/>");
    zip.add("docProps/core.xml",
        QByteArrayLiteral("<cp:coreProperties xmlns:cp=\"x\" xmlns:dc=\"y\">"
                          "<dc:creator>Ada Lovelace</dc:creator></cp:coreProperties>"));
    zip.add("word/document.xml", QByteArray(2048, 'w'));
    return zip.build();
}

/// The value of one fact in the panel, or empty when nothing said it.
QString detailNamed(const PreviewTabController* preview, const QString& label)
{
    const QVariantList details = preview->details();
    for (const QVariant& entry : details) {
        const QVariantMap fact = entry.toMap();
        if (fact.value(QStringLiteral("label")).toString() == label)
            return fact.value(QStringLiteral("value")).toString();
    }
    return {};
}

QStringList detailLabels(const PreviewTabController* preview)
{
    QStringList labels;
    const QVariantList details = preview->details();
    for (const QVariant& entry : details)
        labels.append(entry.toMap().value(QStringLiteral("label")).toString());
    return labels;
}

/// The longest run with no line break in it -- which is what the text engine is
/// handed as one block, and what has to be laid out whole.
qsizetype longestLine(const QString& text)
{
    qsizetype longest = 0;
    qsizetype run = 0;
    for (const QChar c : text) {
        if (c == u'\n') {
            run = 0;
            continue;
        }
        longest = std::max(longest, ++run);
    }
    return longest;
}

} // namespace

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

    // ---- identified by what is in it, not by what it is called -----------
    void aFileWhoseNameSaysNothingIsIdentifiedFromItsContents();
    void contentsOutrankAMisleadingName();
    void identifyingAFileIsOneReadOfOnePage();
    void everyFileKeepsTheViewerItAlreadyHad_data();
    void everyFileKeepsTheViewerItAlreadyHad();
    void theFactListNamesWhatTheFileTurnedOutToBe();

    // ---- what a file says about itself ------------------------------------
    void everyReaderThatClaimsAFileContributes();
    void nothingIsReadUntilTheDetailsAreOpened();
    void theDetailsAreOneSettingForEveryFile();
    void aValueCanBeCopiedOutOfTheDetails();
    void steppingToTheNextFileCancelsAReaderInFlight();
    void aReaderThatFailsCostsOnlyItsOwnRows();
    void aPhotographShowsThePictureAndWhatTheCameraWrote();
    void aDocumentNamesItsAuthorWithoutBeingReadWhole();

    // ---- the bytes of a file, for the files nothing else can show ---------
    void bytesAreShownForWhatNothingElseClaims_data();
    void bytesAreShownForWhatNothingElseClaims();
    void theBytesAreAChoiceOnEveryFileThatHasNoViewer();
    void theHexWindowIsOnePageOfSixtyFourKilobytes();
    void ahugeFileIsPagedAndItsTailIsReachable();
    void theOffsetColumnWidensPastFourGigabytes();
    void aSelectionCopiesAsHexAndAsText();
    void anEmptyFileSaysSoRatherThanShowingAnEmptyGrid();
    void aWindowADriveCannotSeekToReportsTheError();

    void tableOutranksTextForCsv();

    // --- a file of json records ---
    void recordsOutrankTextForJsonLines();
    void aFileOfFlatRecordsOpensAsAGridOfItsKeys();
    void aNestedValueArrivesAsCompactJsonInItsCell();
    void anAbsentKeyIsAnEmptyCellAndANullIsNot();
    void aFileWhoseRecordsAreNotObjectsShowsItsSourceAndSaysWhy();
    void askingForTheSourceOfAFileOfRecordsIsObeyedAndSaysNothing();
    void movingToAnotherFileWhileAnImportRunsLeavesItToFinishOnItsOwn();
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
    void coloursFilesWhoseNameIsTheirType_data();
    void coloursFilesWhoseNameIsTheirType();
    void textWithNothingToColourOpensPlain_data();
    void textWithNothingToColourOpensPlain();
    void aHugeFileWithNoSuffixOpensOnItsFirstWindow();
    void aFileAlreadyColouredFollowsTheThemesPolarity();

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

    // --- files with no lines in them ---
    void aWindowWithNoLineBreaksIsFoldedAndLeftUncoloured();
    void aWindowOfTheSameSizeWithLinesInItIsUntouched();
    void pagingOnFromAFoldedWindowGetsColouringBack();

    // --- markdown the window cannot afford to render ---
    void aMarkdownFileWithAHugeTableOpensAsSourceAndSaysWhy();
    void aMarkdownFileUnderTheBudgetIsRenderedAsBefore();
    void theBudgetIsTableRowsRatherThanTheSizeOfTheFile();
    void askingForThePageRendersADeclinedFile();
    void askingForTheSourceOfAnOrdinaryMarkdownFileIsObeyed();
    void markdownOffersThePageByDefaultAndTheSourceOnRequest();
    void pagingOnFromADeclinedWindowRendersAgain();

    // --- the tab ---
    void loadsTextContent();
    void parsesCsvWithADetectedSeparator();
    void tableFillsWhileTheImportIsStillRunning();
    void separatorCanBeOverridden();

    // --- databases ---
    void aDatabaseListsItsTablesBeforeItHasCountedAnyOfThem();
    void typingAFilterScansOnceRatherThanOncePerCharacter();
    void aDatabaseTableIsShownAPageAtATime();
    void reportsFactsForAnUnknownFile();
    void arrowsStepThroughFilesOnly();
    void survivesAFileThatVanished();
    void remembersItsFileAcrossRestart();

    void f3OnAFileCompressedOnItsOwnShowsWhatIsInside_data();
    void f3OnAFileCompressedOnItsOwnShowsWhatIsInside();
    void aContainerIsNotSubstituted_data();
    void aContainerIsNotSubstituted();
    void aFileNamedGzThatIsNotGzipKeepsTodaysBehaviour();
    void steppingOnReleasesTheSubstitutedMember();
    void withNoArchiveBackendTheTabBehavesAsItDidBefore();

    void aVideoIsClaimedByTheVideoViewer_data();
    void aVideoIsClaimedByTheVideoViewer();
    void aSuffixNoFormatKnowsFallsThroughToTheInformationViewer();
    void aVideoOnADriveThatCannotBePlayedFromIsCopiedLocally();
    void aVideoThatCannotBeDecodedFallsBackToWhatIsKnownAboutIt();
    void aRefusalWithNothingToSayStillSaysWhichViewerGaveUp();
    void anImageThisBuildCannotDecodeFallsBackToTheFacts();
    void aViewerWithNoSourceYetDoesNotGiveTheFileUp();
    void aDeclineStepsOneRungDownRatherThanToTheBottom();
    void theSoundIsOneSettingForEveryVideo();

    // ---- looking at an earlier state of the file --------------------------
    void aPreviewShowsTheFileAsItIsAndSaysSo();
    void aDriveWithNothingOlderOffersNoChoiceAtAll();
    void theVersionsAreNotFetchedUntilSomebodyAsksForThem();
    void movingToAnEarlierVersionShowsThatVersionsContents();
    void theScreenSaysWhichVersionIsOnThroughout();
    void everyViewerWorksOnAnEarlierVersion();
    void goingBackToTheCurrentFileWorks();
    void anEarlierVersionIsReadInPartsLikeAnyOtherFile();

private:
    IPreviewProvider* providerFor(const QString& relativePath) const;
    /// A preview on a drive that keeps earlier states of its files, showing
    /// `name`, with what else there is already known.
    PreviewTabController* previewOnOfferingDrive(const QString& name);
    PreviewTabController* openPreview(const QString& relativePath);
    /// Registers the backend that can open an archive, or answers false where
    /// this build has none -- which is itself one of the cases under test.
    bool withArchiveBackend();
    /// gzips a file that already exists in the fixture, in place, the way the
    /// command line does -- so the header carries the original name and the
    /// member is called what the file was called.
    bool gzipInPlace(const QString& relativePath);
    /// Mounts that exist only so something can be read. Nought is the answer
    /// after every file that is not a wrapper, and after stepping off one.
    int internalMounts() const;

    PrivateProfile m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<AppController> m_app;
    std::shared_ptr<OfferingFileSystem> m_offering;
};

void TestPreview::initTestCase()
{
    QVERIFY(m_profile.isValid());
}

void TestPreview::init()
{
    QFile::remove(QString::fromLocal8Bit(qgetenv("MOLE_SESSION_PATH")));
    // Preferences outlive a test unless they are removed, and several tests here
    // are about what is remembered -- one leaving a viewer choice or an open
    // details panel behind would decide the next one's answer.
    QFile::remove(m_profile.filePath(QStringLiteral("preferences.json")));

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

bool TestPreview::withArchiveBackend()
{
#ifdef MOLE_TEST_HAVE_ARCHIVE
    m_app->services().vfs->registerFactory(std::make_unique<ArchiveFileSystemFactory>());
    return true;
#else
    return false;
#endif
}

bool TestPreview::gzipInPlace(const QString& relativePath)
{
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("gzip"));
    if (tool.isEmpty())
        return false;

    QProcess process;
    process.start(tool, { m_tree->absolute(relativePath) });
    return process.waitForFinished(30000) && process.exitCode() == 0
        && QFile::exists(m_tree->absolute(relativePath + QStringLiteral(".gz")));
}

int TestPreview::internalMounts() const
{
    int internal = 0;
    for (const Mount& mount : m_app->services().vfs->mounts()) {
        if (mount.internal)
            ++internal;
    }
    return internal;
}

PreviewTabController* TestPreview::previewOnOfferingDrive(const QString& name)
{
    m_offering = std::make_shared<OfferingFileSystem>();
    const QString path = QLatin1Char('/') + name;
    m_offering->memory()->addFile(path, QByteArray("the third draft"));
    m_offering->addVersion(path, QStringLiteral("v1"), QByteArray("the first draft"));
    m_offering->addVersion(path, QStringLiteral("v2"), QByteArray("the second draft"));
    // A file this drive has nothing at all for, which is what most files are.
    m_offering->memory()->addFile(QStringLiteral("/plain.txt"), QByteArray("nothing older than this"));
    m_offering->setLinkable(QStringLiteral("/plain.txt"), false);
    // And one whose earlier state is far too big to fetch to show a page of.
    m_offering->memory()->addFile(QStringLiteral("/big.txt"), QByteArray("small now"));
    m_offering->addVersion(
        QStringLiteral("/big.txt"), QStringLiteral("v1"), QByteArray(4 * 1024 * 1024, 'x'));

    Mount mount;
    mount.id = QStringLiteral("offering");
    mount.displayName = QStringLiteral("offering");
    mount.root = VfsUri::fromString(QStringLiteral("mem://offering/"));
    mount.fileSystem = m_offering;
    if (m_app->services().vfs->addMount(mount).isEmpty())
        return nullptr;

    m_app->previewFile(QStringLiteral("mem://offering/") + name);
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    if (!preview)
        return nullptr;
    waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); });
    // What else there is of this file is asked for on a worker, so the chooser
    // knows whether there is anything to choose before anybody opens it.
    waitFor([preview] { return preview->hasOtherVersions(); });
    return preview;
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
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());

    // A file whose name got no further than the fallback tier is identified from
    // its first page before a viewer is chosen, so there is a moment with no
    // viewer at all. Waited for on the condition rather than for a duration: the
    // read is off the UI thread and how long it takes is the machine's business.
    if (preview)
        waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); });
    return preview;
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

// -------------------------------------- identified by what is in it

void TestPreview::aFileWhoseNameSaysNothingIsIdentifiedFromItsContents()
{
    // shared-mime-info has no glob for this name, so the name-only lookup can
    // only ever answer application/octet-stream and hand it to the fact list.
    QVERIFY(m_tree->writeFile(QStringLiteral("Dockerfile"),
        QByteArray("FROM debian:bookworm\nRUN apt-get update && apt-get install -y build-essential\n")));

    PreviewTabController* preview = openPreview(QStringLiteral("Dockerfile"));
    QVERIFY(preview);
    QVERIFY2(preview->viewer(), "a file that is plainly text has to reach a viewer");

    auto* text = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY2(text, qPrintable(QStringLiteral("reached %1").arg(preview->viewerName())));
    QVERIFY(waitFor([text] { return !text->text().isEmpty(); }, 5000));
    QVERIFY(text->text().contains(QStringLiteral("FROM debian")));
}

void TestPreview::contentsOutrankAMisleadingName()
{
    // The other half: a name is a label somebody typed, and the text viewer
    // would fill the window with replacement characters.
    QVERIFY(m_tree->writeFile(
        QStringLiteral("notes-really.txt"), QByteArrayLiteral("PK\x03\x04") + QByteArray(400, '\x01')));

    PreviewTabController* preview = openPreview(QStringLiteral("notes-really.txt"));
    QVERIFY(preview);
    QVERIFY(preview->viewer());
    QVERIFY2(!qobject_cast<TextPreviewController*>(preview->viewer()),
        "a zip called notes.txt must not open in the text viewer");
    // And it is named rather than dumped: Mole knows what a zip is, so what it
    // shows is what the file is.
    QCOMPARE(preview->viewerName(), QStringLiteral("File information"));
    preview->setDetailsOpen(true);
    QVERIFY(waitFor([preview] { return !preview->isDetailsLoading(); }, 5000));
    QCOMPARE(detailNamed(preview, QStringLiteral("MIME type")), QStringLiteral("application/zip"));
}

void TestPreview::identifyingAFileIsOneReadOfOnePage()
{
    // A file far larger than anything that gets read, on a drive that counts
    // each stream separately -- the first one is the sniff, and the bound on it
    // is the whole promise: a 100 GB file with no extension has to open as fast
    // as a 100 byte one.
    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/blob8842"), unknownBinary(4 * 1024 * 1024));
    auto counted = std::make_shared<FaultyFileSystem>(memory);

    Mount mount;
    mount.id = QStringLiteral("counted");
    mount.displayName = QStringLiteral("counted");
    mount.root = VfsUri::fromString(QStringLiteral("mem://counted/"));
    mount.fileSystem = counted;
    m_app->services().vfs->addMount(mount);

    m_app->previewFile(QStringLiteral("mem://counted/blob8842"));
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    QVERIFY(preview);
    QVERIFY(waitFor([preview] { return preview->viewer() != nullptr; }, 5000));

    QCOMPARE(preview->viewerName(), QStringLiteral("Bytes"));

    // One page, and the byte ReadRangeTask reads past the end of every window to
    // learn whether anything follows it.
    QVERIFY(!counted->readSizes().isEmpty());
    QVERIFY2(counted->readSizes().first() <= FileType::kSampleBytes + 1,
        qPrintable(QStringLiteral("read %1 bytes to identify a file").arg(counted->readSizes().first())));
    // And what the viewer then read is its own one window, not the file.
    QVERIFY2(counted->bytesRead() <= FileType::kSampleBytes + HexPreviewController::kWindowBytes + 2,
        qPrintable(QStringLiteral("read %1 bytes of a 4 MB file").arg(counted->bytesRead())));
}

void TestPreview::everyFileKeepsTheViewerItAlreadyHad_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("viewerName");

    QTest::newRow("plain text") << "notes.txt" << "Text";
    QTest::newRow("json") << "config.json" << "Text";
    QTest::newRow("csv") << "prices.csv" << "Table";
    QTest::newRow("tsv") << "data.tsv" << "Table";
    // Nothing to read, so nothing is read: an empty file keeps the answer its
    // name has always given.
    QTest::newRow("empty, named") << "empty.txt" << "Text";
    QTest::newRow("empty, unnamed") << "empty" << "File information";
}

void TestPreview::everyFileKeepsTheViewerItAlreadyHad()
{
    QFETCH(QString, fileName);
    QFETCH(QString, viewerName);

    QVERIFY(m_tree->writeFile(QStringLiteral("empty.txt"), QByteArray()));
    QVERIFY(m_tree->writeFile(QStringLiteral("empty"), QByteArray()));

    PreviewTabController* preview = openPreview(fileName);
    QVERIFY(preview);
    QCOMPARE(preview->viewerName(), viewerName);

    // And a directory is still nobody's business.
    FileEntry folder;
    folder.name = QStringLiteral("subfolder");
    folder.uri = m_tree->rootUri().child(QStringLiteral("subfolder"));
    folder.isDir = true;
    QVERIFY(m_app->previews()->providerFor(folder) == nullptr);
}

void TestPreview::theFactListNamesWhatTheFileTurnedOutToBe()
{
    // Asked of the reader rather than through a viewer: the nine facts belong to
    // the generic metadata reader now, and every viewer shows them in its details
    // panel. What is held is that a file somebody has looked inside is named --
    // this used to answer "Unknown" for everything the database had no glob for.
    FileEntry entry;
    entry.name = QStringLiteral("store");
    entry.uri = m_tree->rootUri().child(QStringLiteral("store"));
    entry.size = 416;
    entry.mimeType = QStringLiteral("application/vnd.sqlite3");

    const GenericMetadataReader reader;
    QVERIFY(reader.canRead(entry));

    const CancelToken cancel;
    const QList<FileFact> facts = reader.read(entry, QByteArrayView(), m_app->services(), cancel);

    const auto factNamed = [&facts](const QString& label) {
        for (const FileFact& fact : facts) {
            if (fact.label == label)
                return fact.value;
        }
        return QString();
    };

    QCOMPARE(factNamed(QStringLiteral("MIME type")), QStringLiteral("application/vnd.sqlite3"));
    QVERIFY2(factNamed(QStringLiteral("Type")) != QStringLiteral("Unknown"),
        qPrintable(factNamed(QStringLiteral("Type"))));
    QVERIFY(factNamed(QStringLiteral("Size")).contains(QStringLiteral("416")));
}

// ------------------------------------------ what a file says about itself

void TestPreview::everyReaderThatClaimsAFileContributes()
{
    // Where this differs from the viewer lookup, which stops at the first match:
    // a container and its contents are two sets of facts about one file, and
    // both are right at once. Priority decides the order, not the winner.
    m_app->metadata()->addReader(std::make_unique<FakeMetadataReader>(QStringLiteral("test.low"),
        QList<FileFact> { { QStringLiteral("Second"), QStringLiteral("later") } }, 10));
    m_app->metadata()->addReader(std::make_unique<FakeMetadataReader>(QStringLiteral("test.high"),
        QList<FileFact> { { QStringLiteral("First"), QStringLiteral("sooner") } }, 20));

    PreviewTabController* preview = openPreview(QStringLiteral("notes.txt"));
    QVERIFY(preview);
    preview->setDetailsOpen(true);
    QVERIFY(waitFor([preview] { return !preview->isDetailsLoading(); }, 5000));

    const QStringList labels = detailLabels(preview);
    QVERIFY2(labels.contains(QStringLiteral("First")), qPrintable(labels.join(QLatin1Char(','))));
    QVERIFY(labels.contains(QStringLiteral("Second")));
    QVERIFY2(labels.indexOf(QStringLiteral("First")) < labels.indexOf(QStringLiteral("Second")),
        "readers are shown in priority order");
    // And the reader that claims everything is still there, underneath both.
    QVERIFY(labels.contains(QStringLiteral("Size")));
    QVERIFY(labels.indexOf(QStringLiteral("Second")) < labels.indexOf(QStringLiteral("Size")));

    QCOMPARE(detailNamed(preview, QStringLiteral("First")), QStringLiteral("sooner"));
}

void TestPreview::nothingIsReadUntilTheDetailsAreOpened()
{
    // The panel is where an expensive reader's cost falls, so it must fall on
    // somebody who asked. A file claimed on its name is the honest case: nothing
    // has read it at all when the viewer opens.
    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/rows.csv"), QByteArray("a,b\n1,2\n"));
    auto counted = std::make_shared<FaultyFileSystem>(memory);

    Mount mount;
    mount.id = QStringLiteral("counted");
    mount.displayName = QStringLiteral("counted");
    mount.root = VfsUri::fromString(QStringLiteral("mem://counted/"));
    mount.fileSystem = counted;
    m_app->services().vfs->addMount(mount);

    auto log = std::make_shared<FakeMetadataReader::Log>();
    m_app->metadata()->addReader(std::make_unique<FakeMetadataReader>(QStringLiteral("test.counting"),
        QList<FileFact> { { QStringLiteral("Asked"), QStringLiteral("yes") } }, 10, log));

    m_app->previewFile(QStringLiteral("mem://counted/rows.csv"));
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    QVERIFY(preview);
    QVERIFY(waitFor([preview] { return preview->viewer() != nullptr; }, 5000));

    QVERIFY2(!preview->isDetailsOpen(), "a viewer with a file to show does not open the panel");
    QCOMPARE(log->reads.load(), 0);

    const qint64 beforeOpening = counted->bytesRead();
    preview->setDetailsOpen(true);
    QVERIFY(waitFor([preview] { return !preview->isDetailsLoading(); }, 5000));

    QCOMPARE(log->reads.load(), 1);
    QCOMPARE(detailNamed(preview, QStringLiteral("Asked")), QStringLiteral("yes"));
    // And what it cost is one page, because that is all a reader is given.
    QVERIFY2(counted->bytesRead() - beforeOpening <= FileType::kSampleBytes,
        qPrintable(
            QStringLiteral("reading the details cost %1 bytes").arg(counted->bytesRead() - beforeOpening)));
    QVERIFY(log->headSize.load() > 0);
}

void TestPreview::theDetailsAreOneSettingForEveryFile()
{
    // Deliberately not ADR-0006's per-suffix key. That one is right for "render
    // this .html as a page", which is a choice about a file type; whether a
    // drawer is open is a choice about the person and their screen, and having
    // it appear for one kind of file and vanish for the next is the surprise.
    QVERIFY(m_tree->writeFile(QStringLiteral("photo.jpg"), QByteArray("not really a photograph\n")));

    PreviewTabController* preview = openPreview(QStringLiteral("notes.txt"));
    QVERIFY(preview);
    QVERIFY2(!preview->isDetailsOpen(), "closed until somebody asks");
    preview->setDetailsOpen(true);

    // A different file type, and the drawer is still there.
    PreviewTabController* table = openPreview(QStringLiteral("prices.csv"));
    QVERIFY2(table->isDetailsOpen(), "one switch for every preview, not one per file type");
    PreviewTabController* picture = openPreview(QStringLiteral("photo.jpg"));
    QVERIFY(picture->isDetailsOpen());

    // And it survives a restart, because it is a preference rather than a mood.
    m_app->saveSessionNow();
    m_app.reset();
    drainEvents();

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));

    PreviewTabController* afterRestart = openPreview(QStringLiteral("notes.txt"));
    QVERIFY(afterRestart);
    QVERIFY(afterRestart->isDetailsOpen());

    // Shutting it shuts it for everything, which is the other half of "one
    // setting" and the half a per-type key got wrong.
    afterRestart->setDetailsOpen(false);
    QVERIFY(!openPreview(QStringLiteral("prices.csv"))->isDetailsOpen());

    // The width is remembered the same way, and only when the divider is let go.
    afterRestart = openPreview(QStringLiteral("notes.txt"));
    afterRestart->setDetailsWidth(420);
    QCOMPARE(openPreview(QStringLiteral("prices.csv"))->detailsWidth(), 420);
}

void TestPreview::aValueCanBeCopiedOutOfTheDetails()
{
    // A fact nobody can copy is a fact somebody retypes. The selection is the
    // view's business; putting every row on the clipboard is the controller's,
    // the same shape as TablePreviewController::copyBlock().
    PreviewTabController* preview = openPreview(QStringLiteral("notes.txt"));
    QVERIFY(preview);
    preview->setDetailsOpen(true);
    QVERIFY(waitFor([preview] { return !preview->isDetailsLoading(); }, 5000));
    QVERIFY(!preview->details().isEmpty());

    preview->copyDetails();
    const QString copied = QGuiApplication::clipboard()->text();
    QVERIFY2(copied.contains(QStringLiteral("Size: ")), qPrintable(copied));
    QVERIFY2(copied.contains(QStringLiteral("MIME type: text/plain")), qPrintable(copied));
    // One row per line, in the order the readers answered.
    QCOMPARE(copied.split(QLatin1Char('\n')).size(), preview->details().size());
}

void TestPreview::steppingToTheNextFileCancelsAReaderInFlight()
{
    // A reader held still while the reader steps away. Waited on the condition
    // -- the gate -- rather than on a clock, and released by the test.
    auto gate = std::make_shared<QSemaphore>();
    // Released whatever happens next, including an assertion below failing and
    // returning from this function. A reader left on the gate is a thread the
    // fixture's teardown waits for until ctest kills the binary, and the failure
    // then reads as a timeout with no assertion named. See MOLE-217.
    const GateRelease letGoOnTheWayOut(gate);

    auto log = std::make_shared<FakeMetadataReader::Log>();
    // Claims `.txt` and nothing else, which is what makes the rest of this
    // deterministic. Unfiltered, it also claimed the file stepped *to* -- the
    // drawer is one setting, so that file starts a read of its own -- and then two
    // readers were waiting on one permit. Whichever won was down to the scheduler:
    // if the new file's read took it, the cancelled counter below never moved and
    // the wait failed on a machine that was merely busy.
    auto slow = std::make_unique<FakeMetadataReader>(QStringLiteral("test.slow"),
        QList<FileFact> { { QStringLiteral("Slow"), QStringLiteral("finally") } }, 10, log,
        QStringLiteral("txt"));
    slow->holdUntilReleased(gate);
    m_app->metadata()->addReader(std::move(slow));

    PreviewTabController* preview = openPreview(QStringLiteral("notes.txt"));
    QVERIFY(preview);
    preview->setDetailsOpen(true);
    QVERIFY(waitFor([log] { return log->reads.load() == 1; }, 30000));

    // Off the GUI thread: it is still stuck in the reader and the interface is
    // not.
    QVERIFY(preview->isDetailsLoading());

    preview->open(m_tree->rootUri().child(QStringLiteral("config.json")).toString());
    QVERIFY2(!preview->isDetailsLoading(), "the tab lets go of a reader the moment the file changes");
    gate->release();

    // Waited on the reader having noticed, rather than on the tab having let go:
    // the two happen on different threads and only the first one is the claim.
    // The budget is a backstop and not the measurement -- there is exactly one
    // reader waiting on exactly one permit now, so the only thing between the
    // release and the counter is the pool getting a slice.
    QVERIFY(waitFor([log] { return log->cancelled.load() == 1; }, 30000));

    // And what it was going to say never reaches the file that is open now: the
    // task finished, late, against a tab that has moved on. The reader cannot
    // claim `config.json` at all, so a "Slow" row here could only have come from
    // the file somebody stepped away from.
    drainEvents();
    QVERIFY2(detailNamed(preview, QStringLiteral("Slow")).isEmpty(),
        "facts about the file somebody stepped away from must not land on this one");
}

void TestPreview::aReaderThatFailsCostsOnlyItsOwnRows()
{
    auto failing = std::make_unique<FakeMetadataReader>(QStringLiteral("test.broken"),
        QList<FileFact> { { QStringLiteral("Never"), QStringLiteral("shown") } }, 20);
    failing->failInstead();
    m_app->metadata()->addReader(std::move(failing));
    // One that simply has nothing to say, which is not a failure at all.
    m_app->metadata()->addReader(
        std::make_unique<FakeMetadataReader>(QStringLiteral("test.silent"), QList<FileFact> {}, 15));

    PreviewTabController* preview = openPreview(QStringLiteral("notes.txt"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);

    preview->setDetailsOpen(true);
    QVERIFY(waitFor([preview] { return !preview->isDetailsLoading(); }, 5000));

    QVERIFY2(detailNamed(preview, QStringLiteral("Never")).isEmpty(), "a reader that threw says nothing");
    QVERIFY2(
        !detailNamed(preview, QStringLiteral("Size")).isEmpty(), "and the readers after it are unaffected");

    // The viewer is untouched by any of it.
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));
    QVERIFY(viewer->errorText().isEmpty());
}

void TestPreview::aPhotographShowsThePictureAndWhatTheCameraWrote()
{
    // The whole point of the panel, on the file type people care most about: the
    // picture is still a picture, and everything the camera wrote is on screen
    // beside it.
    QImage image(320, 240, QImage::Format_RGB32);
    image.fill(Qt::darkMagenta);
    QByteArray encoded;
    {
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "jpeg");
        QVERIFY(writer.write(image));
    }

    // A minimal EXIF block: a make, a model and an exposure, spliced in as the
    // APP1 segment the way a camera writes it. The parser has its own suite --
    // what is held here is that the panel shows what it found.
    const QByteArray exif = QByteArray::fromHex("49492a00080000000200"
                                                "0f01"
                                                "0200"
                                                "06000000"
                                                "26000000"
                                                "1001"
                                                "0200"
                                                "09000000"
                                                "2c000000"
                                                "00000000")
        + QByteArray("Canon\0", 6) + QByteArray("EOS 700D\0", 9);
    QByteArray app1;
    app1 += char(0xff);
    app1 += char(0xe1);
    const int length = int(exif.size()) + 8;
    app1 += char((length >> 8) & 0xff);
    app1 += char(length & 0xff);
    app1 += QByteArray("Exif\0\0", 6);
    app1 += exif;

    QVERIFY(m_tree->writeFile(QStringLiteral("holiday.jpg"), encoded.left(2) + app1 + encoded.mid(2)));

    PreviewTabController* preview = openPreview(QStringLiteral("holiday.jpg"));
    QVERIFY(preview);
    QCOMPARE(preview->providerId(), QStringLiteral("mole.preview.image"));

    auto* viewer = qobject_cast<ImagePreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->source().isEmpty(); }, 5000));

    preview->setDetailsOpen(true);
    QVERIFY(waitFor([preview] { return !preview->isDetailsLoading(); }, 5000));

    QCOMPARE(detailNamed(preview, QStringLiteral("Dimensions")), QStringLiteral("320 × 240"));
    QCOMPARE(detailNamed(preview, QStringLiteral("Format")), QStringLiteral("JPEG"));
    QCOMPARE(detailNamed(preview, QStringLiteral("Camera")), QStringLiteral("Canon EOS 700D"));
    // And the generic facts are still underneath, from the reader below it.
    QVERIFY(!detailNamed(preview, QStringLiteral("Size")).isEmpty());
}

void TestPreview::aDocumentNamesItsAuthorWithoutBeingReadWhole()
{
    if (!DocumentMetadataReader::isAvailable())
        QSKIP("built without libarchive");

    // A container the size of a real report, with its properties at the front
    // where every writer puts them. What must not happen is the whole thing
    // being fetched to read a name out of the first kilobytes.
    QByteArray core
        = QByteArrayLiteral("<?xml version=\"1.0\"?><cp:coreProperties xmlns:cp=\"x\" xmlns:dc=\"y\">"
                            "<dc:creator>Ada Lovelace</dc:creator><dc:title>Quarterly report</dc:title>"
                            "</cp:coreProperties>");

    // Its properties at the front and a body far larger than anything the
    // reader is allowed to fetch.
    StoredZip zip;
    zip.add("docProps/core.xml", core);
    zip.add("word/document.xml", QByteArray(4 * 1024 * 1024, 'w'));
    const QByteArray document = zip.build();

    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/report.docx"), document);
    auto counted = std::make_shared<FaultyFileSystem>(memory);

    Mount mount;
    mount.id = QStringLiteral("counted");
    mount.displayName = QStringLiteral("counted");
    mount.root = VfsUri::fromString(QStringLiteral("mem://counted/"));
    mount.fileSystem = counted;
    m_app->services().vfs->addMount(mount);

    m_app->previewFile(QStringLiteral("mem://counted/report.docx"));
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    QVERIFY(preview);
    QVERIFY(waitFor([preview] { return preview->viewer() != nullptr; }, 5000));

    preview->setDetailsOpen(true);
    QVERIFY(waitFor([preview] { return !preview->isDetailsLoading(); }, 5000));

    QCOMPARE(detailNamed(preview, QStringLiteral("Author")), QStringLiteral("Ada Lovelace"));
    QCOMPARE(detailNamed(preview, QStringLiteral("Title")), QStringLiteral("Quarterly report"));

    // The sniff, the hex viewer's window, and one bounded prefix for the
    // reader. Nowhere near the four megabytes of body.
    const qint64 allowed = FileType::kSampleBytes + HexPreviewController::kWindowBytes
        + DocumentMetadataReader::kPrefixBytes + 8;
    QVERIFY2(counted->bytesRead() <= allowed,
        qPrintable(QStringLiteral("read %1 bytes of a 4 MB document").arg(counted->bytesRead())));
}

// ------------------------------------------------- the bytes themselves

void TestPreview::bytesAreShownForWhatNothingElseClaims_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<QString>("providerId");

    // The bytes are for what nothing can name -- no magic rule, no glob, not
    // text -- and for nothing else.
    QTest::newRow("no format anybody knows") << "firmware.dat" << unknownBinary(4096) << "mole.preview.hex";
    QTest::newRow("no name and no format") << "blob8842" << unknownBinary(4096) << "mole.preview.hex";

    // Everything Mole can name says what it is instead. Showing somebody the
    // first 64 kB of their holiday video in hexadecimal is strictly less than
    // telling them how long it runs -- and since MOLE-37 a video does better than
    // either, which is what the row below now asserts.
    QTest::newRow("a zip under any name")
        << "bundle" << (QByteArrayLiteral("PK\x03\x04") + QByteArray(400, '\x01')) << "mole.preview.fileinfo";
    // The one row whose answer depends on the build, and it is the answer MOLE-37
    // promised: with Qt Multimedia the video viewer takes it, and without the
    // module the very same file gets the information viewer it always had.
    QTest::newRow("a video") << "holiday.mp4" << mp4File()
                             << (VideoPreviewProvider::isAvailable()
                                        ? QStringLiteral("mole.preview.video")
                                        : QStringLiteral("mole.preview.fileinfo"));
    QTest::newRow("an mp3") << "song.mp3" << mp3File() << "mole.preview.fileinfo";
    QTest::newRow("a document") << "report.docx" << docxFile() << "mole.preview.fileinfo";

    // Text never lands in either, however unrecognisable its name.
    QTest::newRow("text with an odd name")
        << "Jenkinsfile" << QByteArray("pipeline { agent any }\n") << "mole.preview.text";
    // And a format with a viewer of its own keeps it: the hex viewer sits below
    // every one of them.
    QTest::newRow("a database") << "index.sqlite"
                                << (QByteArray("SQLite format 3\0", 16) + QByteArray(400, '\x02'))
                                << "mole.preview.sqlite";
}

void TestPreview::bytesAreShownForWhatNothingElseClaims()
{
    QFETCH(QString, fileName);
    QFETCH(QByteArray, contents);
    QFETCH(QString, providerId);

    QVERIFY(m_tree->writeFile(fileName, contents));

    PreviewTabController* preview = openPreview(fileName);
    QVERIFY(preview);
    QVERIFY(preview->viewer());
    QCOMPARE(preview->providerId(), providerId);
}

void TestPreview::theBytesAreAChoiceOnEveryFileThatHasNoViewer()
{
    // The other half of narrowing the claim: the bytes did not go away, they
    // became a choice -- and one that is remembered, like every other viewer's.
    //
    // On audio, because a video has a viewer of its own since MOLE-37 and this is
    // about the files that have none. An mp3 is still one of them: Mole says what
    // it is and what it holds, and does not play it.
    QVERIFY(m_tree->writeFile(QStringLiteral("song.mp3"), mp3File()));
    QVERIFY(m_tree->writeFile(QStringLiteral("second.mp3"), mp3File()));
    QVERIFY(m_tree->writeFile(QStringLiteral("report.docx"), docxFile()));

    PreviewTabController* preview = openPreview(QStringLiteral("song.mp3"));
    QVERIFY(preview);
    QCOMPARE(preview->viewerName(), QStringLiteral("File information"));

    const QVariantList options = preview->viewerOptions();
    QCOMPARE(options.size(), 1);
    QCOMPARE(
        options.first().toMap().value(QStringLiteral("chosen")).toString(), QStringLiteral("Information"));

    auto* viewer = qobject_cast<FileInfoPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY2(!viewer->isShowingBytes(), "and nothing is built for a choice nobody made");
    QVERIFY(viewer->bytes() == nullptr);

    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Bytes"));
    QVERIFY(viewer->isShowingBytes());
    auto* hex = qobject_cast<HexPreviewController*>(viewer->bytes());
    QVERIFY(hex);
    QVERIFY(waitFor([hex] { return !hex->rows().isEmpty(); }, 5000));
    QCOMPARE(
        hex->rows().first().toMap().value(QStringLiteral("offset")).toString(), QStringLiteral("00000000"));

    // The next file of the same type opens the way the last one was left.
    PreviewTabController* second = openPreview(QStringLiteral("second.mp3"));
    QCOMPARE(second->viewerOptions().first().toMap().value(QStringLiteral("chosen")).toString(),
        QStringLiteral("Bytes"));
    auto* secondViewer = qobject_cast<FileInfoPreviewController*>(second->viewer());
    QVERIFY(secondViewer);
    QVERIFY2(secondViewer->isShowingBytes(), "and it is applied before the file is read, not after");

    // A different type is untouched, which is what per-file-type means.
    PreviewTabController* other = openPreview(QStringLiteral("report.docx"));
    QCOMPARE(other->viewerOptions().first().toMap().value(QStringLiteral("chosen")).toString(),
        QStringLiteral("Information"));
}

void TestPreview::theHexWindowIsOnePageOfSixtyFourKilobytes()
{
    const QByteArray contents = unknownBinary(200 * 1024);
    QVERIFY(m_tree->writeFile(QStringLiteral("firmware.dat"), contents));

    PreviewTabController* preview = openPreview(QStringLiteral("firmware.dat"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<HexPreviewController*>(preview->viewer());
    QVERIFY2(viewer, qPrintable(preview->viewerName()));
    QVERIFY(waitFor([viewer] { return !viewer->rows().isEmpty(); }, 5000));

    QCOMPARE(viewer->windowBytes(), HexPreviewController::kWindowBytes);
    QCOMPARE(viewer->rows().size(), HexPreviewController::kWindowBytes / 16);
    QVERIFY(viewer->isPaged());

    // The first row is the first sixteen bytes, in both columns.
    const QVariantMap first = viewer->rows().first().toMap();
    QCOMPARE(first.value(QStringLiteral("offset")).toString(), QStringLiteral("00000000"));
    QVERIFY2(first.value(QStringLiteral("hex")).toString().startsWith(QStringLiteral("7f 01 02 03 ")),
        qPrintable(first.value(QStringLiteral("hex")).toString()));
    QCOMPARE(first.value(QStringLiteral("text")).toString(), QStringLiteral("....MOLE-FIRMWAR"));
}

void TestPreview::ahugeFileIsPagedAndItsTailIsReachable()
{
    // A hundred gigabytes, sparse. The tail is the half that a viewer holding
    // the file could never reach at all.
    constexpr qint64 kSize = 100LL * 1024 * 1024 * 1024;
    const QString path = m_tree->absolute(QStringLiteral("image.dat"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray head = unknownBinary(64 * 1024);
        QCOMPARE(file.write(head), head.size());
        QVERIFY2(file.resize(kSize), qPrintable(file.errorString()));
    }
    if (QFileInfo(path).size() != kSize)
        QSKIP("this filesystem would not make a sparse file");

    auto counted = std::make_shared<FaultyFileSystem>(std::make_shared<LocalFileSystem>());
    Mount mount;
    mount.id = QStringLiteral("counted");
    mount.displayName = QStringLiteral("counted");
    mount.root = m_tree->rootUri();
    mount.fileSystem = counted;
    m_app->services().vfs->addMount(mount);

    m_app->previewFile(VfsUri::fromLocalPath(path).toString());
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    QVERIFY(preview);
    QVERIFY(waitFor([preview] { return preview->viewer() != nullptr; }, 10000));

    auto* viewer = qobject_cast<HexPreviewController*>(preview->viewer());
    QVERIFY2(viewer, qPrintable(preview->viewerName()));
    QVERIFY(waitFor([viewer] { return !viewer->rows().isEmpty(); }, 10000));

    QCOMPARE(viewer->fileSize(), kSize);
    QVERIFY(viewer->isPaged());
    QVERIFY(viewer->isAtStart());

    // Jumping to the end reads the end, and the offsets prove it: the last
    // window starts one window short of the size.
    viewer->lastWindow();
    QVERIFY(waitFor([viewer] { return viewer->windowOffset() > 0; }, 10000));
    QCOMPARE(viewer->windowOffset(), kSize - HexPreviewController::kWindowBytes);
    QVERIFY(viewer->isAtEnd());
    QCOMPARE(viewer->rows().first().toMap().value(QStringLiteral("offset")).toString(),
        QStringLiteral("18ffff0000"));

    // Two windows and one page to identify it, out of a hundred gigabytes.
    QVERIFY2(counted->bytesRead() <= FileType::kSampleBytes + 2 * HexPreviewController::kWindowBytes + 3,
        qPrintable(QStringLiteral("read %1 bytes of a 100 GB file").arg(counted->bytesRead())));
}

void TestPreview::theOffsetColumnWidensPastFourGigabytes()
{
    // Eight digits is the width every hex dump has always had, and it stops
    // being enough at exactly 4 GB.
    HexPreviewController small(m_app->services());
    FileEntry entry;
    entry.name = QStringLiteral("firmware.dat");
    entry.uri = m_tree->rootUri().child(QStringLiteral("firmware.dat"));
    entry.size = 4LL * 1024 * 1024 * 1024 - 1;
    entry.mimeType = QStringLiteral("application/octet-stream");
    QVERIFY(m_tree->writeFile(QStringLiteral("firmware.dat"), unknownBinary(64)));
    small.load(entry);
    QCOMPARE(small.offsetDigits(), 8);

    HexPreviewController large(m_app->services());
    entry.size = 8LL * 1024 * 1024 * 1024;
    large.load(entry);
    QCOMPARE(large.offsetDigits(), 10);
}

void TestPreview::aSelectionCopiesAsHexAndAsText()
{
    // Sixteen bytes whose hex and whose text are both known by hand, so what
    // comes out can be compared against something written down rather than
    // against the same code that produced it.
    const QByteArray contents = QByteArray("\x00\x01MOLE bytes\x7f\xff\xfe\xfd", 16) + QByteArray(64, '*');
    QVERIFY(m_tree->writeFile(QStringLiteral("sixteen.dat"), contents));

    PreviewTabController* preview = openPreview(QStringLiteral("sixteen.dat"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<HexPreviewController*>(preview->viewer());
    QVERIFY2(viewer, qPrintable(preview->viewerName()));
    QVERIFY(waitFor([viewer] { return !viewer->rows().isEmpty(); }, 5000));

    QVERIFY(viewer->selectionAsHex().isEmpty());
    QVERIFY(viewer->selectionSummary().isEmpty());

    viewer->selectRange(0, 15);
    QCOMPARE(viewer->selectionStart(), 0);
    QCOMPARE(viewer->selectionLength(), 16);
    QCOMPARE(viewer->selectionAsHex(), QStringLiteral("00 01 4d 4f 4c 45 20 62 79 74 65 73 7f ff fe fd"));
    QCOMPARE(viewer->selectionAsText(), QStringLiteral("..MOLE bytes...."));

    // Dragged the other way is the same selection, and a run in the middle is
    // the run in the middle.
    viewer->selectRange(5, 2);
    QCOMPARE(viewer->selectionAsText(), QStringLiteral("MOLE"));
    QCOMPARE(viewer->selectionAsHex(), QStringLiteral("4d 4f 4c 45"));
    QVERIFY(viewer->selectionSummary().contains(QStringLiteral("4 bytes")));

    // Past the end of the window clamps rather than reading anything.
    viewer->selectRange(0, contents.size() * 4);
    QCOMPARE(viewer->selectionLength(), contents.size());

    // The clipboard is the platform's, so what is asserted is that asking does
    // not go wrong; what would be copied is asserted above.
    viewer->copySelectionAsHex();
    viewer->copySelectionAsText();

    viewer->clearSelection();
    QCOMPARE(viewer->selectionStart(), -1);
    QVERIFY(viewer->selectionAsText().isEmpty());
}

void TestPreview::anEmptyFileSaysSoRatherThanShowingAnEmptyGrid()
{
    // An empty file never reaches this viewer through the tab -- there is
    // nothing to identify it with -- but a plugin or a backend that fills in a
    // type can send one, and a grid with no rows explains nothing.
    QVERIFY(m_tree->writeFile(QStringLiteral("nothing.dat"), QByteArray()));

    FileEntry entry;
    entry.name = QStringLiteral("nothing.dat");
    entry.uri = m_tree->rootUri().child(QStringLiteral("nothing.dat"));
    entry.size = 0;
    entry.mimeType = QStringLiteral("application/octet-stream");

    HexPreviewController viewer(m_app->services());
    viewer.load(entry);

    QVERIFY(viewer.isEmptyFile());
    QVERIFY(viewer.rows().isEmpty());
    QVERIFY(viewer.errorText().isEmpty());
    QVERIFY(!viewer.isPaged());
}

void TestPreview::aWindowADriveCannotSeekToReportsTheError()
{
    // A drive that can only be read from the beginning: the first window works
    // and the second is refused. What must not happen is an empty grid with no
    // explanation -- or gigabytes read and thrown away to reach an offset.
    const QByteArray contents = unknownBinary(200 * 1024);
    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/stream.dat"), contents);
    auto sequential = std::make_shared<FaultyFileSystem>(memory);
    sequential->cannotSeek();

    Mount mount;
    mount.id = QStringLiteral("sequential");
    mount.displayName = QStringLiteral("sequential");
    mount.root = VfsUri::fromString(QStringLiteral("mem://sequential/"));
    mount.fileSystem = sequential;
    m_app->services().vfs->addMount(mount);

    m_app->previewFile(QStringLiteral("mem://sequential/stream.dat"));
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    QVERIFY(preview);
    QVERIFY(waitFor([preview] { return preview->viewer() != nullptr; }, 5000));

    auto* viewer = qobject_cast<HexPreviewController*>(preview->viewer());
    QVERIFY2(viewer, qPrintable(preview->viewerName()));
    QVERIFY(waitFor([viewer] { return !viewer->rows().isEmpty(); }, 5000));

    viewer->nextWindow();
    QVERIFY(waitFor([viewer] { return !viewer->errorText().isEmpty(); }, 5000));
    QVERIFY2(viewer->errorText().contains(QStringLiteral("beginning")), qPrintable(viewer->errorText()));
    QVERIFY2(viewer->rows().isEmpty(), "rows from the last window under this message would be a lie");
    // Refused rather than satisfied the expensive way: a backend that cannot
    // seek must not be read through to reach an offset.
    QVERIFY2(sequential->bytesRead() <= FileType::kSampleBytes + HexPreviewController::kWindowBytes + 2,
        qPrintable(QStringLiteral("read %1 bytes").arg(sequential->bytesRead())));
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

// --------------------------------------------- a file of json records

namespace {

/// One JSON object per line, from lines of text.
QByteArray jsonLines(const QStringList& records)
{
    QByteArray out;
    for (const QString& record : records) {
        out += record.toUtf8();
        out += '\n';
    }
    return out;
}

} // namespace

void TestPreview::recordsOutrankTextForJsonLines()
{
    // Both accept .jsonl -- the highlighter maps it on to the JSON rules, so the
    // text viewer would show coloured source, one record per line. Priority is
    // what takes it off there, exactly as it does for a .csv.
    TextPreviewProvider text(m_app->services());
    JsonLinesPreviewProvider records(m_app->services());

    for (const char* name : { "events.jsonl", "events.ndjson", "EVENTS.JSONL" }) {
        FileEntry entry;
        entry.name = QLatin1String(name);
        entry.uri = m_tree->rootUri().child(entry.name);
        QVERIFY2(records.canPreview(entry), name);
    }

    // `.jsonl` is the one they both claim -- the highlighter maps it on to the
    // JSON rules, so the text viewer would show coloured source one record to a
    // line. Priority is what takes it off there, exactly as it does for a `.csv`.
    FileEntry lines;
    lines.name = QStringLiteral("events.jsonl");
    lines.uri = m_tree->rootUri().child(lines.name);
    QVERIFY(text.canPreview(lines));
    QVERIFY2(records.priority() > text.priority(), "the grid must win for a file of records");

    // `.ndjson` the text viewer never claimed, so before this viewer existed the
    // same file went to the list of facts. Worth stating: the two suffixes were
    // getting different answers for the same content.
    FileEntry other;
    other.name = QStringLiteral("events.ndjson");
    other.uri = m_tree->rootUri().child(other.name);
    QVERIFY(!text.canPreview(other));

    // A .json file is one document rather than a stream of records and stays
    // where it is. Whether an array of objects should open as a grid too is a
    // second question and not this viewer's.
    FileEntry document;
    document.name = QStringLiteral("config.json");
    document.uri = m_tree->rootUri().child(QStringLiteral("config.json"));
    QVERIFY(!records.canPreview(document));

    // And it offers the table by default, with the source as the other choice.
    FileEntry entry;
    entry.name = QStringLiteral("events.jsonl");
    entry.uri = m_tree->rootUri().child(QStringLiteral("events.jsonl"));
    const QList<ViewerOption> declared = records.options(entry);
    QCOMPARE(declared.size(), 1);
    QCOMPARE(declared.first().choices, QStringList({ QStringLiteral("Table"), QStringLiteral("Source") }));
    QCOMPARE(declared.first().defaultChoice, QStringLiteral("Table"));
}

void TestPreview::aFileOfFlatRecordsOpensAsAGridOfItsKeys()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("events.jsonl"),
        jsonLines({ QStringLiteral(R"({"when":"09:12","what":"opened","ok":true})"),
            QStringLiteral(R"({"when":"09:14","what":"closed","ok":false})"),
            QStringLiteral(R"({"when":"09:20","what":"opened","ok":true})") })));

    PreviewTabController* preview = openPreview(QStringLiteral("events.jsonl"));
    QVERIFY(preview);
    QCOMPARE(preview->providerId(), QStringLiteral("mole.preview.jsonlines"));

    auto* viewer = qobject_cast<JsonLinesPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return viewer->table()->rowCount() == 3; }, 5000));
    QVERIFY(!viewer->isShowingSource());

    // The keys, in the order the file first uses them -- not sorted, because the
    // order a record was written in is information about the file.
    QCOMPARE(viewer->table()->headers(),
        QStringList({ QStringLiteral("when"), QStringLiteral("what"), QStringLiteral("ok") }));

    QCOMPARE(viewer->table()->cellAt(0, 0), QStringLiteral("09:12"));
    QCOMPARE(viewer->table()->cellAt(0, 1), QStringLiteral("opened"));
    QCOMPARE(viewer->table()->cellAt(0, 2), QStringLiteral("true"));
    QCOMPARE(viewer->table()->cellAt(1, 2), QStringLiteral("false"));

    // The filter is SQL over the whole file rather than over what has arrived,
    // and it waits for the typing to stop -- hence QTRY rather than a compare.
    viewer->table()->setFilter(QStringLiteral("closed"));
    QTRY_COMPARE(viewer->table()->matchingRows(), 1);
}

void TestPreview::aNestedValueArrivesAsCompactJsonInItsCell()
{
    // The whole answer to a file with complicated structures in it: nesting never
    // breaks the grid, it makes a cell with JSON in it -- and because the filter
    // is a substring over every column, searching still finds what is inside one.
    QVERIFY(m_tree->writeFile(QStringLiteral("nested.jsonl"),
        jsonLines({ QStringLiteral(R"({"id":1,"user":{"name":"ada","tags":["a","b"]}})") })));

    PreviewTabController* preview = openPreview(QStringLiteral("nested.jsonl"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<JsonLinesPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return viewer->table()->rowCount() == 1; }, 5000));

    QCOMPARE(viewer->table()->cellAt(0, 0), QStringLiteral("1"));
    // Compact: one line, no indentation. A pretty-printed object would be a cell
    // with newlines in it, which is a row the grid cannot draw.
    QCOMPARE(viewer->table()->cellAt(0, 1), QStringLiteral(R"({"name":"ada","tags":["a","b"]})"));

    viewer->table()->setFilter(QStringLiteral("ada"));
    QTRY_COMPARE(viewer->table()->matchingRows(), 1);
}

void TestPreview::anAbsentKeyIsAnEmptyCellAndANullIsNot()
{
    // The difference is worth keeping: a record that does not mention a field and
    // one that says the field is empty are different facts about the data, and a
    // grid that showed both as blank would lose one of them.
    QVERIFY(m_tree->writeFile(QStringLiteral("sparse.jsonl"),
        jsonLines({ QStringLiteral(R"({"id":1,"note":"first"})"), QStringLiteral(R"({"id":2,"note":null})"),
            QStringLiteral(R"({"id":3})") })));

    PreviewTabController* preview = openPreview(QStringLiteral("sparse.jsonl"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<JsonLinesPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return viewer->table()->rowCount() == 3; }, 5000));

    QCOMPARE(viewer->table()->cellAt(0, 1), QStringLiteral("first"));
    QCOMPARE(viewer->table()->cellAt(1, 1), QStringLiteral("null"));
    QCOMPARE(viewer->table()->cellAt(2, 1), QString());
}

void TestPreview::aFileWhoseRecordsAreNotObjectsShowsItsSourceAndSaysWhy()
{
    // A file of arrays under a name that says records. There is no table to show,
    // so the source is shown whatever the preference says -- and the preference is
    // not written, because the reader did not ask for this.
    QVERIFY(m_tree->writeFile(QStringLiteral("arrays.jsonl"),
        jsonLines({ QStringLiteral("[1, 2, 3]"), QStringLiteral("[4, 5, 6]") })));

    PreviewTabController* preview = openPreview(QStringLiteral("arrays.jsonl"));
    QVERIFY(preview);
    QCOMPARE(preview->providerId(), QStringLiteral("mole.preview.jsonlines"));

    auto* viewer = qobject_cast<JsonLinesPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return viewer->isShowingSource(); }, 5000));

    QVERIFY2(!viewer->sourceReason().isEmpty(), "the reader has to be told why");
    QVERIFY(viewer->source() != nullptr);
    // The strip still says Table, because that is what was chosen and nothing has
    // changed it: the file decided this file, not the next one.
    QCOMPARE(preview->viewerOptions().first().toMap().value(QStringLiteral("chosen")).toString(),
        QStringLiteral("Table"));

    // And the next file of records opens as a grid, which is the test that the
    // preference really was left alone.
    QVERIFY(m_tree->writeFile(QStringLiteral("after.jsonl"), jsonLines({ QStringLiteral(R"({"id":1})") })));
    PreviewTabController* next = openPreview(QStringLiteral("after.jsonl"));
    QVERIFY(next);
    auto* nextViewer = qobject_cast<JsonLinesPreviewController*>(next->viewer());
    QVERIFY(nextViewer);
    QVERIFY(waitFor([nextViewer] { return nextViewer->table()->rowCount() == 1; }, 5000));
    QVERIFY(!nextViewer->isShowingSource());
}

void TestPreview::askingForTheSourceOfAFileOfRecordsIsObeyedAndSaysNothing()
{
    QVERIFY(m_tree->writeFile(
        QStringLiteral("readable.jsonl"), jsonLines({ QStringLiteral(R"({"id":1,"note":"first"})") })));

    PreviewTabController* preview = openPreview(QStringLiteral("readable.jsonl"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<JsonLinesPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return viewer->table()->rowCount() == 1; }, 5000));

    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Source"));
    QVERIFY(viewer->isShowingSource());
    QVERIFY(viewer->source() != nullptr);
    // Nothing to explain: the reader asked, and a sentence saying why would be
    // telling them what they just did.
    QVERIFY(viewer->sourceReason().isEmpty());

    auto* text = qobject_cast<TextPreviewController*>(viewer->source());
    QVERIFY(text);
    QVERIFY(waitFor([text] { return text->text().contains(QStringLiteral("\"note\"")); }, 5000));

    // Back again, because a choice that cannot be undone is a trap.
    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Table"));
    QVERIFY(!viewer->isShowingSource());

    // And remembered, so the next one opens the way this was left.
    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Source"));
    QVERIFY(m_tree->writeFile(QStringLiteral("second.jsonl"), jsonLines({ QStringLiteral(R"({"id":2})") })));
    PreviewTabController* next = openPreview(QStringLiteral("second.jsonl"));
    QVERIFY(next);
    auto* nextViewer = qobject_cast<JsonLinesPreviewController*>(next->viewer());
    QVERIFY(nextViewer);
    QVERIFY(nextViewer->isShowingSource());
    QVERIFY2(nextViewer->sourceReason().isEmpty(), "the reader's own answer needs no explanation");
}

void TestPreview::movingToAnotherFileWhileAnImportRunsLeavesItToFinishOnItsOwn()
{
    // Long enough that the import is certainly still running when the next file
    // opens. At a JSON parse per record that is seconds of work, and moving on
    // costs the reader one arrow key -- which is the ordinary way to hit this,
    // not an unusual one.
    QByteArray records;
    records.reserve(200000 * 40);
    for (int i = 0; i < 200000; ++i)
        records += QStringLiteral("{\"id\":%1,\"name\":\"name %1\"}\n").arg(i).toUtf8();
    QVERIFY(m_tree->writeFile(QStringLiteral("huge.jsonl"), records));
    QVERIFY(m_tree->writeFile(QStringLiteral("next.jsonl"), jsonLines({ QStringLiteral(R"({"id":1})") })));

    PreviewTabController* preview = openPreview(QStringLiteral("huge.jsonl"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<JsonLinesPreviewController*>(preview->viewer());
    QVERIFY(viewer);

    // Underway, and asserted on the condition: records in the grid mean the task
    // is inside its loop with the store bound and a batch in its hands.
    QVERIFY(waitFor([viewer] { return viewer->isImporting() && viewer->table()->totalRows() > 0; }, 30000));

    // The task itself, so how it ends is asserted rather than assumed.
    QPointer<Task> importing;
    for (Task* task : m_app->services().tasks->tasks()) {
        if (qobject_cast<ImportJsonLinesTask*>(task) && !task->isFinished())
            importing = task;
    }
    QVERIFY2(importing, "the import has to still be running, or this case is testing nothing");

    // Watched from here, because what the fault says out loud is a warning: the
    // write into the destroyed store fails, and a failure with no message is
    // reported against a file the reader has already left. `make asan` sees the
    // use-after-free itself; this sees it in a plain build too.
    CapturedWarnings warnings;

    // The next file. The tab deletes the viewer before the new one arrives, and
    // the viewer used to take the store and the scratch directory with it --
    // while the task was still binding rows into that database from a pool
    // thread.
    PreviewTabController* next = openPreview(QStringLiteral("next.jsonl"));
    QVERIFY(next);
    auto* nextViewer = qobject_cast<JsonLinesPreviewController*>(next->viewer());
    QVERIFY(nextViewer);

    // The tab deletes the old viewer with deleteLater(), and a deferred delete
    // needs an event loop turn. The application has one running; a test has to
    // ask, and not asking would leave the outgoing viewer alive for the whole
    // case -- which is to say it would test nothing at all.
    drainEvents();

    // The outgoing task runs out on its own, into a store nobody is reading.
    QVERIFY(waitFor([importing] { return importing && importing->isFinished(); }, 30000));
    // Cancelled and not Failed: a store the reader has moved on from is not an
    // I/O error, and there is nobody left to report one to.
    QCOMPARE(importing->state(), Task::State::Cancelled);
    QVERIFY2(!warnings.contains(QStringLiteral("failed")),
        qPrintable(
            QStringLiteral("the import reported a failure on its way out: %1").arg(warnings.joined())));

    // And the file that replaced it is the one on the screen.
    QVERIFY(waitFor(
        [nextViewer] { return !nextViewer->isImporting() && nextViewer->table()->rowCount() == 1; }, 30000));
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

/// The one that will be missed. A document is formatted when it is loaded and
/// nothing asks again, so a source file open when the theme flips keeps the
/// colours it was opened under -- pastel green on white, at about 1.7:1.
void TestPreview::aFileAlreadyColouredFollowsTheThemesPolarity()
{
    QTextDocument document;
    document.setPlainText(QStringLiteral("// a comment\nint x = 1;\n"));

    SourceHighlighter highlighter;
    highlighter.setDocument(&document);
    highlighter.setLanguage(QStringLiteral("cpp"));

    const auto commentColour = [&document]() -> QColor {
        const QTextBlock block = document.findBlockByNumber(0);
        const QList<QTextLayout::FormatRange> runs = block.layout()->formats();
        return runs.isEmpty() ? QColor() : runs.constFirst().format.foreground().color();
    };

    // Index seven of the nine is the comment; the whole first line is one.
    const QColor darkComment = QColor(SourceHighlighter::coloursFor(false).at(7));
    const QColor lightComment = QColor(SourceHighlighter::coloursFor(true).at(7));
    QVERIFY(darkComment != lightComment);

    QVERIFY(commentColour().isValid());
    QCOMPARE(commentColour(), darkComment);

    highlighter.setLightBackground(true);
    QCOMPARE(commentColour(), lightComment);

    // And back, because a reader who tries a light theme and does not like it is
    // the commonest way this gets exercised.
    highlighter.setLightBackground(false);
    QCOMPARE(commentColour(), darkComment);
}

void TestPreview::coloursFilesWhoseNameIsTheirType_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<QString>("languageName");

    // Three languages that have existed since the highlighter was written and
    // could not be reached: no suffix maps to dockerfile at all, and only .mk
    // and .cmake reach the other two.
    QTest::newRow("Dockerfile") << "Dockerfile"
                                << QByteArray("FROM debian:bookworm\n# a comment\nRUN make all\n")
                                << "Dockerfile";
    QTest::newRow("Dockerfile.build")
        << "Dockerfile.build" << QByteArray("FROM alpine\nARG VERSION\n") << "Dockerfile";
    QTest::newRow("Containerfile") << "Containerfile" << QByteArray("FROM alpine\n") << "Dockerfile";
    QTest::newRow("Makefile") << "Makefile" << QByteArray("all:\n\tgcc -o x x.c\n") << "Makefile";
    QTest::newRow("GNUmakefile") << "GNUmakefile" << QByteArray("include config.mk\nall:\n") << "Makefile";
    QTest::newRow("CMakeLists.txt") << "CMakeLists.txt"
                                    << QByteArray("project(mole)\nif(NOT WIN32)\nendif()\n") << "CMake";
    QTest::newRow("bashrc") << ".bashrc" << QByteArray("export PS1='$ '\nalias ll='ls -l'\n") << "Shell";
    QTest::newRow("editorconfig") << ".editorconfig" << QByteArray("root = true\n") << "INI / TOML";

    // And the case the name cannot answer: a script with no suffix and no
    // conventional name, coloured because the content pass knows what it is.
    QTest::newRow("a script called deploy")
        << "deploy" << QByteArray("#!/bin/sh\nset -e\necho deploying\n") << "Shell";
}

void TestPreview::coloursFilesWhoseNameIsTheirType()
{
    QFETCH(QString, fileName);
    QFETCH(QByteArray, contents);
    QFETCH(QString, languageName);

    QVERIFY(m_tree->writeFile(fileName, contents));

    PreviewTabController* preview = openPreview(fileName);
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY2(viewer, qPrintable(QStringLiteral("%1 reached %2").arg(fileName, preview->viewerName())));
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));

    QCOMPARE(viewer->languageName(), languageName);
    QVERIFY(viewer->isHighlighted());
}

void TestPreview::textWithNothingToColourOpensPlain_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QByteArray>("contents");

    QTest::newRow("gitignore") << ".gitignore" << QByteArray("build/\n*.o\n");
    QTest::newRow("LICENSE") << "LICENSE" << QByteArray("MIT License\n\nPermission is hereby granted\n");
}

void TestPreview::textWithNothingToColourOpensPlain()
{
    QFETCH(QString, fileName);
    QFETCH(QByteArray, contents);

    QVERIFY(m_tree->writeFile(fileName, contents));

    PreviewTabController* preview = openPreview(fileName);
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY2(viewer, qPrintable(QStringLiteral("%1 reached %2").arg(fileName, preview->viewerName())));
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));

    // Shown, not coloured, and no complaint about either: an unknown language is
    // no language rather than a wrong guess.
    QVERIFY(!viewer->isHighlighted());
    QVERIFY(viewer->languageName().isEmpty());
    QVERIFY2(viewer->errorText().isEmpty(), qPrintable(viewer->errorText()));
}

void TestPreview::aHugeFileWithNoSuffixOpensOnItsFirstWindow()
{
    // Two gigabytes, sparse: the point is the offsets, not the bytes, and a test
    // that really wrote them would be a test nobody runs. No newline in the
    // first window either, which is the shape a one-line dump arrives in.
    constexpr qint64 kSize = 2LL * 1024 * 1024 * 1024;
    const QString path = m_tree->absolute(QStringLiteral("dump8842"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray head;
        while (head.size() < 1024 * 1024)
            head += QByteArrayLiteral("{\"id\":1,\"name\":\"row\"},");
        QCOMPARE(file.write(head), head.size());
        QVERIFY2(file.resize(kSize), qPrintable(file.errorString()));
    }
    if (QFileInfo(path).size() != kSize)
        QSKIP("this filesystem would not make a sparse file");

    // Through a drive that counts, because "it opened" is only half of it: what
    // must not happen is the file being read to find that out.
    auto counted = std::make_shared<FaultyFileSystem>(std::make_shared<LocalFileSystem>());
    Mount mount;
    mount.id = QStringLiteral("counted");
    mount.displayName = QStringLiteral("counted");
    mount.root = m_tree->rootUri();
    mount.fileSystem = counted;
    m_app->services().vfs->addMount(mount);

    m_app->previewFile(VfsUri::fromLocalPath(path).toString());
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    QVERIFY(preview);
    QVERIFY(waitFor([preview] { return preview->viewer() != nullptr; }, 10000));

    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY2(viewer, qPrintable(QStringLiteral("reached %1").arg(preview->viewerName())));
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 10000));

    QCOMPARE(viewer->fileSize(), kSize);
    QCOMPARE(viewer->windowBytes(), 512 * 1024);
    QVERIFY(viewer->isPaged());

    // One page to identify it and one window to show it. Anything else means
    // something read towards the end of a file it was only asked the start of --
    // and the lower bound is there so a drive that was never asked at all cannot
    // pass this by reading nothing.
    QVERIFY2(counted->bytesRead() >= 512 * 1024, "the window has to have come through this drive");
    QVERIFY2(counted->bytesRead() <= FileType::kSampleBytes + 512 * 1024 + 2,
        qPrintable(QStringLiteral("read %1 bytes of a 2 GB file").arg(counted->bytesRead())));
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

// --------------------------------------------- files with no lines in them

// A 13 MB minified export left the window not answering, and the size was not
// why: the preview reads a 512 kB window and never holds the file. What matters
// is that the window has no line break in it, so everything downstream -- the
// layout, which shapes a block whole, and the highlighter, which colours per
// block -- is handed one line of over half a million characters.

void TestPreview::aWindowWithNoLineBreaksIsFoldedAndLeftUncoloured()
{
    const QByteArray minified = minifiedJson(600 * 1024);
    QVERIFY(m_tree->writeFile(QStringLiteral("export.json"), minified));

    PreviewTabController* preview = openPreview(QStringLiteral("export.json"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));

    // Still a whole window of the file. What changed is the shape it arrives in.
    QVERIFY2(viewer->windowBytes() > 400 * 1024, "the window is read as it always was");
    QVERIFY2(longestLine(viewer->text()) <= TextPreviewController::kFoldedLineChars,
        "a block this long is itemised and shaped whole, on the GUI thread");
    QVERIFY(viewer->longLinesFolded());

    // The file is still JSON and the header still says so; what is off is the
    // colouring of this window, because a fold cuts strings in half and the
    // highlighter carries no state across a break except a block comment.
    QCOMPARE(viewer->languageName(), QStringLiteral("JSON"));
    QVERIFY2(!viewer->isHighlighted(), "colouring is off for a folded window");
}

void TestPreview::aWindowOfTheSameSizeWithLinesInItIsUntouched()
{
    const QByteArray lined = linedJson(600 * 1024);
    QVERIFY(m_tree->writeFile(QStringLiteral("records.json"), lined));

    PreviewTabController* preview = openPreview(QStringLiteral("records.json"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));

    QVERIFY(!viewer->longLinesFolded());
    QVERIFY(viewer->isHighlighted());
    // Not one character added or moved: the window as it was read.
    QCOMPARE(viewer->text(), QString::fromUtf8(lined.left(viewer->windowBytes())));
}

void TestPreview::pagingOnFromAFoldedWindowGetsColouringBack()
{
    // One line filling most of the first window, then ordinary lines, so the
    // first window folds and the second does not. Folding is a property of the
    // window rather than of the file.
    QByteArray mixed = minifiedJson(500 * 1024);
    mixed += '\n';
    mixed += linedJson(200 * 1024);
    QVERIFY(m_tree->writeFile(QStringLiteral("mixed.json"), mixed));

    PreviewTabController* preview = openPreview(QStringLiteral("mixed.json"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));

    QVERIFY(viewer->longLinesFolded());
    QVERIFY(!viewer->isHighlighted());
    QVERIFY2(!viewer->isAtEnd(), "there is a window after this one");

    viewer->nextWindow();
    QVERIFY(waitFor([viewer] { return viewer->windowOffset() > 0; }, 5000));
    QVERIFY2(!viewer->longLinesFolded(), "this window has lines in it");
    QVERIFY2(viewer->isHighlighted(), "so the colouring comes back");
    QVERIFY(longestLine(viewer->text()) < TextPreviewController::kFoldedLineChars);
}

// ------------------------------ markdown the window cannot afford to render

// A 238 kB generated report -- one row per record, a table 2,182 rows by 14
// columns -- took the window for over three seconds and there was nothing to be
// done about it once it had started: behind `textFormat: MarkdownText` the
// TextArea owns the document, so QTextDocument::setMarkdown() runs inside the
// item on the thread that draws and cannot be cancelled. The cost is the table
// and not the size, and the import is quadratic in its rows.
//
// Every case here asserts the *decision* and never a duration. A timing
// assertion on a build machine is a flake, and the numbers behind the budget are
// in MOLE-283 along with the four lines that measure them.

namespace {

/// A Markdown file whose largest table has `rows` rows in it.
///
/// Generated rather than shipped: the whole variable is how many rows there are,
/// and a fixture would have to be regenerated to move the budget. Some prose
/// first, so the file is not only a table and the run being measured is a run
/// inside something.
QByteArray markdownWithTable(int rows, int columns = 4)
{
    QByteArray out = QByteArrayLiteral("# Freshness\n\nOne row per record, generated nightly.\n\n");

    QByteArray header = QByteArrayLiteral("|");
    QByteArray rule = QByteArrayLiteral("|");
    for (int c = 0; c < columns; ++c) {
        header += QByteArrayLiteral(" column ") + QByteArray::number(c) + QByteArrayLiteral(" |");
        rule += QByteArrayLiteral("---|");
    }
    out += header + '\n' + rule + '\n';

    for (int r = 0; r < rows; ++r) {
        out += '|';
        for (int c = 0; c < columns; ++c) {
            out += QByteArrayLiteral(" record-") + QByteArray::number(r) + QByteArrayLiteral("-")
                + QByteArray::number(c) + QByteArrayLiteral(" |");
        }
        out += '\n';
    }
    return out;
}

/// Markdown with no table anywhere in it, at least `bytes` long.
QByteArray markdownProse(qsizetype bytes)
{
    QByteArray out = QByteArrayLiteral("# Notes\n\n");
    for (int i = 0; out.size() < bytes; ++i) {
        out += QByteArrayLiteral("## Section ") + QByteArray::number(i)
            + QByteArrayLiteral("\n\nA paragraph of prose about the section above it, with "
                                "`inline code` and a [link](https://example.invalid/) in it, long "
                                "enough that the file gets somewhere.\n\n- A list item.\n- Another.\n\n");
    }
    return out;
}

} // namespace

void TestPreview::aMarkdownFileWithAHugeTableOpensAsSourceAndSaysWhy()
{
    const int rows = static_cast<int>(TextPreviewController::kMarkdownTableRows) + 200;
    QVERIFY(m_tree->writeFile(QStringLiteral("freshness.md"), markdownWithTable(rows)));

    PreviewTabController* preview = openPreview(QStringLiteral("freshness.md"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));

    QVERIFY2(viewer->markdownDeclined(), "a table this size takes the window for seconds");
    QVERIFY2(!viewer->isMarkdown(), "so nothing hands it to the importer");

    // The header row and the rule are rows of the run as well, which is right:
    // they are blocks the importer builds like any other.
    QCOMPARE(viewer->markdownTableRows(), static_cast<qsizetype>(rows) + 2);

    // Source, and all of it: declining to render is not declining to show.
    QVERIFY(viewer->text().contains(QStringLiteral("| record-0-0 |")));
    QVERIFY(viewer->text().contains(QStringLiteral("# Freshness")));

    // And said out loud, with the figure that decided it, because otherwise a
    // reader is looking at markup with no idea why.
    QVERIFY2(!viewer->markdownDeclinedNote().isEmpty(), "the reader has to be told");
    QVERIFY2(viewer->markdownDeclinedNote().contains(QStringLiteral("source")),
        qPrintable(viewer->markdownDeclinedNote()));
}

void TestPreview::aMarkdownFileUnderTheBudgetIsRenderedAsBefore()
{
    const int rows = static_cast<int>(TextPreviewController::kMarkdownTableRows) / 4;
    QVERIFY(m_tree->writeFile(QStringLiteral("small.md"), markdownWithTable(rows)));

    PreviewTabController* preview = openPreview(QStringLiteral("small.md"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));

    QVERIFY(viewer->isMarkdown());
    QVERIFY(!viewer->markdownDeclined());
    QVERIFY(viewer->markdownDeclinedNote().isEmpty());
}

void TestPreview::theBudgetIsTableRowsRatherThanTheSizeOfTheFile()
{
    // The claim the whole design rests on: a file's size says nothing about what
    // rendering it will cost. 235 kB of prose Markdown parses, lays out and
    // restyles in 122 ms altogether; 238 kB whose largest table is 2,182 rows
    // spends 2,676 ms in setMarkdown() alone. A cap on bytes would have refused
    // the first of these and admitted the second.
    const QByteArray prose = markdownProse(300 * 1024);
    QVERIFY(prose.size() > 300 * 1024);
    QVERIFY(m_tree->writeFile(QStringLiteral("long-prose.md"), prose));

    PreviewTabController* big = openPreview(QStringLiteral("long-prose.md"));
    QVERIFY(big);
    auto* proseViewer = qobject_cast<TextPreviewController*>(big->viewer());
    QVERIFY(proseViewer);
    QVERIFY(waitFor([proseViewer] { return !proseViewer->text().isEmpty(); }, 5000));

    QVERIFY2(proseViewer->isMarkdown(), "a large file with no table in it renders as it always did");
    QVERIFY(!proseViewer->markdownDeclined());
    QCOMPARE(proseViewer->markdownTableRows(), qsizetype(0));

    // And the other half: a file a fifth of the size, declined.
    const QByteArray tabular
        = markdownWithTable(static_cast<int>(TextPreviewController::kMarkdownTableRows) + 1);
    QVERIFY2(tabular.size() < prose.size() / 4, "the declined file is much the smaller of the two");
    QVERIFY(m_tree->writeFile(QStringLiteral("smaller-table.md"), tabular));

    PreviewTabController* small = openPreview(QStringLiteral("smaller-table.md"));
    QVERIFY(small);
    auto* tableViewer = qobject_cast<TextPreviewController*>(small->viewer());
    QVERIFY(tableViewer);
    QVERIFY(waitFor([tableViewer] { return !tableViewer->text().isEmpty(); }, 5000));

    QVERIFY(tableViewer->markdownDeclined());
}

void TestPreview::askingForThePageRendersADeclinedFile()
{
    const int rows = static_cast<int>(TextPreviewController::kMarkdownTableRows) + 200;
    QVERIFY(m_tree->writeFile(QStringLiteral("report.md"), markdownWithTable(rows)));

    PreviewTabController* preview = openPreview(QStringLiteral("report.md"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));
    QVERIFY(viewer->markdownDeclined());

    // The strip shows Rendered, because that is what a Markdown file does -- the
    // decline is about this file, not about what was chosen.
    QCOMPARE(preview->viewerOptions().size(), 1);
    QCOMPARE(preview->viewerOptions().first().toMap().value(QStringLiteral("chosen")).toString(),
        QStringLiteral("Rendered"));

    // Asked for anyway: somebody who wants the page of a huge report can have it
    // and wait for it, which is the difference between a guard and a refusal.
    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Rendered"));
    QVERIFY2(viewer->isMarkdown(), "asked for, so rendered");
    QVERIFY(!viewer->markdownDeclined());
    QVERIFY(viewer->markdownDeclinedNote().isEmpty());

    // Remembered, so the next huge report opens rendered without being asked
    // again. Per suffix, as every viewer choice is -- see ADR-0006.
    QVERIFY(m_tree->writeFile(QStringLiteral("second.md"), markdownWithTable(rows)));
    PreviewTabController* next = openPreview(QStringLiteral("second.md"));
    QVERIFY(next);
    auto* nextViewer = qobject_cast<TextPreviewController*>(next->viewer());
    QVERIFY(nextViewer);
    QVERIFY(waitFor([nextViewer] { return !nextViewer->text().isEmpty(); }, 5000));
    QVERIFY2(nextViewer->isMarkdown(), "the answer was remembered, so it is not asked again");
    QVERIFY(!nextViewer->markdownDeclined());
}

void TestPreview::askingForTheSourceOfAnOrdinaryMarkdownFileIsObeyed()
{
    // The other direction, and the reason the choice exists at all for a file
    // nothing was ever going to decline: somebody reading a README as markup.
    QVERIFY(m_tree->writeFile(QStringLiteral("readme.md"), QByteArray("# Title\n\nProse.\n")));

    PreviewTabController* preview = openPreview(QStringLiteral("readme.md"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));
    QVERIFY(viewer->isMarkdown());

    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Source"));
    QVERIFY(!viewer->isMarkdown());
    // Not the same state as a decline: nothing was declined, so nothing is said.
    QVERIFY(!viewer->markdownDeclined());
    QVERIFY(viewer->markdownDeclinedNote().isEmpty());
    QVERIFY(viewer->text().contains(QStringLiteral("# Title")));

    preview->chooseViewerOption(QStringLiteral("mode"), QStringLiteral("Rendered"));
    QVERIFY(viewer->isMarkdown());
}

void TestPreview::markdownOffersThePageByDefaultAndTheSourceOnRequest()
{
    TextPreviewProvider provider(m_app->services());

    FileEntry markdown;
    markdown.name = QStringLiteral("guide.md");
    markdown.uri = m_tree->rootUri().child(QStringLiteral("guide.md"));

    const QList<ViewerOption> declared = provider.options(markdown);
    QCOMPARE(declared.size(), 1);
    QCOMPARE(declared.first().key, QStringLiteral("mode"));
    QCOMPARE(declared.first().choices, QStringList({ QStringLiteral("Source"), QStringLiteral("Rendered") }));
    // The reverse of the .html default, and for the same reason read the other
    // way: Markdown is written to be read as prose, and a page of HTML in a file
    // manager is usually worth reading as what it is.
    QVERIFY2(
        declared.first().defaultChoice == QStringLiteral("Rendered"), "a Markdown file is worth rendering");

    for (const char* name : { "notes.md", "notes.markdown", "notes.mdown", "notes.mkd", "NOTES.MD" }) {
        FileEntry entry;
        entry.name = QLatin1String(name);
        entry.uri = m_tree->rootUri().child(entry.name);
        QVERIFY2(provider.options(entry).size() == 1, name);
    }

    // And nothing changes for the files that never had a choice to make.
    FileEntry log;
    log.name = QStringLiteral("notes.txt");
    log.uri = m_tree->rootUri().child(QStringLiteral("notes.txt"));
    QVERIFY(provider.options(log).isEmpty());
}

void TestPreview::pagingOnFromADeclinedWindowRendersAgain()
{
    // A property of the window and not of the file, like the fold: a report whose
    // tables are all at the front is prose from the second window on, and there
    // is nothing there to decline.
    // Sized so the arithmetic is not doing the work: the table alone is more than
    // one window, and the prose after it is more than one window, so the first
    // window is nothing but table and the last is nothing but prose.
    const QByteArray table = markdownWithTable(2500, 14);
    QVERIFY2(table.size() > TextPreviewController::kWindowBytes, "the first window has to be all table");
    QByteArray mixed = table;
    mixed += markdownProse(TextPreviewController::kWindowBytes + 128 * 1024);
    QVERIFY(m_tree->writeFile(QStringLiteral("mixed.md"), mixed));

    PreviewTabController* preview = openPreview(QStringLiteral("mixed.md"));
    QVERIFY(preview);
    auto* viewer = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QVERIFY(waitFor([viewer] { return !viewer->text().isEmpty(); }, 5000));

    QVERIFY(viewer->markdownDeclined());
    QVERIFY2(!viewer->isAtEnd(), "there is a window after this one");

    viewer->lastWindow();
    QVERIFY(waitFor([viewer] { return viewer->windowOffset() > 0; }, 5000));
    QVERIFY2(!viewer->markdownDeclined(), "this window is prose");
    QVERIFY2(viewer->isMarkdown(), "so it renders");
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

// ---------------------------------------------------------------- databases

void TestPreview::aDatabaseListsItsTablesBeforeItHasCountedAnyOfThem()
{
    // Opening a database used to count every row of every table and view before
    // it drew anything: once per name for the picker, twice because the binding
    // reads it twice, and a third time for the table the grid landed on. A
    // COUNT(*) is a walk of the table, so a file of a few large tables held the
    // window for as long as it took to walk all of them.
    const QString path = m_tree->absolute(QStringLiteral("catalogue.sqlite"));
    QVERIFY(fixtures::writeSqlite(path));

    FileEntry entry;
    entry.name = QStringLiteral("catalogue.sqlite");
    entry.uri = m_tree->rootUri().child(entry.name);

    SqlitePreviewController viewer(m_app->services());
    QSignalSpy schema(&viewer, &SqlitePreviewController::schemaChanged);
    viewer.load(entry);

    // The file is local, so it is opened inside that call -- and the counting
    // runs on a pool thread and reports back through queued signals, none of
    // which can be delivered until this returns to the event loop. So what is
    // read here is what the window would have on its first frame.
    const QVariantList firstFrame = viewer.tables();
    QCOMPARE(firstFrame.size(), 3);
    QStringList named;
    for (const QVariant& row : firstFrame) {
        const QVariantMap table = row.toMap();
        named.append(table.value(QStringLiteral("name")).toString());
        QVERIFY2(table.value(QStringLiteral("rowsText")).toString().isEmpty(),
            "a count nobody has taken yet is a blank, not a nought and not a guess");
    }
    QCOMPARE(named, QStringList({ "adults", "order", "people" }));
    QVERIFY2(viewer.table()->totalRows() < 0, "and the same for the table the grid landed on");
    QVERIFY2(schema.count() > 0, "the names are on screen from the first frame");
    QVERIFY2(!viewer.summary().contains(QStringLiteral("rows")), qPrintable(viewer.summary()));

    // And then they arrive, each one where it belongs. The table the grid is
    // showing is counted first, because it is the only count anybody is
    // already looking at.
    QTRY_COMPARE(viewer.table()->totalRows(), 3);
    QVERIFY2(viewer.summary().contains(QStringLiteral("3 rows")), qPrintable(viewer.summary()));
    QTRY_COMPARE(
        viewer.tables().at(2).toMap().value(QStringLiteral("rowsText")).toString(), QLocale().toString(6));
    QCOMPARE(
        viewer.tables().at(1).toMap().value(QStringLiteral("rowsText")).toString(), QLocale().toString(2));
}

void TestPreview::typingAFilterScansOnceRatherThanOncePerCharacter()
{
    // The filter is `CAST(<column> AS TEXT) LIKE '%...%'` over every column at
    // once, which no index can answer -- so it is a full scan, and it used to be
    // one per character typed, on the thread that draws.
    const QString path = m_tree->absolute(QStringLiteral("catalogue.sqlite"));
    QVERIFY(fixtures::writeSqlite(path));

    FileEntry entry;
    entry.name = QStringLiteral("catalogue.sqlite");
    entry.uri = m_tree->rootUri().child(entry.name);

    SqlitePreviewController viewer(m_app->services());
    viewer.load(entry);
    // The table with something to filter in it, once its count has landed.
    viewer.setCurrentTable(QStringLiteral("people"));
    QTRY_COMPARE(viewer.table()->totalRows(), 6);

    // Counted from the model resetting, which is what a filter costs the view:
    // every cached page dropped, every row refetched, and the scan that says how
    // many there are.
    QSignalSpy refreshed(viewer.table(), &QAbstractItemModel::modelReset);
    for (const QString& typed : { QStringLiteral("B"), QStringLiteral("Be"), QStringLiteral("Ber"),
             QStringLiteral("Berl"), QStringLiteral("Berli") }) {
        viewer.table()->setFilter(typed);
    }

    // What was typed is on screen at once -- the field is not made to lag behind
    // the keyboard -- while the scan waits for the typing to stop.
    QCOMPARE(viewer.table()->filter(), QStringLiteral("Berli"));
    QCOMPARE(refreshed.count(), 0);

    QTRY_COMPARE(viewer.table()->matchingRows(), 1);
    QCOMPARE(refreshed.count(), 1);
    QCOMPARE(viewer.table()->cellAt(0, 1), QStringLiteral("Grace"));
}

void TestPreview::aDatabaseTableIsShownAPageAtATime()
{
    // Reached the way the window reaches it: through the controller, the model
    // it owns and the SQLite source behind that. A table used to be offered
    // whole, so the offset the model fetched at was an offset into the file and
    // one drag of the scrollbar issued a run of `OFFSET 9000000` queries.
    const QString path = m_tree->absolute(QStringLiteral("wide.sqlite"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("pager"));
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE readings (n INTEGER)")));
        QVERIFY(query.exec(QStringLiteral("BEGIN")));
        // Two full pages and a short one, so there is a last page that is not
        // the same shape as the others.
        for (int i = 0; i < 12000; ++i)
            QVERIFY(query.exec(QStringLiteral("INSERT INTO readings VALUES (%1)").arg(i)));
        QVERIFY(query.exec(QStringLiteral("COMMIT")));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("pager"));

    FileEntry entry;
    entry.name = QStringLiteral("wide.sqlite");
    entry.uri = m_tree->rootUri().child(entry.name);

    SqlitePreviewController viewer(m_app->services());
    viewer.load(entry);
    QTRY_COMPARE(viewer.table()->totalRows(), 12000);

    QCOMPARE(viewer.table()->pageCount(), 3);
    QCOMPARE(viewer.table()->rowCount(), TableModel::kPageRows);
    QCOMPARE(viewer.table()->cellAt(0, 0), QStringLiteral("0"));

    viewer.table()->nextPage();
    QCOMPARE(viewer.table()->firstRowOnPage(), TableModel::kPageRows);
    QCOMPARE(viewer.table()->cellAt(0, 0), QStringLiteral("5000"));

    viewer.table()->lastPage();
    QCOMPARE(viewer.table()->page(), 2);
    QCOMPARE(viewer.table()->rowCount(), 2000);
    QCOMPARE(viewer.table()->cellAt(1999, 0), QStringLiteral("11999"));

    // A different table is a different set of rows, so the page goes back to
    // the first rather than staying where the last table left it.
    viewer.setCurrentTable(QStringLiteral("readings"));
    viewer.table()->lastPage();
    QCOMPARE(viewer.table()->page(), 2);
    viewer.table()->setFilter(QStringLiteral("999"));
    viewer.table()->applyFilter();
    QCOMPARE(viewer.table()->page(), 0);
}

void TestPreview::reportsFactsForAnUnknownFile()
{
    // What is left for the fact list now that the bytes have a viewer of their
    // own: a file with nothing in it to show. Nothing read it, so nothing
    // identified it, and its size and dates are all there is to say -- which is
    // the details panel, opened because for this viewer it is the content.
    QVERIFY(m_tree->writeFile(QStringLiteral("nothing-in-it"), QByteArray()));

    PreviewTabController* preview = openPreview(QStringLiteral("nothing-in-it"));
    QVERIFY(preview);
    QCOMPARE(preview->viewerName(), QStringLiteral("File information"));

    auto* viewer = qobject_cast<FileInfoPreviewController*>(preview->viewer());
    QVERIFY(viewer);
    QCOMPARE(viewer->headline(), QStringLiteral("nothing-in-it"));

    // The facts are this viewer's content, so it asks for them whether or not
    // the drawer is open -- which is what FileInfoPreview.qml does on load.
    QVERIFY2(!preview->isDetailsOpen(), "and it does not turn the drawer on to do it");
    preview->requestDetails();
    QVERIFY(waitFor([preview] { return !preview->details().isEmpty(); }, 5000));
    QVERIFY2(preview->details().size() >= 5, "an unknown file still has plenty to say about it");
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

// ---- F3 on a file compressed on its own ---------------------------------
//
// `notes.txt.gz` used to show nine facts out of stat(), because no viewer claims
// application/gzip and none should: what a reader wanted was the text inside.
// MOLE-216 gave the member an address; this substitutes it, so every viewer Mole
// already has works through the wrapper -- a .gz of a CSV is a table and of a PNG
// is the picture, with no new provider, controller or QML anywhere. ADR-0033
// already says the first answer about a file can be wrong and its contents settle
// it; this is a third reason and not about identification. See MOLE-219.

void TestPreview::f3OnAFileCompressedOnItsOwnShowsWhatIsInside_data()
{
    QTest::addColumn<QString>("memberName");
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<QString>("viewerName");

    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "png");
        writer.write(image);
    }

    QTest::newRow("text") << "inside.txt" << QByteArray("a line of text inside a wrapper\n") << "Text";
    // The one that proves the substitution goes through the ordinary lookup
    // rather than a text special case.
    QTest::newRow("csv") << "inside.csv" << QByteArray("name;price\nwidget;1,50\nbolt;0,99\n") << "Table";
    QTest::newRow("png") << "inside.png" << png << "Image";
}

void TestPreview::f3OnAFileCompressedOnItsOwnShowsWhatIsInside()
{
    QFETCH(QString, memberName);
    QFETCH(QByteArray, contents);
    QFETCH(QString, viewerName);

    if (!withArchiveBackend())
        QSKIP("this build has no backend that can open an archive");
    QVERIFY(m_tree->writeFile(memberName, contents));
    if (!gzipInPlace(memberName))
        QSKIP("gzip is not available");

    PreviewTabController* preview = openPreview(memberName + QStringLiteral(".gz"));
    QVERIFY(preview);
    QCOMPARE(preview->viewerName(), viewerName);
    // The viewer names the member, not the wrapper, so it is clear what is being
    // read -- while the arrows and the session still work on the file in the
    // folder, which is what currentUri answers.
    QCOMPARE(preview->title(), memberName);
    QVERIFY(preview->currentUri().endsWith(memberName + QStringLiteral(".gz")));
    QCOMPARE(internalMounts(), 1);
}

void TestPreview::aContainerIsNotSubstituted_data()
{
    QTest::addColumn<QString>("fileName");

    // A tarball has many members and F3 on it goes on doing what it did. The name
    // test comes first for exactly this: confirming one member costs a header
    // read, and there is no reason to spend it on something already ruled out.
    QTest::newRow("tar.gz") << "bundle.tar.gz";
    QTest::newRow("zip") << "bundle.zip";
}

void TestPreview::aContainerIsNotSubstituted()
{
    QFETCH(QString, fileName);

    if (!withArchiveBackend())
        QSKIP("this build has no backend that can open an archive");

    // Built by hand rather than by a tool: what matters is that it is a container
    // by name, and the name is where this decision is taken.
    QVERIFY(m_tree->writeFile(fileName, QByteArray(512, '\x1f')));

    PreviewTabController* preview = openPreview(fileName);
    QVERIFY(preview);
    // The wrapper itself, named as itself, with nothing mounted for it.
    QCOMPARE(preview->title(), fileName);
    QCOMPARE(internalMounts(), 0);
    QVERIFY2(!preview->viewerName().isEmpty(), "a container still gets whatever viewer it always got");
}

void TestPreview::aFileNamedGzThatIsNotGzipKeepsTodaysBehaviour()
{
    if (!withArchiveBackend())
        QSKIP("this build has no backend that can open an archive");

    // The name says one member; the bytes say nothing at all. The answer is
    // today's -- a viewer for the file as it stands, not an error where a preview
    // belongs.
    QVERIFY(
        m_tree->writeFile(QStringLiteral("pretending.gz"), QByteArray("gzip starts 1f 8b; this does not")));

    PreviewTabController* preview = openPreview(QStringLiteral("pretending.gz"));
    QVERIFY(preview);
    QCOMPARE(preview->title(), QStringLiteral("pretending.gz"));
    QCOMPARE(internalMounts(), 0);
    QVERIFY2(!preview->viewerName().isEmpty(), "a file that is not an archive still gets a viewer");
}

void TestPreview::steppingOnReleasesTheSubstitutedMember()
{
    if (!withArchiveBackend())
        QSKIP("this build has no backend that can open an archive");
    QVERIFY(m_tree->writeFile(QStringLiteral("aaa.txt"), QByteArray("inside the wrapper\n")));
    if (!gzipInPlace(QStringLiteral("aaa.txt")))
        QSKIP("gzip is not available");

    PreviewTabController* preview = openPreview(QStringLiteral("aaa.txt.gz"));
    QVERIFY(preview);
    QCOMPARE(internalMounts(), 1);

    // The arrows work on the folder, so the siblings have to have arrived before
    // stepping means anything. Waited on the condition, not on a duration.
    QVERIFY(waitFor([preview] { return preview->position() > 0; }, 5000));
    preview->next();
    QVERIFY(waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); }, 5000));

    // The wrapper goes with the file it belonged to: a walk along a folder of
    // two hundred of these must not leave two hundred mounts behind.
    QCOMPARE(internalMounts(), 0);
    QVERIFY2(!preview->title().endsWith(QStringLiteral(".txt.gz")), "the tab is still showing the wrapper");
    QVERIFY2(preview->title() != QStringLiteral("aaa.txt"),
        "the tab is still showing the member of the file that was stepped off");
}

void TestPreview::withNoArchiveBackendTheTabBehavesAsItDidBefore()
{
    // Deliberately without withArchiveBackend(): nothing here may make the
    // preview tab depend on a plugin. A build with no archive backend has no
    // factory that claims a .gz, and the feature is then quietly absent rather
    // than broken.
    QVERIFY(m_tree->writeFile(QStringLiteral("alone.txt"), QByteArray("wrapped up\n")));
    if (!gzipInPlace(QStringLiteral("alone.txt")))
        QSKIP("gzip is not available");

    PreviewTabController* preview = openPreview(QStringLiteral("alone.txt.gz"));
    QVERIFY(preview);
    QCOMPARE(preview->title(), QStringLiteral("alone.txt.gz"));
    QCOMPARE(internalMounts(), 0);
    QVERIFY2(!preview->viewerName().isEmpty(), "the tab still chose a viewer the way it always did");
}

// ---- video ---------------------------------------------------------------
//
// Video was the last family of file with no viewer at all: a .mp4 reached the
// information viewer and got nine facts out of stat(). The provider joins the
// others rather than changing anything, as an optional dependency in the shape
// Qt PDF already has. See MOLE-37.

void TestPreview::aVideoIsClaimedByTheVideoViewer_data()
{
    QTest::addColumn<QString>("fileName");

    // The containers anybody previews video in. Claimed by name, from the system's
    // own MIME database -- see VideoPreviewProvider::videoSuffixes() for why Qt's
    // list of decodable formats cannot be the oracle here.
    QTest::newRow("mp4") << "holiday.mp4";
    QTest::newRow("mkv") << "recording.mkv";
    QTest::newRow("webm") << "clip.webm";
    QTest::newRow("mov") << "camera.mov";
}

void TestPreview::aVideoIsClaimedByTheVideoViewer()
{
    QFETCH(QString, fileName);

    if (!VideoPreviewProvider::isAvailable())
        QSKIP("this build has no video decoder, which is the other half of this ticket");

    IPreviewProvider* provider = providerFor(fileName);
    QVERIFY2(provider, qPrintable(QStringLiteral("nothing claimed %1").arg(fileName)));
    QCOMPARE(provider->id(), QStringLiteral("mole.preview.video"));
    QCOMPARE(provider->displayName(), QStringLiteral("Video"));
}

void TestPreview::aSuffixNoFormatKnowsFallsThroughToTheInformationViewer()
{
    // The rule the whole optional dependency rests on: what is not claimed falls
    // to a viewer that can say something useful. A `.mp5` is a name nothing knows,
    // which is what a container this installation cannot open looks like from
    // here.
    QVERIFY(!VideoPreviewProvider::videoSuffixes().contains(QStringLiteral("mp5")));

    QVERIFY(m_tree->writeFile(QStringLiteral("mystery.mp5"), QByteArray(64, '\x01')));
    PreviewTabController* preview = openPreview(QStringLiteral("mystery.mp5"));
    QVERIFY(preview);
    QVERIFY2(preview->viewerName() != QStringLiteral("Video"),
        "a suffix no format knows was claimed by the video viewer");
    QVERIFY2(!preview->viewerName().isEmpty(), "and something still shows it");
}

void TestPreview::aVideoOnADriveThatCannotBePlayedFromIsCopiedLocally()
{
    if (!VideoPreviewProvider::isAvailable())
        QSKIP("this build has no video decoder");

    // MediaPlayer cannot open an `archive://` uri or a remote one, so the viewer
    // asks for a local copy the way the image and document viewers do. A memory
    // drive is the cheapest thing that is not a local disk.
    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/clip.mp4"), QByteArray(2048, 'v'));

    Mount mount;
    mount.id = QStringLiteral("elsewhere");
    mount.displayName = QStringLiteral("elsewhere");
    mount.root = VfsUri::fromString(QStringLiteral("mem://elsewhere/"));
    mount.fileSystem = memory;
    m_app->services().vfs->addMount(mount);

    m_app->previewFile(QStringLiteral("mem://elsewhere/clip.mp4"));
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    QVERIFY(preview);
    QVERIFY(waitFor([preview] { return preview->viewer() != nullptr; }, 5000));
    QCOMPARE(preview->viewerName(), QStringLiteral("Video"));

    QObject* viewer = preview->viewer();
    QVERIFY(viewer);
    QVERIFY2(waitFor([viewer] { return !viewer->property("source").toString().isEmpty(); }, 30000),
        "the viewer never got a local copy to play");
    const QString source = viewer->property("source").toString();
    QVERIFY2(source.startsWith(QStringLiteral("file:")), qPrintable(source));
    QVERIFY(QFile::exists(QUrl(source).toLocalFile()));
}

void TestPreview::aVideoThatCannotBeDecodedFallsBackToWhatIsKnownAboutIt()
{
    if (!VideoPreviewProvider::isAvailable())
        QSKIP("this build has no video decoder");

    // A container this build can demux may still hold a stream it has no decoder
    // for, and nothing can know before trying -- so this viewer is the plain case
    // for declining after reading. What it had before there was anywhere to step
    // down to was a sentence in a black frame; what it has now is the list of
    // facts, which has the duration and the codec in it. The view calls this when
    // the player errors; tst_Walkthrough asserts the player really does.
    QVERIFY(m_tree->writeFile(QStringLiteral("broken.mp4"), QByteArray("not a container at all")));
    PreviewTabController* preview = openPreview(QStringLiteral("broken.mp4"));
    QVERIFY(preview);
    QCOMPARE(preview->viewerName(), QStringLiteral("Video"));
    QVERIFY(preview->fallbackNote().isEmpty());

    QObject* viewer = preview->viewer();
    QVERIFY(viewer);
    QVERIFY(QMetaObject::invokeMethod(
        viewer, "reportPlaybackFailure", Q_ARG(QString, QStringLiteral("no h265 decoder installed"))));

    // Queued, because the refusal arrives from inside the player: the viewer that
    // is about to be deleted is the one whose call is still running.
    QVERIFY(waitFor(
        [preview] { return preview->providerId() == QStringLiteral("mole.preview.fileinfo"); }, 5000));
    QCOMPARE(preview->viewerName(), QStringLiteral("File information"));

    // Which viewer gave up, and why. Both, because the reader is looking at a
    // list of facts where they asked for a video and neither half explains that
    // on its own.
    QVERIFY2(preview->fallbackNote().contains(QStringLiteral("Video")), qPrintable(preview->fallbackNote()));
    QVERIFY2(preview->fallbackNote().contains(QStringLiteral("no h265 decoder")),
        qPrintable(preview->fallbackNote()));
}

void TestPreview::aRefusalWithNothingToSayStillSaysWhichViewerGaveUp()
{
    if (!VideoPreviewProvider::isAvailable())
        QSKIP("this build has no video decoder");

    // Qt's player does not always have an error string. A note that said only
    // "could not show this file: " would be worse than none, and a note that said
    // nothing at all would leave the viewer having changed under the reader for no
    // stated cause.
    QVERIFY(m_tree->writeFile(QStringLiteral("silent.mp4"), QByteArray("not a container at all")));
    PreviewTabController* preview = openPreview(QStringLiteral("silent.mp4"));
    QVERIFY(preview);

    QObject* viewer = preview->viewer();
    QVERIFY(viewer);
    QVERIFY(QMetaObject::invokeMethod(viewer, "reportPlaybackFailure", Q_ARG(QString, QString())));

    QVERIFY(waitFor(
        [preview] { return preview->providerId() == QStringLiteral("mole.preview.fileinfo"); }, 5000));
    QVERIFY2(preview->fallbackNote().contains(QStringLiteral("Video")), qPrintable(preview->fallbackNote()));
    QVERIFY2(!preview->fallbackNote().endsWith(QStringLiteral(": ")), qPrintable(preview->fallbackNote()));
}

void TestPreview::anImageThisBuildCannotDecodeFallsBackToTheFacts()
{
    // A `.png` that is not a PNG. The suffixes this viewer claims are the ones Qt
    // says it has plugins for, so the name gets it this far and only the decode
    // can tell -- the same shape as the video, from a different direction.
    QVERIFY(m_tree->writeFile(QStringLiteral("not-really.png"), QByteArray("this is not a PNG at all")));
    PreviewTabController* preview = openPreview(QStringLiteral("not-really.png"));
    QVERIFY(preview);
    QCOMPARE(preview->viewerName(), QStringLiteral("Image"));

    QObject* viewer = preview->viewer();
    QVERIFY(viewer);
    // The local copy has to be in place first: an Image with no source reports
    // Error too, and giving the file up in that gap would mean never showing one.
    QVERIFY(waitFor([viewer] { return !viewer->property("source").toString().isEmpty(); }, 5000));
    QVERIFY(QMetaObject::invokeMethod(viewer, "reportDecodeFailure"));

    QVERIFY(waitFor(
        [preview] { return preview->providerId() == QStringLiteral("mole.preview.fileinfo"); }, 5000));
    QVERIFY2(preview->fallbackNote().contains(QStringLiteral("Image")), qPrintable(preview->fallbackNote()));
}

void TestPreview::aViewerWithNoSourceYetDoesNotGiveTheFileUp()
{
    // The gap above, held open on purpose: a QML Image reports Error for an empty
    // source as well, which is what it has between the file being opened and the
    // local copy arriving. A decline in that gap would take every image in the
    // application down to the list of facts.
    //
    // The controller directly rather than through a tab, because a copy of a file
    // on local disk arrives in the same turn -- there is no gap to catch there,
    // and one on a remote drive would be a test about the network.
    ImagePreviewController viewer(m_app->services());
    QSignalSpy gaveUp(&viewer, &PreviewController::declined);

    viewer.reportDecodeFailure();
    QCOMPARE(gaveUp.count(), 0);
    QVERIFY(viewer.errorText().isEmpty());
}

void TestPreview::aDeclineStepsOneRungDownRatherThanToTheBottom()
{
    // The ladder is the registry's own order, and "below" is one place in it
    // rather than a whole tier: the database, Parquet and video viewers all sit
    // at priority 60, and a decline that jumped to the next lower number would
    // skip a viewer that might have shown the file.
    //
    // Asked of the registry directly, because building a file that three viewers
    // in turn accept and refuse would be a fixture about nothing.
    FileEntry text;
    text.name = QStringLiteral("notes.txt");
    text.uri = m_tree->rootUri().child(QStringLiteral("notes.txt"));
    text.mimeType = QStringLiteral("text/plain");

    IPreviewProvider* first = m_app->previews()->providerFor(text);
    QVERIFY(first);
    QCOMPARE(first->id(), QStringLiteral("mole.preview.text"));

    // Hex claims only what the content pass could make nothing of, so a plain
    // text file steps past it to the facts rather than to hexadecimal.
    IPreviewProvider* second = m_app->previews()->providerBelow(text, first);
    QVERIFY(second);
    QCOMPARE(second->id(), QStringLiteral("mole.preview.fileinfo"));

    // And the bottom of the ladder has nothing under it, which is what stops a
    // decline walking for ever.
    QVERIFY(m_app->previews()->providerBelow(text, second) == nullptr);

    // A provider that is not on the ladder has no position to step down from.
    TextPreviewProvider stranger(m_app->services());
    QVERIFY(m_app->previews()->providerBelow(text, &stranger) == nullptr);

    // A directory is not previewable at all, in either direction.
    FileEntry folder;
    folder.name = QStringLiteral("subfolder");
    folder.uri = m_tree->rootUri().child(QStringLiteral("subfolder"));
    folder.isDir = true;
    QVERIFY(m_app->previews()->providerBelow(folder, nullptr) == nullptr);
}

void TestPreview::theSoundIsOneSettingForEveryVideo()
{
    if (!VideoPreviewProvider::isAvailable())
        QSKIP("this build has no video decoder");

    // Nothing here has to be a decodable video: the viewer is claimed by suffix and
    // the answer to "is the sound off" is read before anybody finds out what is in
    // the container. So this costs no ffmpeg and runs on every machine.
    QVERIFY(m_tree->writeFile(QStringLiteral("first.mp4"), QByteArray(2048, 'v')));
    QVERIFY(m_tree->writeFile(QStringLiteral("second.mp4"), QByteArray(2048, 'v')));

    PreviewTabController* first = openPreview(QStringLiteral("first.mp4"));
    QVERIFY(first);
    auto* sound = qobject_cast<VideoPreviewController*>(first->viewer());
    QVERIFY2(sound, "the video viewer is what holds the answer");
    QVERIFY2(!sound->isMuted(), "a video preview has its sound on until somebody says otherwise");

    sound->setMuted(true);
    QVERIFY(sound->isMuted());

    // The next video opens the way the last one was left, which is the whole point.
    // One key for every video rather than one per suffix: whether the room is quiet
    // is a fact about the person, not about a container format.
    auto* next = qobject_cast<VideoPreviewController*>(openPreview(QStringLiteral("second.mp4"))->viewer());
    QVERIFY(next);
    QVERIFY2(next->isMuted(), "muting one video did not carry to the next one");

    // And it survives a restart, because it is a preference rather than a mood --
    // the same claim theDetailsAreOneSettingForEveryFile makes, checked the same way.
    m_app->saveSessionNow();
    m_app.reset();
    drainEvents();

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(m_tree->rootUri().toString()));
    QString error;
    QVERIFY2(m_app->initialise(std::move(builtIns), &error), qPrintable(error));

    auto* afterRestart
        = qobject_cast<VideoPreviewController*>(openPreview(QStringLiteral("first.mp4"))->viewer());
    QVERIFY(afterRestart);
    QVERIFY2(afterRestart->isMuted(), "a remembered setting that does not survive a restart is a mood");

    // And back on again, for everything: a setting that can only be turned one way
    // is a trap rather than a setting.
    afterRestart->setMuted(false);
    auto* last = qobject_cast<VideoPreviewController*>(openPreview(QStringLiteral("second.mp4"))->viewer());
    QVERIFY(last);
    QVERIFY(!last->isMuted());
}

// ------------------------------------------- an earlier state of the file

/// Where a preview starts, and it says so. The label is not conditional on there
/// being anything else to show: a preview showing an earlier version while
/// looking like the file itself is the one failure this subject has to avoid.
void TestPreview::aPreviewShowsTheFileAsItIsAndSaysSo()
{
    PreviewTabController* preview = previewOnOfferingDrive(QStringLiteral("report.txt"));
    QVERIFY(preview);

    QVERIFY(preview->showingVersion().isEmpty());
    auto* text = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(text);
    QVERIFY(waitFor([text] { return text->text().contains(QStringLiteral("third")); }));
}

void TestPreview::aDriveWithNothingOlderOffersNoChoiceAtAll()
{
    PreviewTabController* preview = previewOnOfferingDrive(QStringLiteral("report.txt"));
    QVERIFY(preview);

    preview->open(QStringLiteral("mem://offering/plain.txt"));
    waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); });
    drainEvents();
    QVERIFY2(!preview->hasOtherVersions(), "a file with nothing older must offer no chooser");
    QVERIFY(preview->otherVersions().isEmpty());
}

/// Knowing there is something else costs one listing call; fetching the list is
/// a call into storage, and opening a preview must not make one nobody asked for.
void TestPreview::theVersionsAreNotFetchedUntilSomebodyAsksForThem()
{
    PreviewTabController* preview = previewOnOfferingDrive(QStringLiteral("report.txt"));
    QVERIFY(preview);

    QVERIFY(preview->hasOtherVersions());
    QCOMPARE(m_offering->invokeCallCount(), 0);
    QVERIFY(preview->otherVersions().isEmpty());

    preview->requestVersions();
    QVERIFY(waitFor([preview] { return preview->otherVersions().size() == 2; }));
    QCOMPARE(m_offering->invokeCallCount(), 1);

    // Asking twice does not ask the drive twice.
    preview->requestVersions();
    drainEvents();
    QCOMPARE(m_offering->invokeCallCount(), 1);
}

void TestPreview::movingToAnEarlierVersionShowsThatVersionsContents()
{
    PreviewTabController* preview = previewOnOfferingDrive(QStringLiteral("report.txt"));
    QVERIFY(preview);
    preview->requestVersions();
    QVERIFY(waitFor([preview] { return preview->otherVersions().size() == 2; }));

    const QString first = preview->otherVersions().first().toMap().value(QStringLiteral("uri")).toString();
    preview->showVersion(first);
    waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); });

    auto* text = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY2(text, "an earlier version is an ordinary file and reaches the same viewer");
    QVERIFY(waitFor([text] { return text->text().contains(QStringLiteral("first")); }));
    QVERIFY2(!text->text().contains(QStringLiteral("third")),
        "the current file's contents shown as an earlier version is the failure this avoids");
}

void TestPreview::theScreenSaysWhichVersionIsOnThroughout()
{
    PreviewTabController* preview = previewOnOfferingDrive(QStringLiteral("report.txt"));
    QVERIFY(preview);
    QCOMPARE(preview->showingVersion(), QString());

    preview->requestVersions();
    QVERIFY(waitFor([preview] { return preview->otherVersions().size() == 2; }));

    const QVariantList versions = preview->otherVersions();
    for (const QVariant& entry : versions) {
        const QVariantMap version = entry.toMap();
        preview->showVersion(version.value(QStringLiteral("uri")).toString());
        QCOMPARE(preview->showingVersion(), version.value(QStringLiteral("label")).toString());
    }
}

/// The tab goes on being about the file it is about, so there is a way back and
/// the arrows still step through the folder.
void TestPreview::goingBackToTheCurrentFileWorks()
{
    PreviewTabController* preview = previewOnOfferingDrive(QStringLiteral("report.txt"));
    QVERIFY(preview);
    preview->requestVersions();
    QVERIFY(waitFor([preview] { return preview->otherVersions().size() == 2; }));

    const QString earlier = preview->otherVersions().first().toMap().value(QStringLiteral("uri")).toString();
    preview->showVersion(earlier);
    QCOMPARE(preview->showingVersion(), QStringLiteral("v1"));
    QCOMPARE(preview->currentUri(), QStringLiteral("mem://offering/report.txt"));

    preview->showVersion(QString());
    waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); });
    QVERIFY(preview->showingVersion().isEmpty());

    auto* text = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(text);
    QVERIFY(waitFor([text] { return text->text().contains(QStringLiteral("third")); }));
}

/// Every viewer works on one, because it is a uri and nothing below the preview
/// learns anything new. A table is the one worth saying it about: it reads with
/// its own parser through the same drive.
void TestPreview::everyViewerWorksOnAnEarlierVersion()
{
    m_offering = std::make_shared<OfferingFileSystem>();
    m_offering->memory()->addFile(QStringLiteral("/prices.csv"), QByteArray("name,price\nnow,3\n"));
    m_offering->addVersion(
        QStringLiteral("/prices.csv"), QStringLiteral("v1"), QByteArray("name,price\nthen,1\n"));

    Mount mount;
    mount.id = QStringLiteral("offering");
    mount.displayName = QStringLiteral("offering");
    mount.root = VfsUri::fromString(QStringLiteral("mem://offering/"));
    mount.fileSystem = m_offering;
    QVERIFY(!m_app->services().vfs->addMount(mount).isEmpty());

    m_app->previewFile(QStringLiteral("mem://offering/prices.csv"));
    auto* preview = qobject_cast<PreviewTabController*>(m_app->tabs()->currentController());
    QVERIFY(preview);
    waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); });
    const QString viewerNow = preview->viewerName();

    QVERIFY(waitFor([preview] { return preview->hasOtherVersions(); }));
    preview->requestVersions();
    QVERIFY(waitFor([preview] { return preview->otherVersions().size() == 1; }));

    preview->showVersion(preview->otherVersions().first().toMap().value(QStringLiteral("uri")).toString());
    waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); });

    QCOMPARE(preview->viewerName(), viewerNow);
    QVERIFY2(preview->viewer(), "the same viewer has to build for an earlier version");
}

/// A remote drive must not download a whole earlier version to show the first
/// page of it, any more than it does for the current one.
void TestPreview::anEarlierVersionIsReadInPartsLikeAnyOtherFile()
{
    PreviewTabController* preview = previewOnOfferingDrive(QStringLiteral("report.txt"));
    QVERIFY(preview);

    preview->open(QStringLiteral("mem://offering/big.txt"));
    waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); });
    QVERIFY(waitFor([preview] { return preview->hasOtherVersions(); }));
    preview->requestVersions();
    QVERIFY(waitFor([preview] { return preview->otherVersions().size() == 1; }));

    preview->showVersion(preview->otherVersions().first().toMap().value(QStringLiteral("uri")).toString());
    waitFor([preview] { return preview->viewer() != nullptr || !preview->isIdentifying(); });

    auto* text = qobject_cast<TextPreviewController*>(preview->viewer());
    QVERIFY(text);
    QVERIFY(waitFor([text] { return text->fileSize() > 0; }));
    QCOMPARE(text->fileSize(), qint64(4 * 1024 * 1024));
    QVERIFY2(text->isPaged(), "four megabytes must be shown a window at a time");
    QVERIFY2(
        text->windowBytes() < text->fileSize(), "a page of an earlier version must not cost the whole of it");
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
