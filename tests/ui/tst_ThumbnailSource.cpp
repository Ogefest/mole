#include "host/ThumbnailRegistry.h"
#include "support/FakePlugin.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/ThumbnailCache.h"
#include "ui/ThumbnailSource.h"

#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

using namespace mole;
using namespace mole::test;

/// Everything about producing a thumbnail except the Qt Quick types, which is the
/// half that can be held to the house rules: off the GUI thread, cancellable, and
/// one winner per file. See MOLE-140.
class TestThumbnailSource : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void theUrlCarriesTheUriTheSizeAndTheDate();
    void aUrlWithNothingUsableInItIsNotAThumbnail();
    void nothingDecodesOnTheGuiThread();
    void theHighestPriorityThumbnailerAnswersAndTheOthersAreNotAsked();
    void aFileNoThumbnailerClaimsAnswersWithNothing();
    void cancellingAResponseReachesTheDecode();
    void anAnswerArrivesExactlyOnceEvenForACancelledRequest();
    void aPictureAskedForTwiceIsDecodedOnce();
    void aSecondRunReadsFromDiskAndDecodesNothing();
    void aTileEvictedFromMemoryComesBackFromDiskWithoutDecoding();
    void twoPanesAskingAtOnceDecodeItOnce();
    void onlySoManyDecodeAtOnceAndTheRestQueue();
    void whatCameIntoViewLastIsServedFirst();
    void abandoningARequestBeforeItStartsCostsNothing();

private:
    /// The answer for `id`, waited on rather than timed.
    QImage awaitThumbnail(QObject* response, const QString& id);

    std::unique_ptr<QTemporaryDir> m_dir;
    std::shared_ptr<MemoryFileSystem> m_fs;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<EventBus> m_events;
    std::unique_ptr<IndexDatabase> m_index;
    std::unique_ptr<ThumbnailRegistry> m_registry;
    PluginServices m_services;
    std::unique_ptr<ThumbnailPump> m_pump;
    /// Built only by the tests that are about caching, so every other test here
    /// counts real decodes.
    std::unique_ptr<ThumbnailCache> m_cache;
};

void TestThumbnailSource::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_fs = std::make_shared<MemoryFileSystem>();
    m_fs->addFile(QStringLiteral("/photos/a.jpg"), QByteArray("pretend"));
    m_fs->addFile(QStringLiteral("/notes.txt"), QByteArray("words"));

    m_vfs = std::make_unique<VfsManager>();
    Mount mount;
    mount.displayName = QStringLiteral("scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = m_fs;
    QVERIFY(!m_vfs->addMount(mount).isEmpty());

    m_tasks = std::make_unique<TaskManager>();
    m_events = std::make_unique<EventBus>();
    m_index = std::make_unique<IndexDatabase>(QDir(m_dir->path()).filePath(QStringLiteral("i.sqlite")));
    QVERIFY(m_index->open().ok());
    m_registry = std::make_unique<ThumbnailRegistry>();

    m_services = PluginServices { m_vfs.get(), m_tasks.get(), m_index.get(), m_events.get() };
    m_services.thumbnails = m_registry.get();
    m_pump = std::make_unique<ThumbnailPump>(m_services, nullptr);
}

void TestThumbnailSource::cleanup()
{
    m_pump.reset();
    m_cache.reset();
    m_tasks.reset();
    m_registry.reset();
    m_index.reset();
    m_events.reset();
    m_vfs.reset();
    m_fs.reset();
    m_dir.reset();
}

QImage TestThumbnailSource::awaitThumbnail(QObject* response, const QString& id)
{
    QImage answer;
    bool arrived = false;
    // The context object dies with this call, and the connection with it. Bound to
    // the pump instead, a later answer would run a lambda whose captures are this
    // function's dead stack -- which is what make asan said.
    QObject scope;
    QObject::connect(m_pump.get(), &ThumbnailPump::ready, &scope, [&](QObject* who, const QImage& image) {
        if (who != response)
            return;
        answer = image;
        arrived = true;
    });
    m_pump->startFor(response, id);
    if (!waitFor([&] { return arrived; }, 10000))
        return {};
    return answer;
}

