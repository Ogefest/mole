#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/events/EventBus.h"

#include <QSignalSpy>
#include <QThread>

#include <atomic>
#include <thread>

using namespace mole;
using namespace mole::test;

class TestEventBus : public QObject
{
    Q_OBJECT

private slots:
    void deliversSynchronouslyOnTheOwningThread();
    void entryCreatedAlsoAnnouncesItsDirectory();
    void entryRenamedAnnouncesBothDirectories();
    void renameWithinOneDirectoryAnnouncesItOnce();
    void deliversFromWorkerThreadsOnTheOwningThread();
    void notificationsCarrySeverity();
    void customTopicsPassThroughUntouched();
    void noSubscribersIsHarmless();
};

void TestEventBus::deliversSynchronouslyOnTheOwningThread()
{
    EventBus bus;
    QSignalSpy spy(&bus, &EventBus::mountsChanged);

    bus.postMountsChanged();

    // Same-thread events must not wait for the next event-loop turn, or the UI
    // would visibly lag one frame behind its own actions.
    QCOMPARE(spy.count(), 1);
}

void TestEventBus::entryCreatedAlsoAnnouncesItsDirectory()
{
    EventBus bus;
    QSignalSpy created(&bus, &EventBus::entryCreated);
    QSignalSpy changed(&bus, &EventBus::directoryChanged);

    bus.postEntryCreated(VfsUri::fromString(QStringLiteral("file:///home/user/new.txt")));

    QCOMPARE(created.count(), 1);
    // An open pane listens for directoryChanged, not entryCreated, so the
    // derived event has to be emitted too or nothing would refresh.
    QCOMPARE(changed.count(), 1);
    QCOMPARE(
        changed.first().first().value<VfsUri>(), VfsUri::fromString(QStringLiteral("file:///home/user")));
}

void TestEventBus::entryRenamedAnnouncesBothDirectories()
{
    EventBus bus;
    QSignalSpy changed(&bus, &EventBus::directoryChanged);

    bus.postEntryRenamed(VfsUri::fromString(QStringLiteral("file:///a/one.txt")),
        VfsUri::fromString(QStringLiteral("file:///b/two.txt")));

    QCOMPARE(changed.count(), 2);
    QCOMPARE(changed.at(0).first().value<VfsUri>(), VfsUri::fromString(QStringLiteral("file:///a")));
    QCOMPARE(changed.at(1).first().value<VfsUri>(), VfsUri::fromString(QStringLiteral("file:///b")));
}

void TestEventBus::renameWithinOneDirectoryAnnouncesItOnce()
{
    EventBus bus;
    QSignalSpy changed(&bus, &EventBus::directoryChanged);

    bus.postEntryRenamed(VfsUri::fromString(QStringLiteral("file:///a/one.txt")),
        VfsUri::fromString(QStringLiteral("file:///a/two.txt")));

    QCOMPARE(changed.count(), 1);
}

void TestEventBus::deliversFromWorkerThreadsOnTheOwningThread()
{
    EventBus bus;
    QThread* const mainThread = QThread::currentThread();

    std::atomic_int received { 0 };
    std::atomic_bool wrongThread { false };

    connect(&bus, &EventBus::indexUpdated, this, [&](qint64, qint64) {
        if (QThread::currentThread() != mainThread)
            wrongThread = true;
        ++received;
    });

    // Background work publishes from pool threads; subscribers must never have
    // to think about locking, so delivery is marshalled here.
    constexpr int kPublishers = 4;
    constexpr int kPerPublisher = 25;
    std::vector<std::thread> publishers;
    for (int p = 0; p < kPublishers; ++p) {
        publishers.emplace_back([&bus] {
            for (int i = 0; i < kPerPublisher; ++i)
                bus.postIndexUpdated(1, i);
        });
    }
    for (std::thread& publisher : publishers)
        publisher.join();

    QVERIFY(waitFor([&received] { return received.load() == kPublishers * kPerPublisher; }));
    QVERIFY2(!wrongThread.load(), "events must always arrive on the bus's own thread");
}

void TestEventBus::notificationsCarrySeverity()
{
    EventBus bus;
    QSignalSpy spy(&bus, &EventBus::notificationPosted);

    bus.postNotification(
        EventBus::Severity::Error, QStringLiteral("Mount failed"), QStringLiteral("host unreachable"));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<EventBus::Severity>(), EventBus::Severity::Error);
    QCOMPARE(spy.first().at(1).toString(), QStringLiteral("Mount failed"));
    QCOMPARE(spy.first().at(2).toString(), QStringLiteral("host unreachable"));
}

void TestEventBus::customTopicsPassThroughUntouched()
{
    EventBus bus;
    QSignalSpy spy(&bus, &EventBus::customEvent);

    QVariantMap payload { { QStringLiteral("branch"), QStringLiteral("main") } };
    bus.postCustom(QStringLiteral("org.example.gitlab/branchChanged"), payload);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toString(), QStringLiteral("org.example.gitlab/branchChanged"));
    QCOMPARE(spy.first().at(1).toMap(), payload);
}

void TestEventBus::noSubscribersIsHarmless()
{
    // Harmless means *silent* as well as not fatal: a bus that logged a warning
    // for every event nobody wanted would fill the session log with the ordinary
    // case. `QVERIFY(true)` said only that the three calls returned, which they
    // would have done while complaining. See MOLE-399.
    CapturedWarnings said(QtWarningMsg);

    EventBus bus;
    bus.postMountsChanged();
    bus.postDirectoryChanged(VfsUri::fromString(QStringLiteral("file:///tmp")));
    bus.postNotification(EventBus::Severity::Info, QStringLiteral("hello"));

    // Delivered on the event loop, so the posts have to be allowed to land before
    // asking what was said about them.
    QCoreApplication::processEvents();

    QVERIFY2(said.messages().isEmpty(), qPrintable(said.joined()));
}

MOLE_TEST_MAIN(TestEventBus)
#include "tst_EventBus.moc"
