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
    void twoPanesAskingAtOnceDecodeItOnce();

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
    QObject::connect(
        m_pump.get(), &ThumbnailPump::ready, m_pump.get(), [&](QObject* who, const QImage& image) {
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
    QObject::connect(
        m_pump.get(), &ThumbnailPump::ready, m_pump.get(), [&](QObject* who, const QImage& image) {
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
    QObject::connect(
        m_pump.get(), &ThumbnailPump::ready, m_pump.get(), [&](QObject* who, const QImage& image) {
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

MOLE_TEST_MAIN(TestThumbnailSource)
#include "tst_ThumbnailSource.moc"
