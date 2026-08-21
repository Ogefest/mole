#include "host/ThumbnailRegistry.h"
#include "plugins/builtin/previews/VideoPreview.h"
#include "plugins/builtin/thumbnails/PdfThumbnailer.h"
#include "plugins/builtin/thumbnails/VideoThumbnailer.h"
#include "support/TestSupport.h"
#include "support/VideoFixtures.h"
#include "ui/ThumbnailSource.h"

#include "core/CoreMetaTypes.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// A folder of PDFs is a wall of identical icons, and so is a folder of videos.
/// Both are thumbnailable and both already have a viewer in this application that
/// proves how. See MOLE-144.
class TestMediaThumbnailers : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aPdfShowsItsFirstPage();
    void aDamagedPdfIsAnOrdinaryAnswer();
    void aPdfOnARemoteDriveOverTheCeilingIsLeftAlone();
    void aVideoShowsAFrameThatIsNotTheFirst();
    void aLongVideoIsSeekedRatherThanPlayedTo();
    void aVideoThatCannotBeDecodedInTimeYieldsNothing();
    void aVideoOnADriveThatIsNotLocalIsNotClaimed();
    void aVideoIsClaimedOnlyByABuildThatCanDecodeOne();
    void bothAreAbsentFromABuildWithoutTheirDependency();
    void anEmptyCodecListIsNotTheSameAsNoMultimedia();
    void theDiagnosticsSayWhichOfTheTwoHappened();
    void neitherRunsOnTheGuiThreadAndBothStopWhenTold();

private:
    /// A one-page PDF written by hand: a page of a known shape, and no library
    /// needed to make one.
    static QByteArray onePagePdf(int widthPoints, int heightPoints);
    FileEntry localEntry(const QString& relativePath) const;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<VfsManager> m_vfs;
    PluginServices m_services;
};

void TestMediaThumbnailers::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());

    m_vfs = std::make_unique<VfsManager>();
    Mount mount;
    mount.displayName = QStringLiteral("disk");
    mount.root = VfsUri::fromLocalPath(m_dir->path());
    mount.fileSystem = std::make_shared<LocalFileSystem>();
    QVERIFY(!m_vfs->addMount(mount).isEmpty());

    m_services = PluginServices {};
    m_services.vfs = m_vfs.get();
}

void TestMediaThumbnailers::cleanup()
{
    m_vfs.reset();
    m_dir.reset();
}

FileEntry TestMediaThumbnailers::localEntry(const QString& relativePath) const
{
    const QString path = QDir(m_dir->path()).filePath(relativePath);
    FileEntry entry;
    entry.uri = VfsUri::fromLocalPath(path);
    entry.name = QFileInfo(path).fileName();
    entry.size = QFileInfo(path).size();
    return entry;
}

QByteArray TestMediaThumbnailers::onePagePdf(int widthPoints, int heightPoints)
{
    // The smallest thing that is really a PDF: a catalogue, a page tree, one page
    // with a content stream that paints a rectangle, and a cross-reference table
    // whose offsets are computed rather than guessed.
    const QByteArray content = QByteArray("0.1 0.4 0.8 rg\n20 20 ") + QByteArray::number(widthPoints - 40)
        + " " + QByteArray::number(heightPoints - 40) + " re f\n";

    QList<QByteArray> objects;
    objects.append(QByteArrayLiteral("<< /Type /Catalog /Pages 2 0 R >>"));
    objects.append(QByteArrayLiteral("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"));
    objects.append(QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ")
        + QByteArray::number(widthPoints) + " " + QByteArray::number(heightPoints) + "] /Contents 4 0 R >>");
    objects.append(QByteArray("<< /Length ") + QByteArray::number(content.size()) + " >>\nstream\n" + content
        + "endstream");

    QByteArray out = QByteArrayLiteral("%PDF-1.4\n");
    QList<qsizetype> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(out.size());
        out += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
    }
    const qsizetype xrefAt = out.size();
    out += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n";
    out += "0000000000 65535 f \n";
    for (const qsizetype offset : offsets)
        out += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    out += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n"
        + QByteArray::number(xrefAt) + "\n%%EOF\n";
    return out;
}

