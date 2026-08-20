#include "host/ThumbnailRegistry.h"
#include "plugins/builtin/BrowserFeature.h"
#include "plugins/builtin/thumbnails/VideoThumbnailer.h"
#include "support/FakePlugin.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/ThumbnailSource.h"
#include "ui/models/BrowserPaneController.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The gallery and the thumbnail queue together, through the real view and the
/// real image provider.
///
/// A flick through eight hundred photographs asked for eight hundred decodes at
/// once, and the ones under the cursor when the scrolling stopped were the last to
/// arrive. That is what "asynchronous" gets you without scheduling. See MOLE-142.
class TestThumbnails : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void startingTheApplicationDoesNotBuildTheMediaStack();
    void scrollingAFolderNeverRunsMoreDecodesThanTheBound();
    void leavingAFolderTakesItsQueueWithIt();
    void aListingIsNeverBehindAQueueOfDecodes();

private:
    /// A folder of `count` files, all of them claimed by the held thumbnailer.
    bool fillGallery(int count);
    BrowserController* browser() const;
    /// The grid, once the gallery is showing and laid out.
    QQuickItem* tiles();

    std::unique_ptr<QmlAppHarness> m_harness;
    std::shared_ptr<FakeThumbnailer::Log> m_log;
    std::shared_ptr<QSemaphore> m_gate;
};

namespace {

/// The shared objects the loader has actually mapped into this process.
///
/// Empty when the platform cannot say, which the caller checks: a test that holds
/// "this library was never loaded" is worthless if the instrument silently answers
/// nothing. Linux only, and asking the loader is the point -- there is no Qt API
/// for "has Qt Multimedia been brought up yet", because bringing it up is what
/// asking it anything does.
QString modulesLoadedIntoThisProcess()
{
#ifdef Q_OS_LINUX
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromLatin1(maps.readAll());
#else
    return {};
#endif
}

} // namespace

void TestThumbnails::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start(QmlAppHarness::Options {}, &error), qPrintable(error));

    // A thumbnailer that answers for everything, counts, and can be held still --
    // above the built-in one, so what the view gets is countable.
    m_log = std::make_shared<FakeThumbnailer::Log>();
    m_gate = std::make_shared<QSemaphore>();
    auto held
        = std::make_unique<FakeThumbnailer>(QStringLiteral("test.counting"), QColor(Qt::green), 500, m_log);
    held->holdUntilReleased(m_gate);
    QVERIFY(m_harness->app()->thumbnails()->addThumbnailer(std::move(held)));
    QVERIFY(m_harness->thumbnails());
}

void TestThumbnails::cleanup()
{
    // Before the harness: a decode waiting on a gate is a blocked pool thread, and
    // teardown waits for it. See GateRelease and MOLE-217.
    if (m_gate)
        m_gate->release(4096);
    m_harness.reset();
    m_gate.reset();
    m_log.reset();
}

BrowserController* TestThumbnails::browser() const
{
    return qobject_cast<BrowserController*>(m_harness->app()->tabs()->currentController());
}

bool TestThumbnails::fillGallery(int count)
{
    if (!m_harness->makeDirs(QStringLiteral("many")))
        return false;
    for (int i = 0; i < count; ++i) {
        if (!m_harness->writeFile(
                QStringLiteral("many/shot-%1.jpg").arg(i, 4, 10, QLatin1Char('0')), QByteArray("bytes")))
            return false;
    }

    BrowserController* tab = browser();
    if (!tab)
        return false;
    tab->activePane()->navigateTo(m_harness->fixtureUri() + QStringLiteral("/many"));
    if (!m_harness->until([tab, count] { return tab->activePane()->files()->rowCount() == count; }, 20000))
        return false;

    tab->setViewMode(BrowserController::ViewMode::Gallery);
    m_harness->settle(4);
    return true;
}

QQuickItem* TestThumbnails::tiles()
{
    QQuickItem* grid = m_harness->item(QStringLiteral("fileGrid"));
    if (!grid)
        return nullptr;
    return m_harness->until([grid] { return grid->isVisible() && grid->width() > 0; }) ? grid : nullptr;
}