void TestThumbnailSource::theUrlCarriesTheUriTheSizeAndTheDate()
{
    // A path with everything in it that would break a url built by concatenation:
    // a space, a hash, a question mark, a percent sign.
    const VfsUri awkward = VfsUri::fromString(QStringLiteral("mem:///odd/a b#c?d%e.jpg"));
    const QString url = ThumbnailKey::urlFor(awkward, 160, 1700000000);
    QVERIFY(url.startsWith(QStringLiteral("image://mole-thumb/")));

    const QString id = url.mid(QStringLiteral("image://mole-thumb/").size());
    const ThumbnailKey back = ThumbnailKey::parse(id);
    QVERIFY2(back.isValid(), qPrintable(id));
    QCOMPARE(back.uri.toString(), awkward.toString());
    QCOMPARE(back.size, 160);
    QCOMPARE(back.mtime, 1700000000);

    // The same file at two sizes is two keys, because they are two pictures.
    QVERIFY(ThumbnailKey::urlFor(awkward, 160, 1) != ThumbnailKey::urlFor(awkward, 320, 1));
    // And an edited file is a new key, which is what the date is in there for.
    QVERIFY(ThumbnailKey::urlFor(awkward, 160, 1) != ThumbnailKey::urlFor(awkward, 160, 2));
}

void TestThumbnailSource::aUrlWithNothingUsableInItIsNotAThumbnail()
{
    QVERIFY(!ThumbnailKey::parse(QStringLiteral("nonsense")).isValid());
    QVERIFY(!ThumbnailKey::parse(QStringLiteral("mem%3A%2F%2F%2Fa.jpg")).isValid()); // no query at all
    QVERIFY(!ThumbnailKey::parse(QStringLiteral("mem%3A%2F%2F%2Fa.jpg?size=0&mtime=1")).isValid());

    // And a request for one answers rather than hanging, because a view waiting
    // for a picture that will never come is a tile that never settles.
    QObject response;
    QCOMPARE(awaitThumbnail(&response, QStringLiteral("nonsense")), QImage());
}

/// The house rule: the UI thread never touches storage, and a 24-megapixel decode
/// on it would freeze the window for exactly as long as it takes.
void TestThumbnailSource::nothingDecodesOnTheGuiThread()
{
    auto log = std::make_shared<FakeThumbnailer::Log>();
    QVERIFY(m_registry->addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("test.only"), QColor(Qt::green), 0, log)));

    QObject response;
    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/a.jpg"));
    key.size = 64;
    key.mtime = 5;
    const QImage image = awaitThumbnail(&response, key.toId());
    QVERIFY(!image.isNull());
    QCOMPARE(log->made.load(), 1);
    // The size the tile asked for reaches the thumbnailer, not a default.
    QCOMPARE(log->lastSize.load(), 64);
    QVERIFY2(log->ranOn.load() != QThread::currentThread(),
        "a thumbnail decoded on the GUI thread is a frozen window");
}

void TestThumbnailSource::theHighestPriorityThumbnailerAnswersAndTheOthersAreNotAsked()
{
    auto below = std::make_shared<FakeThumbnailer::Log>();
    auto above = std::make_shared<FakeThumbnailer::Log>();
    // Registered lowest first, so the answer cannot come from registration order.
    QVERIFY(m_registry->addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("test.below"), QColor(Qt::red), 0, below)));
    QVERIFY(m_registry->addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("test.above"), QColor(Qt::blue), 100, above)));

    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/a.jpg"));
    key.size = 32;
    auto* task = new ThumbnailTask(m_services, key, nullptr);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->answeredBy(), QStringLiteral("test.above"));
    QCOMPARE(task->image().pixelColor(0, 0), QColor(Qt::blue));
    QCOMPARE(above->made.load(), 1);
    QVERIFY2(below->made.load() == 0, "a file has one picture, so the loser is not asked");
}