void TestMediaThumbnailers::aPdfShowsItsFirstPage()
{
#ifndef MOLE_HAVE_QTPDF
    QSKIP("this build has no Qt Pdf, so a PDF gets the icon tile");
#else
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("report.pdf"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray pdf = onePagePdf(400, 600); // portrait, like most documents
        QCOMPARE(file.write(pdf), qint64(pdf.size()));
    }

    PdfThumbnailer thumbnailer;
    const FileEntry entry = localEntry(QStringLiteral("report.pdf"));
    QVERIFY(thumbnailer.canThumbnail(entry));

    const QImage tile = thumbnailer.thumbnail(entry, 160, m_services, CancelToken {});
    QVERIFY2(!tile.isNull(), "a folder of PDFs has to be more than identical icons");
    QCOMPARE(qMax(tile.width(), tile.height()), 160);
    // The page's own shape, not a square: a portrait document that came back
    // square would make every tile in a folder of reports look the same again.
    QVERIFY2(tile.height() > tile.width(),
        qPrintable(QStringLiteral("came back %1x%2").arg(tile.width()).arg(tile.height())));
#endif
}

void TestMediaThumbnailers::aDamagedPdfIsAnOrdinaryAnswer()
{
#ifndef MOLE_HAVE_QTPDF
    QSKIP("this build has no Qt Pdf");
#else
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("torn.pdf"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // A header and nothing behind it, which is what half a download looks like.
        QVERIFY(file.write(onePagePdf(400, 600).left(60)) > 0);
    }

    PdfThumbnailer thumbnailer;
    QVERIFY2(thumbnailer.thumbnail(localEntry(QStringLiteral("torn.pdf")), 160, m_services, {}).isNull(),
        "an unreadable document keeps its icon and says nothing");
#endif
}

void TestMediaThumbnailers::aPdfOnARemoteDriveOverTheCeilingIsLeftAlone()
{
#ifndef MOLE_HAVE_QTPDF
    QSKIP("this build has no Qt Pdf");
#else
    // There is nothing small inside a PDF to read instead -- rendering page 0
    // needs the file -- so the ceiling is the only guard there is.
    FileEntry entry;
    entry.uri = VfsUri::fromString(QStringLiteral("mem:///books/scan.pdf"));
    entry.name = QStringLiteral("scan.pdf");
    entry.size = PdfThumbnailer::kRemoteCeiling + 1;

    PdfThumbnailer thumbnailer;
    QVERIFY(thumbnailer.canThumbnail(entry));
    QVERIFY2(thumbnailer.thumbnail(entry, 160, m_services, CancelToken {}).isNull(),
        "a 300 MB scanned book on a bucket is not worth a tile");
#endif
}

void TestMediaThumbnailers::aVideoShowsAFrameThatIsNotTheFirst()
{
#ifndef MOLE_HAVE_MULTIMEDIA
    QSKIP("this build has no Qt Multimedia, so a video gets the icon tile");
#else
    if (!VideoThumbnailer::isAvailable())
        QSKIP("this build can decode no video at all");
    if (!fixtures::videoEncoderAvailable())
        QSKIP("no encoder to make a video fixture with");

    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("clip.mp4"));
    QVERIFY2(fixtures::writeVideoWithBlackOpening(path), "the fixture is a black second then colour");

    VideoThumbnailer thumbnailer;
    const FileEntry entry = localEntry(QStringLiteral("clip.mp4"));
    QVERIFY(thumbnailer.canThumbnail(entry));

    const QImage tile = thumbnailer.thumbnail(entry, 120, m_services, CancelToken {});
    QVERIFY2(!tile.isNull(), "a folder of videos has to be more than identical icons");
    QCOMPARE(qMax(tile.width(), tile.height()), 120);

    // The frame is from after the opening, which is the whole claim: a folder of
    // black tiles is less useful than a folder of icons.
    const QColor middle = tile.pixelColor(tile.width() / 2, tile.height() / 2);
    QVERIFY2(middle.red() > 80 || middle.blue() > 80,
        qPrintable(QStringLiteral("the frame came back %1, which is the black opening").arg(middle.name())));
#endif
}