/// Starting the application must not bring up Qt Multimedia.
///
/// Asking it anything -- and `QMediaFormat::supportedVideoCodecs()` is anything --
/// creates its platform integration, and creating that builds the whole GStreamer
/// stack: a device monitor with every provider it can find, enumerating ALSA
/// cards, opening a PulseAudio context and starting PipeWire threads. Measured at
/// 710 ms and five extra threads on the author's machine, on the GUI thread, before
/// the window appears. A file manager cannot spend that on the chance that the
/// session might look at a video.
///
/// So the question is deferred to the file that needs it answered, and this is the
/// guard on that: the loader's own table, read after the application has started
/// and its plugins have registered.
void TestThumbnails::startingTheApplicationDoesNotBuildTheMediaStack()
{
    const QString loaded = modulesLoadedIntoThisProcess();
    if (loaded.isEmpty())
        QSKIP("nothing here can say which modules the loader mapped");

    QVERIFY2(!loaded.contains(QLatin1String("libgst")),
        "starting the application built the GStreamer stack, which costs most of a second "
        "before the window appears");

#ifdef MOLE_HAVE_MULTIMEDIA
    // And asking about an ordinary file must not build it either, or the first
    // gallery of anything at all pays the same price. The suffix is a question for
    // the MIME database, which costs nothing; only a file that really looks like a
    // video is worth waking a decoder for.
    QVERIFY(m_harness->writeFile(QStringLiteral("notes.txt")));
    FileEntry text;
    text.uri = VfsUri::fromLocalPath(QDir(m_harness->fixturePath()).filePath(QStringLiteral("notes.txt")));
    text.name = QStringLiteral("notes.txt");
    const VideoThumbnailer thumbnailer;
    QVERIFY(!thumbnailer.canThumbnail(text));
    QVERIFY2(!modulesLoadedIntoThisProcess().contains(QLatin1String("libgst")),
        "asking the video thumbnailer about a text file built the GStreamer stack");
#endif
}

void TestThumbnails::scrollingAFolderNeverRunsMoreDecodesThanTheBound()
{
    QVERIFY(fillGallery(240));
    QQuickItem* grid = tiles();
    QVERIFY(grid);

    ThumbnailPump* pump = m_harness->thumbnails();
    QVERIFY(pump);
    // Two, so the bound is visible on a machine of any size.
    pump->setConcurrency(2);

    // Something is asked for as soon as the folder is on screen.
    QVERIFY(m_harness->until([pump] { return pump->waiting() > 0; }, 20000));

    // Top to bottom and back, releasing as it goes so decodes really finish and
    // the queue really turns over.
    int worst = 0;
    const qreal end = qMax<qreal>(0, grid->property("contentHeight").toReal() - grid->height());
    for (int step = 0; step <= 20; ++step) {
        const qreal at = (step <= 10 ? step : 20 - step) / 10.0;
        grid->setProperty("contentY", end * at);
        m_harness->settle(2);
        m_gate->release(8);
        m_harness->settle(2);
        worst = qMax(worst, pump->outstanding());
    }

    QVERIFY2(worst <= 2,
        qPrintable(QStringLiteral("%1 decodes were running at once against a bound of 2").arg(worst)));
    QVERIFY2(m_log->made.load() > 0, "something has to have been decoded, or this asserted nothing");
    // And nowhere near one per entry: what is on screen is what is asked for.
    QVERIFY2(m_log->made.load() < 240,
        qPrintable(QStringLiteral("%1 decodes for a folder of 240").arg(m_log->made.load())));
}

/// Otherwise walking through five folders leaves five folders' worth of decoding
/// behind, and the folder on screen is behind all of it.
void TestThumbnails::leavingAFolderTakesItsQueueWithIt()
{
    QVERIFY(fillGallery(120));
    QVERIFY(tiles());

    ThumbnailPump* pump = m_harness->thumbnails();
    QVERIFY(pump);
    pump->setConcurrency(1);
    QVERIFY(m_harness->until([pump] { return pump->queued() > 0; }, 20000));
    const int queuedThere = pump->queued();
    QVERIFY(queuedThere > 0);

    // Up a level, to a folder of a few entries.
    browser()->activePane()->navigateTo(m_harness->fixtureUri());
    QVERIFY(m_harness->until([this] { return !browser()->activePane()->isLoading(); }, 20000));
    m_harness->settle(6);

    QVERIFY2(pump->queued() < queuedThere,
        qPrintable(QStringLiteral("%1 of the %2 queued decodes outlived the folder")
                       .arg(pump->queued())
                       .arg(queuedThere)));
    // The one still running is allowed to finish -- it is past the point of no
    // return -- but nothing is queued for a folder nobody is looking at.
    QVERIFY(pump->queued() <= browser()->activePane()->files()->rowCount());
}

/// The pane's own load of a folder is never behind a queue of decodes: the listing
/// is what the user is waiting for.
void TestThumbnails::aListingIsNeverBehindAQueueOfDecodes()
{
    QVERIFY(fillGallery(200));
    QVERIFY(tiles());

    ThumbnailPump* pump = m_harness->thumbnails();
    QVERIFY(pump);
    QVERIFY(m_harness->until([pump] { return pump->outstanding() > 0; }, 20000));

    // Every decode is held, so if the listing shared their queue it would never
    // arrive. Nothing is released for the rest of this test.
    QVERIFY(m_harness->makeDirs(QStringLiteral("elsewhere")));
    for (int i = 0; i < 5; ++i)
        QVERIFY(m_harness->writeFile(QStringLiteral("elsewhere/note-%1.txt").arg(i)));

    browser()->activePane()->navigateTo(m_harness->fixtureUri() + QStringLiteral("/elsewhere"));
    QVERIFY2(m_harness->until([this] { return browser()->activePane()->files()->rowCount() == 5; }, 20000),
        "a listing behind a queue of held decodes is a window that has stopped answering");
}

int main(int argc, char** argv)
{
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestThumbnails testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_Thumbnails.moc"
