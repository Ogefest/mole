#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/MountTableWatch.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <memory>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace mole;
using namespace mole::test;

namespace {

#ifdef Q_OS_UNIX
/// A mount table nobody has to mount anything to change: the text is held here,
/// and a byte down a pipe is the kernel saying it moved. The notifier is real, so
/// what is under test is the watch's own wiring and not a stand-in for it.
struct FakeTable
{
    int pipe[2] = { -1, -1 };
    QByteArray text = "/dev/sda2 / ext4 rw 0 0\n";
    int reads = 0;

    FakeTable()
    {
        if (::pipe(pipe) != 0) {
            pipe[0] = pipe[1] = -1;
            return;
        }
        // Not blocking, so draining it stops when it is empty.
        ::fcntl(pipe[0], F_SETFL, ::fcntl(pipe[0], F_GETFL) | O_NONBLOCK);
    }
    ~FakeTable()
    {
        if (pipe[1] >= 0)
            ::close(pipe[1]);
    }

    MountTableWatch::Source source()
    {
        MountTableWatch::Source out;
        out.descriptor = pipe[0];
        out.wakesOn = QSocketNotifier::Read;
        // Draining the pipe is what re-arms it, as reading the real table does.
        out.read = [this] {
            char drain[64];
            while (::read(pipe[0], drain, sizeof drain) > 0) { }
            ++reads;
            return text;
        };
        const int readEnd = pipe[0];
        out.close = [readEnd] { ::close(readEnd); };
        return out;
    }

    /// What the kernel does when the table changes.
    void wake() { (void)!::write(pipe[1], "!", 1); }
};
#endif

/// Turns the event loop over without waiting on any clock: there is no timer in
/// the watch, so anything it would do in response to nothing has had its chance.
void turnTheLoop()
{
    for (int round = 0; round < 20; ++round)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

} // namespace

class TestMountTableWatch : public QObject
{
    Q_OBJECT

private slots:
    void aChangeToTheTableIsSaidOnce();
    void anUnchangedTableIsNeitherReadNorReported();
    void aWakeThatFindsTheSameTableSaysNothing();
    void whereThereIsNoTableToWatchNothingIsStarted();
    void theRealTableIsWatchedOnLinux();
};

void TestMountTableWatch::aChangeToTheTableIsSaidOnce()
{
#ifndef Q_OS_UNIX
    QSKIP("the fake table is a pipe");
#else
    FakeTable table;
    QVERIFY(table.pipe[0] >= 0);
    MountTableWatch watch(table.source());
    QVERIFY(watch.isWatching());
    QVERIFY(watch.whyNotWatching().isEmpty());
    QSignalSpy changed(&watch, &MountTableWatch::changed);

    // A stick appears.
    table.text += "/dev/sdb1 /media/me/STICK vfat rw 0 0\n";
    table.wake();
    QVERIFY(waitFor([&] { return changed.count() == 1; }));

    // And it is said once: the wake has been answered and nothing re-fires.
    turnTheLoop();
    QCOMPARE(changed.count(), 1);
    QCOMPARE(watch.readCount(), 2);

    // It goes away again.
    table.text = "/dev/sda2 / ext4 rw 0 0\n";
    table.wake();
    QVERIFY(waitFor([&] { return changed.count() == 2; }));
#endif
}

/// What makes this cheaper than a timer. A watch that wakes spuriously is a timer
/// with extra steps, so while the table does not change nothing is read and
/// nothing is sent.
void TestMountTableWatch::anUnchangedTableIsNeitherReadNorReported()
{
#ifndef Q_OS_UNIX
    QSKIP("the fake table is a pipe");
#else
    FakeTable table;
    MountTableWatch watch(table.source());
    QSignalSpy changed(&watch, &MountTableWatch::changed);
    QCOMPARE(table.reads, 1); // the starting read, which is the baseline

    turnTheLoop();
    QCOMPARE(table.reads, 1);
    QCOMPARE(changed.count(), 0);
#endif
}

/// A mount and its unmount between two reads: the kernel woke the watch, and the
/// table is what it was. Nothing changed as far as anybody listening is concerned.
void TestMountTableWatch::aWakeThatFindsTheSameTableSaysNothing()
{
#ifndef Q_OS_UNIX
    QSKIP("the fake table is a pipe");
#else
    FakeTable table;
    MountTableWatch watch(table.source());
    QSignalSpy changed(&watch, &MountTableWatch::changed);

    table.wake();
    QVERIFY(waitFor([&] { return table.reads == 2; }));
    turnTheLoop();
    QCOMPARE(changed.count(), 0);
#endif
}

/// macOS and Windows have no table to poll, and a timer over what is read there
/// instead -- a statvfs per mount -- is MOLE-361 on a schedule. Asserted on Linux,
/// because the platform is an argument rather than an #ifdef. See ADR-0104.
void TestMountTableWatch::whereThereIsNoTableToWatchNothingIsStarted()
{
    for (const HostPlatform platform : { HostPlatform::MacOS, HostPlatform::Windows }) {
        MountTableWatch watch(platform);
        QVERIFY(!watch.isWatching());
        QVERIFY(!watch.whyNotWatching().isEmpty());
        QCOMPARE(watch.readCount(), 0);
    }
}

void TestMountTableWatch::theRealTableIsWatchedOnLinux()
{
#ifndef Q_OS_LINUX
    QSKIP("only Linux has /proc/self/mounts");
#else
    MountTableWatch watch(HostPlatform::Posix);
    QVERIFY2(watch.isWatching(), qPrintable(watch.whyNotWatching()));
    QCOMPARE(watch.readCount(), 1);
#endif
}

MOLE_TEST_MAIN(TestMountTableWatch)
#include "tst_MountTableWatch.moc"