/// The case a five-second fixture cannot make: a video long enough that seeking to
/// the frame and playing to it are different outcomes rather than different speeds.
///
/// A tenth of the way into a minute is six seconds, and the thumbnailer gives
/// itself five. So a build that seeks answers at once, and a build that cannot seek
/// -- and therefore plays the opening at 1x waiting to arrive -- runs out of time
/// and produces nothing. That is exactly what a folder of real videos showed:
/// tiles for the short ones, icons for everything over about a minute.
void TestMediaThumbnailers::aLongVideoIsSeekedRatherThanPlayedTo()
{
#ifndef MOLE_HAVE_MULTIMEDIA
    QSKIP("this build has no Qt Multimedia, so a video gets the icon tile");
#else
    if (!VideoThumbnailer::isAvailable())
        QSKIP("this build can decode no video at all");
    if (!fixtures::videoEncoderAvailable())
        QSKIP("no encoder to make a video fixture with");

    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("lecture.mp4"));
    QVERIFY2(fixtures::writeMinuteLongVideoWithBlackOpening(path),
        "the fixture is a minute long, opening on black");

    VideoThumbnailer thumbnailer;
    const FileEntry entry = localEntry(QStringLiteral("lecture.mp4"));
    QVERIFY(thumbnailer.canThumbnail(entry));

    const QImage tile = thumbnailer.thumbnail(entry, 120, m_services, CancelToken {});
    QVERIFY2(!tile.isNull(),
        "a video over a minute has to have a tile too -- playing to the frame instead of "
        "seeking to it cannot reach one inside the time limit");

    // And it is the frame that was asked for, not whatever the opening happened to
    // hold: six seconds in is well past the black.
    const QColor middle = tile.pixelColor(tile.width() / 2, tile.height() / 2);
    QVERIFY2(middle.red() > 80 || middle.blue() > 80,
        qPrintable(QStringLiteral("the frame came back %1, which is the black opening").arg(middle.name())));
#endif
}

void TestMediaThumbnailers::aVideoThatCannotBeDecodedInTimeYieldsNothing()
{
#ifndef MOLE_HAVE_MULTIMEDIA
    QSKIP("this build has no Qt Multimedia");
#else
    if (!VideoThumbnailer::isAvailable())
        QSKIP("this build can decode no video at all");

    // Bytes with a video name and nothing a decoder can use. The limit is what
    // stops this being an unbounded wait, and it is asserted rather than described.
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("broken.mp4"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArray(200000, '\x11')) > 0);
    }

    VideoThumbnailer thumbnailer;
    QElapsedTimer clock;
    clock.start();
    const QImage tile = thumbnailer.thumbnail(localEntry(QStringLiteral("broken.mp4")), 120, m_services, {});
    const qint64 took = clock.elapsed();

    QVERIFY2(tile.isNull(), "the tile stays an icon");
    QVERIFY2(took < VideoThumbnailer::kTimeLimitMs * 2,
        qPrintable(QStringLiteral("gave up after %1 ms against a limit of %2")
                       .arg(took)
                       .arg(VideoThumbnailer::kTimeLimitMs)));
#endif
}

void TestMediaThumbnailers::aVideoOnADriveThatIsNotLocalIsNotClaimed()
{
#ifndef MOLE_HAVE_MULTIMEDIA
    QSKIP("this build has no Qt Multimedia");
#else
    if (!VideoThumbnailer::isAvailable())
        QSKIP("this build can decode no video at all");

    FileEntry entry;
    entry.uri = VfsUri::fromString(QStringLiteral("mem:///media/film.mp4"));
    entry.name = QStringLiteral("film.mp4");
    entry.size = 900000;

    VideoThumbnailer thumbnailer;
    QVERIFY2(!thumbnailer.canThumbnail(entry),
        "decoding a frame means seeking, and a drive that answers a seek by downloading is the case "
        "MOLE-143 exists to avoid");
#endif
}