void TestThumbnailSource::aFileNoThumbnailerClaimsAnswersWithNothing()
{
    QVERIFY(m_registry->addThumbnailer(std::make_unique<FakeThumbnailer>(
        QStringLiteral("test.jpegs"), QColor(Qt::green), 0, nullptr, QStringLiteral("jpg"))));

    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));
    key.size = 32;
    auto* task = new ThumbnailTask(m_services, key, nullptr);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    // An ordinary answer: the tile keeps its icon and nothing is said about it.
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->image().isNull());
    QVERIFY(task->answeredBy().isEmpty());
}

/// Qt calls cancel() when the Image asking for a picture is destroyed, which is
/// what a GridView does the moment a delegate leaves its cache buffer. It has to
/// reach the decode, or a flick through a folder leaves work nobody wants.
void TestThumbnailSource::cancellingAResponseReachesTheDecode()
{
    auto log = std::make_shared<FakeThumbnailer::Log>();
    auto gate = std::make_shared<QSemaphore>();
    GateRelease release(gate);
    auto held = std::make_unique<FakeThumbnailer>(QStringLiteral("test.held"), QColor(Qt::green), 0, log);
    held->holdUntilReleased(gate);
    QVERIFY(m_registry->addThumbnailer(std::move(held)));

    QObject response;
    QImage answer;
    bool arrived = false;
    // Dies with the test body, taking the connection with it -- see awaitThumbnail.
    QObject scope;
    QObject::connect(m_pump.get(), &ThumbnailPump::ready, &scope, [&](QObject* who, const QImage& image) {
        if (who != &response)
            return;
        answer = image;
        arrived = true;
    });

    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/a.jpg"));
    key.size = 48;
    m_pump->startFor(&response, key.toId());

    // Held inside the decode, which is the only moment cancelling means anything.
    QVERIFY(waitFor([&] { return log->made.load() == 1; }));
    QCOMPARE(m_pump->outstanding(), 1);
    m_pump->cancelFor(&response);
    gate->release();

    // The response is answered at once -- Qt expects a cancelled one to finish
    // rather than to hang -- and the decode itself finds its token set.
    QVERIFY(waitFor([&] { return arrived; }, 10000));
    QVERIFY2(answer.isNull(), "a cancelled decode is not a picture");
    QVERIFY2(waitFor([&] { return log->cancelled.load() == 1; }, 10000),
        "the cancellation has to reach the decode, not only the response");
    QVERIFY(waitFor([&] { return m_pump->outstanding() == 0; }, 10000));
    QCOMPARE(m_pump->waiting(), 0);
}

void TestThumbnailSource::anAnswerArrivesExactlyOnceEvenForACancelledRequest()
{
    QVERIFY(m_registry->addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("test.only"), QColor(Qt::green))));

    QObject response;
    QSignalSpy answers(m_pump.get(), &ThumbnailPump::ready);

    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/a.jpg"));
    key.size = 24;
    m_pump->startFor(&response, key.toId());
    // Asked twice for the same response, which is not a second picture.
    m_pump->startFor(&response, key.toId());
    QVERIFY(waitFor([&] { return answers.count() > 0; }, 10000));

    // Cancelling something already answered is not an error and not a second
    // answer: by then the response may not even exist any more.
    m_pump->cancelFor(&response);
    QTest::qWait(20);
    QCOMPARE(answers.count(), 1);
}

/// A GridView destroys a delegate that leaves its cache buffer and builds a new
/// one when it comes back, so without a cache the second look at a picture costs
/// exactly what the first one did. Counted rather than timed. See MOLE-141.
void TestThumbnailSource::aPictureAskedForTwiceIsDecodedOnce()
{
    m_cache = std::make_unique<ThumbnailCache>(QDir(m_dir->path()).filePath(QStringLiteral("thumbs")));
    m_pump = std::make_unique<ThumbnailPump>(m_services, m_cache.get());

    auto log = std::make_shared<FakeThumbnailer::Log>();
    QVERIFY(m_registry->addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("test.only"), QColor(Qt::green), 0, log)));

    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/a.jpg"));
    key.size = 40;
    key.mtime = 7;

    QObject first;
    QVERIFY(!awaitThumbnail(&first, key.toId()).isNull());
    QCOMPARE(log->made.load(), 1);

    // The same tile scrolled away and back: answered from memory, and on the spot
    // rather than through a task.
    QObject second;
    QVERIFY(!awaitThumbnail(&second, key.toId()).isNull());
    QCOMPARE(log->made.load(), 1);

    // A different date is a different picture, which is what the date is for.
    key.mtime = 8;
    QObject edited;
    QVERIFY(!awaitThumbnail(&edited, key.toId()).isNull());
    QCOMPARE(log->made.load(), 2);
}