/// The other half of "present only when its dependency is": a build without one
/// compiles, is green, and shows the icon tile.
/// The other half of deferring the codec question: a suffix that looks like a
/// video is not on its own a promise of a tile.
///
/// canThumbnail() checks the name against the MIME database first, because that
/// costs nothing, and asks Qt Multimedia second, because asking it anything builds
/// the whole GStreamer stack. Cheap-first is only correct if the second half is
/// still asked -- otherwise a build with the module and no decoders would claim
/// every `.mp4` and hand back nothing, which is a folder of blank tiles where it
/// used to be a folder of icons. Deliberately not skipped when there is no
/// decoder: that is the case being held.
void TestMediaThumbnailers::aVideoIsClaimedOnlyByABuildThatCanDecodeOne()
{
#ifndef MOLE_HAVE_MULTIMEDIA
    QSKIP("this build has no Qt Multimedia, so a video gets the icon tile");
#else
    QVERIFY(m_dir);
    QVERIFY(QFile(QDir(m_dir->path()).filePath(QStringLiteral("holiday.mp4"))).open(QIODevice::WriteOnly));

    const VideoThumbnailer thumbnailer;
    QCOMPARE(
        thumbnailer.canThumbnail(localEntry(QStringLiteral("holiday.mp4"))), VideoThumbnailer::isAvailable());

    // And a name no video format goes by is refused either way, without the
    // decoder being consulted at all.
    QVERIFY(!thumbnailer.canThumbnail(localEntry(QStringLiteral("holiday.mp5"))));
#endif
}

void TestMediaThumbnailers::bothAreAbsentFromABuildWithoutTheirDependency()
{
#if defined(MOLE_HAVE_QTPDF) || defined(MOLE_HAVE_MULTIMEDIA)
    // This build has at least one of them, so what is held here is the compile
    // itself: the guards above are what a build without either exercises, and this
    // suite is compiled in both.
    QVERIFY(true);
#else
    QVERIFY2(true, "nothing to thumbnail with, and nothing broken by that");
#endif
}

/// The house rule, for the two thumbnails that can take real time. The video one
/// runs an event loop of its own, and where that loop runs is the thing worth
/// asserting rather than describing.
void TestMediaThumbnailers::neitherRunsOnTheGuiThreadAndBothStopWhenTold()
{
    // Cancellation first, in the plainest form: a token already set.
    CancelToken cancelled;
    cancelled.cancel();
#ifdef MOLE_HAVE_QTPDF
    {
        const QString path = QDir(m_dir->path()).filePath(QStringLiteral("late.pdf"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray pdf = onePagePdf(300, 400);
        QCOMPARE(file.write(pdf), qint64(pdf.size()));
        file.close();
        PdfThumbnailer thumbnailer;
        QVERIFY(thumbnailer.thumbnail(localEntry(QStringLiteral("late.pdf")), 120, m_services, cancelled)
                    .isNull());
    }
#endif
#ifdef MOLE_HAVE_MULTIMEDIA
    if (VideoThumbnailer::isAvailable() && fixtures::videoEncoderAvailable()) {
        const QString path = QDir(m_dir->path()).filePath(QStringLiteral("late.mp4"));
        QVERIFY(fixtures::writeVideoWithBlackOpening(path));
        VideoThumbnailer thumbnailer;
        QVERIFY(thumbnailer.thumbnail(localEntry(QStringLiteral("late.mp4")), 120, m_services, cancelled)
                    .isNull());
    }
#endif

#ifdef MOLE_HAVE_QTPDF
    // And through the task the application really uses, which is where the claim
    // about the thread can be made at all.
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("report.pdf"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray pdf = onePagePdf(300, 400);
        QCOMPARE(file.write(pdf), qint64(pdf.size()));
    }

    ThumbnailRegistry registry;
    QVERIFY(registry.addThumbnailer(std::make_unique<PdfThumbnailer>()));
    QTemporaryDir indexDir;
    QVERIFY(indexDir.isValid());
    IndexDatabase index(QDir(indexDir.path()).filePath(QStringLiteral("i.sqlite")));
    QVERIFY(index.open().ok());
    TaskManager tasks;

    PluginServices services = m_services;
    services.tasks = &tasks;
    services.index = &index;
    services.thumbnails = &registry;

    ThumbnailKey key;
    key.uri = VfsUri::fromLocalPath(path);
    key.size = 120;
    key.bytes = QFileInfo(path).size();
    auto* task = new ThumbnailTask(services, key, nullptr);
    tasks.submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->answeredBy(), QStringLiteral("mole.thumb.pdf"));
    QVERIFY(!task->image().isNull());
    QVERIFY2(task->ranOn() != QThread::currentThread(),
        "rendering a page on the GUI thread is a window that has stopped answering");
#endif

#ifdef MOLE_HAVE_MULTIMEDIA
    // The same for the video, and this is the one that has to be proved rather
    // than assumed: it runs an event loop of its own, and a pool thread has none
    // until that loop starts. If this ever stops working, a folder of videos goes
    // back to icons in silence.
    if (!VideoThumbnailer::isAvailable() || !fixtures::videoEncoderAvailable())
        return;

    const QString clip = QDir(m_dir->path()).filePath(QStringLiteral("clip.mp4"));
    QVERIFY(fixtures::writeVideoWithBlackOpening(clip));

    ThumbnailRegistry videoRegistry;
    QVERIFY(videoRegistry.addThumbnailer(std::make_unique<VideoThumbnailer>()));
    QTemporaryDir videoIndexDir;
    QVERIFY(videoIndexDir.isValid());
    IndexDatabase videoIndex(QDir(videoIndexDir.path()).filePath(QStringLiteral("i.sqlite")));
    QVERIFY(videoIndex.open().ok());
    TaskManager videoTasks;

    PluginServices videoServices = m_services;
    videoServices.tasks = &videoTasks;
    videoServices.index = &videoIndex;
    videoServices.thumbnails = &videoRegistry;

    ThumbnailKey videoKey;
    videoKey.uri = VfsUri::fromLocalPath(clip);
    videoKey.size = 120;
    videoKey.bytes = QFileInfo(clip).size();
    auto* videoTask = new ThumbnailTask(videoServices, videoKey, nullptr);
    videoTasks.submit(videoTask);
    QVERIFY(waitForTask(videoTask, 30000));

    QCOMPARE(videoTask->answeredBy(), QStringLiteral("mole.thumb.video"));
    QVERIFY2(!videoTask->image().isNull(), "a frame has to come back from a pool thread too");
    QVERIFY(videoTask->ranOn() != QThread::currentThread());
#endif
}

// A QGuiApplication rather than the usual guiless main: a QVideoSink reaches the
// platform layer, and a video decoded under a QCoreApplication crashes inside
// gstreamer rather than answering.
void TestMediaThumbnailers::anEmptyCodecListIsNotTheSameAsNoMultimedia()
{
    using Probe = VideoPreviewProvider::Probe;

    // No multimedia module: a real answer, and the only one that hides the
    // feature. There is nothing to try.
    Probe absent;
    absent.moduleBuiltIn = false;
    QVERIFY(!VideoPreviewProvider::videoIsWorthTrying(absent));

    // A module that is there and lists what it can decode: obviously yes.
    Probe speaking;
    speaking.moduleBuiltIn = true;
    speaking.decodableVideoCodecs = { QStringLiteral("H264"), QStringLiteral("VP8") };
    QVERIFY(VideoPreviewProvider::videoIsWorthTrying(speaking));

    // The case this exists for. A module that is present and reports nothing is
    // declining to say, not saying no -- QMediaFormat's codec list is the half
    // the GStreamer backend fills in, and another backend need not. Treating it
    // as no made videos stop existing in the application, with no error, no log
    // line and nothing greyed out, which looks like a decision rather than a
    // missing feature, so nobody investigates.
    //
    // Fed in rather than waited for: there is no way to arrange a silent backend
    // on the machine this suite runs on.
    Probe silent;
    silent.moduleBuiltIn = true;
    QVERIFY2(VideoPreviewProvider::videoIsWorthTrying(silent),
        "a backend that reports no codecs is declining to answer, not answering no");
}

void TestMediaThumbnailers::theDiagnosticsSayWhichOfTheTwoHappened()
{
    // The first question anybody will ask about a build that shows no video, and
    // it used to be unanswerable without a debugger.
    const QStringList lines = VideoPreviewProvider::diagnosticLines();
    QVERIFY(!lines.isEmpty());

    const QString all = lines.join(QLatin1Char('\n'));
    QVERIFY2(all.contains(QStringLiteral("multimedia module")), qPrintable(all));

    if (!VideoPreviewProvider::probe().moduleBuiltIn) {
        QVERIFY2(all.contains(QStringLiteral("not in this build")), qPrintable(all));
        return;
    }

    QVERIFY2(all.contains(QStringLiteral("media backend")), qPrintable(all));
    QVERIFY2(all.contains(QStringLiteral("video codecs it reports")), qPrintable(all));
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestMediaThumbnailers testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_MediaThumbnailers.moc"