void TestThumbnailSource::aSecondRunReadsFromDiskAndDecodesNothing()
{
    const QString directory = QDir(m_dir->path()).filePath(QStringLiteral("thumbs"));
    auto log = std::make_shared<FakeThumbnailer::Log>();
    QVERIFY(m_registry->addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("test.only"), QColor(Qt::green), 0, log)));

    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/a.jpg"));
    key.size = 40;
    key.mtime = 7;

    m_cache = std::make_unique<ThumbnailCache>(directory);
    m_pump = std::make_unique<ThumbnailPump>(m_services, m_cache.get());
    QObject first;
    QVERIFY(!awaitThumbnail(&first, key.toId()).isNull());
    QCOMPARE(log->made.load(), 1);

    // A fresh cache and a fresh pump over the same directory, which is what
    // opening the folder again tomorrow looks like.
    m_pump.reset();
    m_cache = std::make_unique<ThumbnailCache>(directory);
    m_pump = std::make_unique<ThumbnailPump>(m_services, m_cache.get());

    QObject again;
    QVERIFY(!awaitThumbnail(&again, key.toId()).isNull());
    QVERIFY2(log->made.load() == 1, "a second visit to a folder must not decode it again");
}

/// The same picture asked for again *in the same run*, after the memory tier has
/// let it go.
///
/// aSecondRunReadsFromDiskAndDecodesNothing above builds a fresh cache over the
/// same directory, which is what opening the folder again tomorrow looks like. This
/// is the other case and the one that actually happens: the memory tier is a few
/// tens of megabytes, a folder of several hundred photographs walks straight past
/// it, and the first tile is the first evicted. Scrolling back to the top then asks
/// for it again inside the same cache object.
///
/// Found through the task strip: `27-gallery` in the guide counted seventeen
/// finished jobs on some runs and sixteen on others, because whether that second
/// request needed a task at all depended on what was still resident. What it must
/// never need is a second decode. See MOLE-258.
void TestThumbnailSource::aTileEvictedFromMemoryComesBackFromDiskWithoutDecoding()
{
    const QString directory = QDir(m_dir->path()).filePath(QStringLiteral("thumbs"));
    auto log = std::make_shared<FakeThumbnailer::Log>();
    QVERIFY(m_registry->addThumbnailer(
        std::make_unique<FakeThumbnailer>(QStringLiteral("test.only"), QColor(Qt::green), 0, log)));
    m_fs->addFile(QStringLiteral("/photos/b.jpg"), QByteArray("pretend too"));

    const auto keyFor = [](const QString& path) {
        ThumbnailKey key;
        key.uri = VfsUri::fromString(QStringLiteral("mem://") + path);
        key.size = 40;
        key.mtime = 7;
        return key;
    };
    const ThumbnailKey first = keyFor(QStringLiteral("/photos/a.jpg"));
    const ThumbnailKey second = keyFor(QStringLiteral("/photos/b.jpg"));

    // A memory tier that holds one tile and no more, so asking for the second
    // evicts the first -- the same thing a real cap does over a real folder, at a
    // size a test can be sure of. The disk tier keeps its default room.
    m_cache = std::make_unique<ThumbnailCache>(directory, 1, ThumbnailCache::kDefaultDiskCap);
    m_pump = std::make_unique<ThumbnailPump>(m_services, m_cache.get());

    QObject one;
    QVERIFY(!awaitThumbnail(&one, first.toId()).isNull());
    QCOMPARE(log->made.load(), 1);
    QObject two;
    QVERIFY(!awaitThumbnail(&two, second.toId()).isNull());
    QCOMPARE(log->made.load(), 2);
    QVERIFY2(m_cache->inMemory(first).isNull(),
        "the point of the case is that the first tile is no longer resident");

    // And now the scroll back up. It may cost a task -- reading storage is not
    // something the interface thread may do -- but it may not cost a decode.
    QObject again;
    QVERIFY(!awaitThumbnail(&again, first.toId()).isNull());
    QVERIFY2(log->made.load() == 2,
        qPrintable(QStringLiteral("a tile that is still on disk was decoded again: %1 decodes")
                       .arg(log->made.load())));
}

/// Two panes showing one folder ask for the same picture at the same moment, and
/// decoding it twice is twice the work for one answer.
void TestThumbnailSource::twoPanesAskingAtOnceDecodeItOnce()
{
    auto log = std::make_shared<FakeThumbnailer::Log>();
    auto gate = std::make_shared<QSemaphore>();
    GateRelease release(gate);
    auto held = std::make_unique<FakeThumbnailer>(QStringLiteral("test.held"), QColor(Qt::green), 0, log);
    held->holdUntilReleased(gate);
    QVERIFY(m_registry->addThumbnailer(std::move(held)));

    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/a.jpg"));
    key.size = 44;

    QObject left;
    QObject right;
    QList<QObject*> answered;
    // Dies with the test body, taking the connection with it -- see awaitThumbnail.
    QObject scope;
    QObject::connect(m_pump.get(), &ThumbnailPump::ready, &scope, [&](QObject* who, const QImage& image) {
        if (!image.isNull())
            answered.append(who);
    });

    m_pump->startFor(&left, key.toId());
    QVERIFY(waitFor([&] { return log->made.load() == 1; }));
    // The second one arrives while the first is still being made.
    m_pump->startFor(&right, key.toId());

    QCOMPARE(m_pump->outstanding(), 1);
    QCOMPARE(m_pump->waiting(), 2);
    gate->release();

    QVERIFY(waitFor([&] { return answered.size() == 2; }, 10000));
    QVERIFY2(log->made.load() == 1, "one picture, one decode, however many are waiting for it");
    QVERIFY(answered.contains(&left));
    QVERIFY(answered.contains(&right));
}

/// A flick through eight hundred photographs asked for eight hundred decodes at
/// once. Unbounded parallel decode of 4K JPEGs is a way to make the whole
/// application unresponsive while the file listing is what the user is waiting
/// for. See MOLE-142.
void TestThumbnailSource::onlySoManyDecodeAtOnceAndTheRestQueue()
{
    auto log = std::make_shared<FakeThumbnailer::Log>();
    auto gate = std::make_shared<QSemaphore>();
    GateRelease release(gate);
    auto held = std::make_unique<FakeThumbnailer>(QStringLiteral("test.held"), QColor(Qt::green), 0, log);
    held->holdUntilReleased(gate);
    QVERIFY(m_registry->addThumbnailer(std::move(held)));

    // Two at a time, so the queue is visible on a machine of any size.
    m_pump->setConcurrency(2);

    std::vector<std::unique_ptr<QObject>> responses;
    for (int i = 0; i < 12; ++i) {
        responses.push_back(std::make_unique<QObject>());
        ThumbnailKey key;
        key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/shot-%1.jpg").arg(i));
        key.size = 40;
        m_pump->startFor(responses.back().get(), key.toId());
    }

    QVERIFY(waitFor([&] { return log->made.load() == 2; }));
    // And it stays at two: the other ten are asked for and not started.
    QTest::qWait(50);
    QCOMPARE(m_pump->outstanding(), 2);
    QCOMPARE(log->made.load(), 2);
    QCOMPARE(m_pump->queued(), 10);
    QCOMPARE(m_pump->waiting(), 12);

    // Released, and the queue drains without ever going over the bound.
    int seenOver = 0;
    // Dies with the test body, taking the connection with it -- see awaitThumbnail.
    QObject scope;
    QObject::connect(m_pump.get(), &ThumbnailPump::ready, &scope, [&](QObject*, const QImage&) {
        if (m_pump->outstanding() > 2)
            ++seenOver;
    });
    gate->release(64);
    QVERIFY(waitFor([&] { return m_pump->waiting() == 0; }, 20000));
    QCOMPARE(log->made.load(), 12);
    QCOMPARE(seenOver, 0);
}

/// Without this the view fills in listing order, which is the order the user is
/// scrolling away from.
void TestThumbnailSource::whatCameIntoViewLastIsServedFirst()
{
    auto log = std::make_shared<FakeThumbnailer::Log>();
    auto gate = std::make_shared<QSemaphore>();
    GateRelease release(gate);
    auto held = std::make_unique<FakeThumbnailer>(QStringLiteral("test.held"), QColor(Qt::green), 0, log);
    held->holdUntilReleased(gate);
    QVERIFY(m_registry->addThumbnailer(std::move(held)));

    m_pump->setConcurrency(1);

    // One decode holds the only slot; the next two queue behind it.
    QObject busy;
    QObject early;
    QObject late;
    const auto keyFor = [](const QString& name) {
        ThumbnailKey key;
        key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/%1").arg(name));
        key.size = 40;
        return key;
    };
    m_pump->startFor(&busy, keyFor(QStringLiteral("busy.jpg")).toId());
    QVERIFY(waitFor([&] { return log->made.load() == 1; }));

    m_pump->startFor(&early, keyFor(QStringLiteral("early.jpg")).toId());
    m_pump->startFor(&late, keyFor(QStringLiteral("late.jpg")).toId());
    QCOMPARE(m_pump->queued(), 2);

    QStringList order;
    // Dies with the test body, taking the connection with it -- see awaitThumbnail.
    QObject scope;
    QObject::connect(m_pump.get(), &ThumbnailPump::ready, &scope, [&](QObject* who, const QImage&) {
        order.append(who == &busy ? QStringLiteral("busy")
                : who == &early   ? QStringLiteral("early")
                                  : QStringLiteral("late"));
    });

    gate->release(64);
    QVERIFY(waitFor([&] { return order.size() == 3; }, 20000));
    QCOMPARE(order.first(), QStringLiteral("busy"));
    QVERIFY2(order.indexOf(QStringLiteral("late")) < order.indexOf(QStringLiteral("early")),
        qPrintable(QStringLiteral("served in the order %1").arg(order.join(QStringLiteral(", ")))));
}

/// Leaving a folder destroys its delegates, which is how the queue is cleared:
/// walking through five folders must not leave five folders' worth of decoding
/// behind the one on screen.
void TestThumbnailSource::abandoningARequestBeforeItStartsCostsNothing()
{
    auto log = std::make_shared<FakeThumbnailer::Log>();
    auto gate = std::make_shared<QSemaphore>();
    GateRelease release(gate);
    auto held = std::make_unique<FakeThumbnailer>(QStringLiteral("test.held"), QColor(Qt::green), 0, log);
    held->holdUntilReleased(gate);
    QVERIFY(m_registry->addThumbnailer(std::move(held)));

    m_pump->setConcurrency(1);

    QObject busy;
    std::vector<std::unique_ptr<QObject>> leaving;
    ThumbnailKey key;
    key.uri = VfsUri::fromString(QStringLiteral("mem:///photos/busy.jpg"));
    key.size = 40;
    m_pump->startFor(&busy, key.toId());
    QVERIFY(waitFor([&] { return log->made.load() == 1; }));

    for (int i = 0; i < 6; ++i) {
        leaving.push_back(std::make_unique<QObject>());
        ThumbnailKey queued;
        queued.uri = VfsUri::fromString(QStringLiteral("mem:///gone/shot-%1.jpg").arg(i));
        queued.size = 40;
        m_pump->startFor(leaving.back().get(), queued.toId());
    }
    QCOMPARE(m_pump->queued(), 6);

    // The folder is left, so Qt cancels every one of its requests.
    for (const auto& response : leaving)
        m_pump->cancelFor(response.get());
    QCOMPARE(m_pump->queued(), 0);
    QCOMPARE(m_pump->waiting(), 1); // only the one that was already running

    gate->release(64);
    QVERIFY(waitFor([&] { return m_pump->waiting() == 0; }, 20000));
    QVERIFY2(log->made.load() == 1, "a queued decode that nobody wants any more costs nothing at all");
}

MOLE_TEST_MAIN(TestThumbnailSource)
#include "tst_ThumbnailSource.moc"
