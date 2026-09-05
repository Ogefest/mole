#include "plugins/network/SftpFileSystem.h"
#include "plugins/network/WebdavFileSystem.h"
#include "scale/HeavyPayload.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "support/TestbedControl.h"
#include "support/Victim.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

namespace {

QString env(const char* name)
{
    return QString::fromLocal8Bit(qgetenv(name));
}

qint64 envBytes(const char* name, qint64 fallback)
{
    bool ok = false;
    const qint64 value = env(name).toLongLong(&ok);
    return ok && value > 0 ? value : fallback;
}

SftpSettings settingsFromEnvironment(int port)
{
    SftpSettings settings;
    settings.host = env("MOLE_TEST_SFTP_HOST");
    settings.port = port;
    settings.username = env("MOLE_TEST_SFTP_USER");
    settings.password = env("MOLE_TEST_SFTP_PASS");
    settings.remoteRoot = QStringLiteral("/");
    return settings;
}

QString remoteBase()
{
    const QString base = env("MOLE_TEST_SFTP_BASE");
    return base.isEmpty() ? QStringLiteral("/Shared") : base;
}

} // namespace

/// The same transfers, with the server being attacked while they run.
///
/// Everything here needs two things that are absent on an ordinary machine: an
/// account on a test server, and a control channel that can do something to it.
/// Without either, every case skips itself and says which one was missing --
/// silence is how three backends went a year without ever meeting a server.
///
/// **What is asserted is not that the transfer survives.** Some of these it must
/// survive and some it must not; what every one of them has in common is that
/// the outcome is *named*. A copy that hangs, or that reports success with half
/// a file at the far end, is the failure this suite exists to rule out -- the
/// difference between a file manager somebody can trust with a backup and one
/// they cannot.
class TestInterference : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void theConnectionDiesMidUpload();
    void theConnectionDiesMidDownload();
    void theServerGoesAwayAndComesBack();

    void aSlowOrLossyLinkStillDelivers_data();
    void aSlowOrLossyLinkStillDelivers();

    void aChangedHostKeyIsRefused();
    void theProcessIsKilledMidUpload();

    // Last, and in this order, because these are the cases that can leave the
    // machine unreachable for a while: a total outage takes the link down and
    // filling the disk takes the services with it. A case that ran after one of
    // them and reported "no route to host" would be reporting on the previous
    // case rather than on the product.
    void theDestinationFillsUpMidCopy();
    void theOutageCutsTheTransferAndNotTheChannel();
    void anOutageShorterThanTheGuardIsSurvived_data();
    void anOutageShorterThanTheGuardIsSurvived();
    void anOutageLongerThanTheGuardIsReported();

private:
    /// Builds a payload on local disk and returns its uri.
    VfsUri makeSource(const QString& name, qint64 bytes);
    /// Puts one on the server, out of the way of the interference, for the
    /// cases that read rather than write.
    VfsUri seedRemote(const QString& name, qint64 bytes);
    /// A transfer that was interfered with, and whether the interference
    /// actually landed while it was still running.
    ///
    /// `landed` is not a detail. A transfer that finished before anything could
    /// be done to it passes every assertion below for the wrong reason -- it was
    /// never attacked -- and that is a green test that checks nothing, which is
    /// worse than a red one.
    struct Interfered
    {
        TransferTask* task = nullptr;
        bool landed = false;
        qint64 atBytes = 0;
        QString answer; ///< what the machine said it did
    };

    /// Submits the transfer and, once it has moved `atBytes`, interferes.
    ///
    /// Waiting on the byte count rather than on a clock is what puts the
    /// interference in the middle of the transfer instead of before or after
    /// it, on a fast machine and on a slow one alike.
    Interfered runInterfered(const TransferTask::Request& request, qint64 atBytes,
        const std::function<QString()>& interfere, int timeoutMs = 600000);

    /// The transfer `runInterfered` currently has in flight.
    ///
    /// An interfere callback is built before the task exists, so this is how one
    /// asks what the transfer is doing while it is being attacked -- which is
    /// the whole of what the case below checks.
    TransferTask* m_running = nullptr;

    FileSystemPtr m_drive;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<QTemporaryDir> m_dir;
    qint64 m_payload = 0;
    QString m_name;
};

void TestInterference::initTestCase()
{
    if (env("MOLE_TEST_SFTP_HOST").isEmpty())
        QSKIP("No SFTP account in the environment; `make test-heavy` sets one up.");
    if (!TestbedControl::isAvailable()) {
        QSKIP("No control channel. Set MOLE_TEST_CONTROL to a command that can interfere with the "
              "server -- scripts/testbed/control.sh install puts it there.");
    }
    // Small by default: these cases are about what happens *during* a transfer,
    // and the two outage ones slow the link down so that a quarter of a gigabyte
    // lasts long enough to be interrupted twice over.
    m_payload = envBytes("MOLE_TEST_INTERFERENCE_BYTES", 256 * 1024 * 1024);
}

void TestInterference::init()
{
    m_disk = std::make_shared<LocalFileSystem>();
    m_drive = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settingsFromEnvironment(22));
    m_tasks = std::make_unique<TaskManager>();
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_name = QStringLiteral("mole-interference-%1.bin").arg(QCoreApplication::applicationPid());

    // The previous case may have left the machine on its back -- an outage
    // clears itself, but not instantly, and a case that starts before the link
    // is up again reports "no route to host" as though it were a fault in the
    // product. So each one waits for a machine that is answering, and skips
    // itself with the reason rather than blaming the code.
    const VfsUri root(QStringLiteral("sftp"), QString(), remoteBase());
    if (!waitFor([this, root] { return m_drive->list(root, CancelToken()).ok(); }, 120000))
        QSKIP("the machine has not come back from the previous case");
}

void TestInterference::cleanup()
{
    // Whatever a case did to the machine, the next one starts from a machine
    // that is behaving. A suite that leaves netem on poisons everything after it.
    if (TestbedControl::isAvailable())
        TestbedControl::restore();
    if (m_drive) {
        const VfsUri onServer = VfsUri(QStringLiteral("sftp"), QString(), remoteBase()).child(m_name);
        m_drive->remove(onServer, false);
        // Whatever staging file is actually there. Deriving the name removed
        // nothing at all and left every killed case's wreckage on the server, a
        // quarter of a gibibyte at a time. See MOLE-433.
        const VfsUri partial = partialWriteFor(*m_drive, onServer);
        if (partial.isValid())
            m_drive->remove(partial, false);
    }

    // And on the small disk, whatever a case aimed at it. A quarter of a
    // gibibyte left behind per run fills a four-gigabyte disk in eight runs, and
    // then every case that needs room fails for a reason that has nothing to do
    // with the code -- which is exactly what happened.
    if (!env("MOLE_TEST_WEBDAV_URL").isEmpty()) {
        WebdavSettings settings;
        settings.baseUrl = env("MOLE_TEST_WEBDAV_URL");
        settings.username = env("MOLE_TEST_WEBDAV_USER");
        settings.password = env("MOLE_TEST_WEBDAV_PASS");
        settings.verifyTls = env("MOLE_TEST_IGNORE_SELF_SIGNED_CERT").isEmpty();
        settings.remoteRoot = QStringLiteral("/");
        auto dav = std::make_shared<WebdavFileSystem>(QStringLiteral("dav"), settings);
        const VfsUri landed = VfsUri(QStringLiteral("dav"), QString(), QStringLiteral("/")).child(m_name);
        dav->remove(landed, false);
        const VfsUri partial = partialWriteFor(*dav, landed);
        if (partial.isValid())
            dav->remove(partial, false);
    }
    m_running = nullptr; // the manager owns it, and the next case has its own
    m_tasks.reset();
    m_dir.reset();
    m_drive.reset();
    m_disk.reset();
}

VfsUri TestInterference::makeSource(const QString& name, qint64 bytes)
{
    const QString path = QDir(m_dir->path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    QString problem;
    if (!HeavyPayload::writeTo(file, bytes, &problem)) {
        qWarning("%s", qPrintable(problem));
        return {};
    }
    file.close();
    return VfsUri::fromLocalPath(path);
}

VfsUri TestInterference::seedRemote(const QString& name, qint64 bytes)
{
    const VfsUri source = makeSource(name, bytes);
    if (!source.isValid())
        return {};

    TransferTask::Request request;
    request.sourceFileSystem = m_disk;
    request.targetFileSystem = m_drive;
    request.sources = { source };
    request.targetDirectory = VfsUri(QStringLiteral("sftp"), QString(), remoteBase());
    request.onConflict = TransferTask::Conflict::Overwrite;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    if (!waitForTask(task, 600000) || !task->failures().isEmpty()) {
        qWarning("could not seed the server: %s", qPrintable(task->failures().join(QLatin1Char(' '))));
        return {};
    }
    QFile::remove(source.toLocalPath());
    return VfsUri(QStringLiteral("sftp"), QString(), remoteBase()).child(name);
}

TestInterference::Interfered TestInterference::runInterfered(const TransferTask::Request& request,
    qint64 atBytes, const std::function<QString()>& interfere, int timeoutMs)
{
    Interfered out;
    out.task = new TransferTask(request);
    m_running = out.task;
    m_tasks->submit(out.task);

    auto* task = out.task;
    if (!waitFor([task, atBytes] { return task->bytesDone() >= atBytes || task->isFinished(); }, 300000))
        qWarning("the transfer never reached %lld bytes", static_cast<long long>(atBytes));

    out.atBytes = task->bytesDone();
    if (!task->isFinished()) {
        QElapsedTimer sinceInterference;
        sinceInterference.start();
        out.answer = interfere();
        out.landed = true;
        qInfo("interfered at %lld bytes, after %lld ms of transfer; the machine took %lld ms to answer: %s",
            static_cast<long long>(out.atBytes), static_cast<long long>(task->elapsedMs()),
            static_cast<long long>(sinceInterference.elapsed()), qPrintable(out.answer));
    } else {
        qInfo("nothing was interfered with: the transfer was already over");
    }

    waitForTask(task, timeoutMs);
    qInfo("it ended as %s after %lld ms, having moved %lld bytes",
        task->state() == Task::State::Succeeded    ? "succeeded"
            : task->state() == Task::State::Failed ? "failed"
                                                   : "cancelled",
        static_cast<long long>(task->elapsedMs()), static_cast<long long>(task->bytesDone()));
    return out;
}

namespace {

/// Fails the case when the transfer was over before anything could be done to
/// it. The message says what to change, because the answer is always the same:
/// a bigger payload or a slower link.
#define VERIFY_IT_LANDED(run)                                                                                \
    QVERIFY2((run).landed,                                                                                   \
        qPrintable(QStringLiteral("the transfer finished before it could be interfered with (%1 bytes "      \
                                  "moved). Raise MOLE_TEST_INTERFERENCE_BYTES or slow the link further.")    \
                       .arg((run).atBytes)))

} // namespace

void TestInterference::theConnectionDiesMidUpload()
{
    const VfsUri source = makeSource(m_name, m_payload);
    QVERIFY(source.isValid());

    TransferTask::Request request;
    request.sourceFileSystem = m_disk;
    request.targetFileSystem = m_drive;
    request.sources = { source };
    request.targetDirectory = VfsUri(QStringLiteral("sftp"), QString(), remoteBase());
    request.onConflict = TransferTask::Conflict::Overwrite;

    const Interfered run = runInterfered(request, m_payload / 4,
        [] { return TestbedControl::run({ QStringLiteral("kill-connections"), QStringLiteral("22") }); });
    VERIFY_IT_LANDED(run);
    TransferTask* task = run.task;

    // Named, either way. A connection cut at 25% may be reported as a failure or
    // may be survived by a retry, and both are answers; what must not happen is
    // silence, a hang, or a file at the far end under the name somebody asked
    // for holding a quarter of the bytes.
    QVERIFY2(task->isFinished(), "the upload never finished after the connection was cut");
    if (!task->failures().isEmpty()) {
        const QString failure = task->failures().first();
        QVERIFY2(failure.contains(m_name), qPrintable(failure));
        const VfsUri landed = VfsUri(QStringLiteral("sftp"), QString(), remoteBase()).child(m_name);
        QVERIFY2(!m_drive->stat(landed).ok(), "a failed upload left a file under the name it was aiming at");
    } else {
        QCOMPARE(task->copiedCount(), 1);
    }
}

void TestInterference::theConnectionDiesMidDownload()
{
    const VfsUri remote = seedRemote(m_name, m_payload);
    QVERIFY(remote.isValid());

    QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("back")));
    TransferTask::Request request;
    request.sourceFileSystem = m_drive;
    request.targetFileSystem = m_disk;
    request.sources = { remote };
    request.targetDirectory = VfsUri::fromLocalPath(QDir(m_dir->path()).filePath(QStringLiteral("back")));

    const Interfered run = runInterfered(request, m_payload / 4,
        [] { return TestbedControl::run({ QStringLiteral("kill-connections"), QStringLiteral("22") }); });
    VERIFY_IT_LANDED(run);
    TransferTask* task = run.task;

    QVERIFY2(task->isFinished(), "the download never finished after the connection was cut");
    const QString landed = QDir(m_dir->path()).filePath(QStringLiteral("back/") + m_name);
    if (!task->failures().isEmpty()) {
        QVERIFY2(task->failures().first().contains(m_name), qPrintable(task->failures().first()));
        QVERIFY2(!QFile::exists(landed), "a failed download left a file under the name it was aiming at");
    } else {
        QFile copy(landed);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QString problem;
        QVERIFY2(HeavyPayload::verify(copy, m_payload, &problem), qPrintable(problem));
    }
}

void TestInterference::theServerGoesAwayAndComesBack()
{
    const VfsUri source = makeSource(m_name, m_payload);
    QVERIFY(source.isValid());

    TransferTask::Request request;
    request.sourceFileSystem = m_disk;
    request.targetFileSystem = m_drive;
    request.sources = { source };
    request.targetDirectory = VfsUri(QStringLiteral("sftp"), QString(), remoteBase());
    request.onConflict = TransferTask::Conflict::Overwrite;

    const Interfered run = runInterfered(request, m_payload / 4, [] {
        return TestbedControl::run(
            { QStringLiteral("service"), QStringLiteral("restart"), QStringLiteral("ssh") });
    });
    VERIFY_IT_LANDED(run);
    TransferTask* task = run.task;

    QVERIFY2(task->isFinished(), "the transfer never finished after the server was restarted");
    if (!task->failures().isEmpty()) {
        QVERIFY2(task->failures().first().contains(m_name), qPrintable(task->failures().first()));
        const VfsUri landed = VfsUri(QStringLiteral("sftp"), QString(), remoteBase()).child(m_name);
        QVERIFY2(!m_drive->stat(landed).ok(), "a failed upload left a file under its final name");
    }

    // And the drive works again afterwards, which is the other half of "came
    // back": a backend that gave up on a server for good would pass everything
    // above and be useless.
    QVERIFY(waitFor(
        [this] {
            return m_drive->list(VfsUri(QStringLiteral("sftp"), QString(), remoteBase()), CancelToken()).ok();
        },
        60000));
}

void TestInterference::aSlowOrLossyLinkStillDelivers_data()
{
    QTest::addColumn<QString>("what");
    QTest::addColumn<QString>("amount");

    QTest::newRow("200 ms of latency") << QStringLiteral("delay") << QStringLiteral("200ms");
    QTest::newRow("1% packet loss") << QStringLiteral("loss") << QStringLiteral("1%");
    QTest::newRow("5% packet loss") << QStringLiteral("loss") << QStringLiteral("5%");
}

void TestInterference::aSlowOrLossyLinkStillDelivers()
{
    QFETCH(QString, what);
    QFETCH(QString, amount);

    // Smaller: a bad link is meant to make this slow, and the assertion is that
    // every byte still arrives rather than that it arrives quickly.
    const qint64 bytes = envBytes("MOLE_TEST_INTERFERENCE_LOSSY_BYTES", 32 * 1024 * 1024);
    const VfsUri source = makeSource(m_name, bytes);
    QVERIFY(source.isValid());

    // Applied for the whole run and cleared by the machine itself afterwards --
    // this channel travels over the link it is damaging.
    TestbedControl::run({ QStringLiteral("netem"), what, amount, QStringLiteral("300") });

    TransferTask::Request request;
    request.sourceFileSystem = m_disk;
    request.targetFileSystem = m_drive;
    request.sources = { source };
    request.targetDirectory = VfsUri(QStringLiteral("sftp"), QString(), remoteBase());
    request.onConflict = TransferTask::Conflict::Overwrite;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY2(waitForTask(task, 900000), "the transfer never finished over a bad link");
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));

    // Read back through the same bad link and checked byte for byte: a lossy
    // link that silently drops a block is the failure worth ruling out here.
    TestbedControl::run({ QStringLiteral("netem"), QStringLiteral("clear") });
    const VfsUri landed = VfsUri(QStringLiteral("sftp"), QString(), remoteBase()).child(m_name);
    Result<std::unique_ptr<QIODevice>> back = m_drive->openRead(landed, bytes);
    QVERIFY2(back.ok(), qPrintable(back.error().message));
    QString problem;
    QVERIFY2(HeavyPayload::verify(*back.value(), bytes, &problem), qPrintable(problem));
}

/// The instrument, held against the one thing that made it unusable.
///
/// Everything else in this file assumes it can attack the machine and still
/// reach it afterwards, and for a while that was luck. The first way of causing
/// a total outage was `netem loss 100%` on the root qdisc, which stops the
/// machine answering ARP: unreachable to everything, including the timer
/// scheduled to clear the rule and the command that would check on it. It cost
/// two rescues over the hypervisor's guest agent, and it is why the two cases
/// below spent nine days behind an environment variable.
///
/// So this proves the property the rest of the tier rests on, rather than
/// assuming it: while a transfer on one port is cut stone dead, the control
/// channel on its own port answers *every time it is asked*, for the whole
/// outage. One reply at the end would not do -- the interesting failure is a
/// channel that goes away in the middle and comes back when the rule clears,
/// which looks identical from either end of the outage.
///
/// See ADR-0054 for why the channel has a server to itself.
void TestInterference::theOutageCutsTheTransferAndNotTheChannel()
{
    const VfsUri remote = seedRemote(m_name, m_payload);
    QVERIFY(remote.isValid());

    QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("back")));
    TransferTask::Request request;
    request.sourceFileSystem = m_drive;
    request.targetFileSystem = m_disk;
    request.sources = { remote };
    request.targetDirectory = VfsUri::fromLocalPath(QDir(m_dir->path()).filePath(QStringLiteral("back")));

    // Slowed first, so the payload outlasts the outage and the outage lands in
    // the middle of a transfer rather than after one.
    //
    // Faster than the two cases below, which want a link slow enough to survive
    // two minutes of silence. Twenty seconds needs far less, and four megabytes
    // a second still leaves about a minute of transfer after the trigger point --
    // while the eight megabits those use would make this case four minutes long
    // for nothing. The arithmetic is worth writing down, because the failure it
    // guards against is silent: a transfer that ends before it is attacked
    // passes every assertion here for the wrong reason, which is what
    // VERIFY_IT_LANDED is for.
    const QString limited = TestbedControl::run(
        { QStringLiteral("netem"), QStringLiteral("rate"), QStringLiteral("32mbit"), QStringLiteral("300") });
    QVERIFY2(limited.contains(QStringLiteral("rate limited")), qPrintable(limited));

    // Twenty seconds, which is the figure MOLE-109 names, and comfortably inside
    // the transfer's budget -- what is being measured here is the instrument,
    // not the giving-up path.
    const int outageSeconds = 20;
    QStringList refusals;
    int replies = 0;
    qint64 movedEarly = -1;
    qint64 movedLate = -1;

    const Interfered run = runInterfered(request, m_payload / 8, [&] {
        const QString started = TestbedControl::run(
            { QStringLiteral("blackhole"), QStringLiteral("22"), QString::number(outageSeconds) });

        QElapsedTimer outage;
        outage.start();
        // A short timeout on purpose. The default is a minute, and a channel
        // that had been cut would spend the whole outage inside one call and
        // then report a single failure -- which is the same answer as "asked
        // once, at the end", and the thing this case exists not to accept.
        const int patience = 8000;
        while (outage.elapsed() < outageSeconds * 1000) {
            const QString answer = TestbedControl::run({ QStringLiteral("status") }, patience);
            if (answer.contains(QStringLiteral("mole-control:")))
                ++replies;
            else
                refusals += QStringLiteral("at %1 ms: %2").arg(outage.elapsed()).arg(answer);

            // Two readings of the transfer, three seconds apart at the ends of
            // the outage. The first is late enough that bytes already in flight
            // when the rule landed have arrived; anything between the two is a
            // transfer the blackhole did not actually stop.
            if (movedEarly < 0 && outage.elapsed() > 3000)
                movedEarly = m_running->bytesDone();
            if (outage.elapsed() > (outageSeconds - 3) * 1000)
                movedLate = m_running->bytesDone();
        }
        return started;
    });
    VERIFY_IT_LANDED(run);

    QVERIFY2(refusals.isEmpty(),
        qPrintable(QStringLiteral("the control channel stopped answering during the outage: %1")
                       .arg(refusals.join(QStringLiteral("; ")))));
    QVERIFY2(replies >= 2,
        qPrintable(QStringLiteral("the channel was only asked %1 times; that proves nothing about "
                                  "the middle of the outage")
                       .arg(replies)));

    QVERIFY2(movedEarly >= 0 && movedLate >= 0, "the outage was too short to take two readings");
    QVERIFY2(movedLate == movedEarly,
        qPrintable(QStringLiteral("the transfer moved %1 bytes during a total outage, so the "
                                  "blackhole did not cut it")
                       .arg(movedLate - movedEarly)));

    // And the machine comes back on its own, which is the other half of an
    // instrument nobody has to watch: the rule clears itself and the transfer
    // it interrupted finishes whole.
    TransferTask* task = run.task;
    QVERIFY2(waitForTask(task, 300000), "the transfer never finished after the outage cleared");
    QVERIFY2(task->failures().isEmpty(),
        qPrintable(QStringLiteral("a %1-second outage was not survived: %2")
                       .arg(outageSeconds)
                       .arg(task->failures().join(QLatin1Char(' ')))));

    QFile copy(QDir(m_dir->path()).filePath(QStringLiteral("back/") + m_name));
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QString problem;
    QVERIFY2(HeavyPayload::verify(copy, m_payload, &problem), qPrintable(problem));
}

void TestInterference::anOutageShorterThanTheGuardIsSurvived_data()
{
    // Two rows, and they share one watchdog.
    //
    // QTest arms `QTEST_FUNCTION_TIMEOUT` once for the function and does not
    // rearm it between data rows, while the time it prints when it fires is the
    // *row's*. Each row here is about seven and a half minutes -- a quarter of a
    // gigabyte at eight megabits, plus the outage -- so fifteen minutes kills the
    // second one a minute before it would have finished, and reports "440
    // seconds" while doing it, which looks like anything but the real cause.
    // `make test-heavy` allows two hours. Anybody running this binary by hand
    // wants at least twenty minutes.
    QTest::addColumn<int>("unused");
    QTest::newRow("a minute") << 0;
    QTest::newRow("a second inside the budget") << 0;
}

void TestInterference::anOutageShorterThanTheGuardIsSurvived()
{
    // Run unattended, which it was not for nine days.
    //
    // The outage these need used to be `netem loss 100%` on the root qdisc, and
    // that stops the machine answering ARP: unreachable to everything, including
    // the timer scheduled to clear the rule. Twice the way back in was the
    // hypervisor's guest agent, now scripts/testbed/rescue.sh, and while that
    // was the instrument these two cases sat behind an environment variable --
    // which meant in practice they did not run, and they are the two that found
    // MOLE-108.
    //
    // The outage is per-port now, over a control channel with a server to
    // itself that mole-control refuses to be pointed at (ADR-0054), and
    // theOutageCutsTheTransferAndNotTheChannel above holds that property
    // directly. So they run with everything else.

    // A transfer is given up on after two minutes with no byte arriving. An
    // outage inside that has to be ridden out: the link comes back, the next
    // attempt delivers, the clock resets and the file arrives whole. No single
    // connection survives the outage -- they die and are retried, which is the
    // point -- so what is being tested is the budget rather than a connection's
    // luck.
    //
    // Two rows, because they answer different questions. A minute is the honest
    // regression case: comfortably inside the budget, and it must pass on any
    // machine. One minute fifty-nine is the boundary, one second inside a budget
    // of a hundred and twenty, and it is expected to be sensitive -- an attempt
    // has to be in flight almost all of the time for it to pass, which is why
    // the backoff between attempts is capped at two seconds.
    const VfsUri remote = seedRemote(m_name, m_payload);
    QVERIFY(remote.isValid());

    QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("back")));
    TransferTask::Request request;
    request.sourceFileSystem = m_drive;
    request.targetFileSystem = m_disk;
    request.sources = { remote };
    request.targetDirectory = VfsUri::fromLocalPath(QDir(m_dir->path()).filePath(QStringLiteral("back")));

    // Slowed first, so a quarter of a gigabyte lasts longer than the outage and
    // the outage lands in the middle of a transfer rather than after it.
    // Slowed first, so the payload outlasts the outage and the outage lands in
    // the middle of a transfer rather than after one. The rate limit clears
    // itself, as everything done to this machine does.
    const QString limited = TestbedControl::run(
        { QStringLiteral("netem"), QStringLiteral("rate"), QStringLiteral("8mbit"), QStringLiteral("400") });
    QVERIFY2(limited.contains(QStringLiteral("rate limited")), qPrintable(limited));

    const QString seconds = QString::number(QTest::currentDataTag() == QLatin1String("a minute") ? 60 : 119);
    const Interfered run = runInterfered(request, m_payload / 8, [seconds] {
        return TestbedControl::run({ QStringLiteral("blackhole"), QStringLiteral("22"), seconds });
    });
    VERIFY_IT_LANDED(run);
    TransferTask* task = run.task;

    QVERIFY2(task->isFinished(), "the download never finished");
    QVERIFY2(task->failures().isEmpty(),
        qPrintable(QStringLiteral("an outage of %1 seconds was not survived: %2")
                       .arg(seconds, task->failures().join(QLatin1Char(' ')))));

    QFile copy(QDir(m_dir->path()).filePath(QStringLiteral("back/") + m_name));
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QString problem;
    QVERIFY2(HeavyPayload::verify(copy, m_payload, &problem), qPrintable(problem));
}

void TestInterference::anOutageLongerThanTheGuardIsReported()
{
    // Run unattended, which it was not for nine days.
    //
    // The outage these need used to be `netem loss 100%` on the root qdisc, and
    // that stops the machine answering ARP: unreachable to everything, including
    // the timer scheduled to clear the rule. Twice the way back in was the
    // hypervisor's guest agent, now scripts/testbed/rescue.sh, and while that
    // was the instrument these two cases sat behind an environment variable --
    // which meant in practice they did not run, and they are the two that found
    // MOLE-108.
    //
    // The outage is per-port now, over a control channel with a server to
    // itself that mole-control refuses to be pointed at (ADR-0054), and
    // theOutageCutsTheTransferAndNotTheChannel above holds that property
    // directly. So they run with everything else.

    const VfsUri remote = seedRemote(m_name, m_payload);
    QVERIFY(remote.isValid());

    QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("back")));
    TransferTask::Request request;
    request.sourceFileSystem = m_drive;
    request.targetFileSystem = m_disk;
    request.sources = { remote };
    request.targetDirectory = VfsUri::fromLocalPath(QDir(m_dir->path()).filePath(QStringLiteral("back")));

    // Slowed first, so the payload outlasts the outage and the outage lands in
    // the middle of a transfer rather than after one. The rate limit clears
    // itself, as everything done to this machine does.
    const QString limited = TestbedControl::run(
        { QStringLiteral("netem"), QStringLiteral("rate"), QStringLiteral("8mbit"), QStringLiteral("400") });
    QVERIFY2(limited.contains(QStringLiteral("rate limited")), qPrintable(limited));

    // Four minutes, and the number is arithmetic rather than a guess: the
    // transfer's budget is two minutes with no byte arriving, plus one attempt's
    // worth of noticing -- twenty seconds for the socket to admit the link has
    // gone, twenty more for the connect that replaces it -- plus room for a slow
    // machine. Measured at 174 seconds after the outage began.
    //
    // Down from seven minutes, which was two whole budgets: before, a dead link
    // cost the guard once for the span that stopped and again for the single
    // resume. Anything past four minutes now is not slowness, it is a transfer
    // that is never going to end, and the number has to be finite for the
    // verdict to mean anything.
    const Interfered run = runInterfered(
        request, m_payload / 8,
        [] {
            // Comfortably past the budget rather than just past it. A hundred
            // and forty against a hundred and twenty leaves twenty seconds of
            // margin, and the clock does not start when the outage does -- it
            // starts at the last byte, which is however long the connection took
            // to notice plus whatever was still in flight. Measured: a 140-second
            // outage was ridden out and the transfer went on, which is right
            // behaviour and the wrong test.
            return TestbedControl::run(
                { QStringLiteral("blackhole"), QStringLiteral("22"), QStringLiteral("200") });
        },
        240000);
    VERIFY_IT_LANDED(run);
    TransferTask* task = run.task;

    // The guard must fire, and it must say what happened. "It stopped" is not
    // something anybody can act on; "less than a byte a second for two minutes"
    // is.
    QVERIFY2(task->isFinished(),
        qPrintable(QStringLiteral("the download neither finished nor failed: %1 bytes moved, still "
                                  "running after %2 seconds, against a stall guard set to 120. "
                                  "That is MOLE-108.")
                       .arg(task->bytesDone())
                       .arg(task->elapsedMs() / 1000)));
    QVERIFY2(!task->failures().isEmpty(), "an outage past the stall guard was reported as a success");
    const QString failure = task->failures().first();
    qInfo("the guard said: %s", qPrintable(failure));
    QVERIFY2(failure.contains(m_name), qPrintable(failure));
    QVERIFY2(
        failure.contains(QStringLiteral("too slow")) || failure.contains(QStringLiteral("stopped after")),
        qPrintable(failure));

    QVERIFY2(!QFile::exists(QDir(m_dir->path()).filePath(QStringLiteral("back/") + m_name)),
        "a download the guard gave up on left a file under its final name");
}

void TestInterference::theDestinationFillsUpMidCopy()
{
    const QString url = env("MOLE_TEST_WEBDAV_URL");
    if (url.isEmpty())
        QSKIP("No WebDAV account; the small disk is what fills up, and that is where its root is.");

    // The room is taken away *before* the copy starts, rather than while it runs.
    //
    // Filling a disk takes a `dd` the length of the disk -- half a minute here --
    // and the first version of this raced that against the upload and lost: the
    // copy was finished and reported before the disk was full, and the case
    // passed a transfer that had never met the condition. Leaving too little room
    // needs no timing at all and exercises the same path, a write that fails part
    // way through with no space left, every single time.
    // Emptied first: a ballast left by an earlier run makes "fill to 98%" a
    // no-op on a disk that is already at 100%, and the case would then be
    // measuring nothing.
    TestbedControl::run({ QStringLiteral("empty") });
    const QString filled = TestbedControl::run({ QStringLiteral("fill"), QStringLiteral("90") }, 180000);
    QVERIFY2(filled.contains(QStringLiteral("filled")), qPrintable(filled));

    // The payload is sized from what the machine says is left, rather than the
    // ballast being sized to leave a known amount. Filling to a percentage
    // eats into the filesystem's reserve -- five per cent of an ext4 belongs to
    // root, and the ballast is written by root -- so "98% full" leaves a
    // non-root writer nothing at all and the case cannot tell "no room" from
    // "no room to even start".
    const qint64 room
        = TestbedControl::run({ QStringLiteral("room"), QStringLiteral("webdav") }).trimmed().toLongLong();
    QVERIFY2(room > 1024 * 1024,
        qPrintable(QStringLiteral("nothing can be written at all: %1 bytes left").arg(room)));
    const qint64 payload = room + 64LL * 1024 * 1024;
    qInfo("the destination has %lld bytes left and is about to be sent %lld", static_cast<long long>(room),
        static_cast<long long>(payload));

    const VfsUri source = makeSource(m_name, payload);
    QVERIFY(source.isValid());

    WebdavSettings settings;
    settings.baseUrl = url;
    settings.username = env("MOLE_TEST_WEBDAV_USER");
    settings.password = env("MOLE_TEST_WEBDAV_PASS");
    settings.verifyTls = env("MOLE_TEST_IGNORE_SELF_SIGNED_CERT").isEmpty();
    settings.remoteRoot = QStringLiteral("/");
    auto dav = std::make_shared<WebdavFileSystem>(QStringLiteral("dav"), settings);

    TransferTask::Request request;
    request.sourceFileSystem = m_disk;
    request.targetFileSystem = dav;
    request.sources = { source };
    request.targetDirectory = VfsUri(QStringLiteral("dav"), QString(), QStringLiteral("/"));
    request.onConflict = TransferTask::Conflict::Overwrite;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY2(waitForTask(task, 900000), "the upload hung when the destination ran out of room");

    // A copy that did not fit must say so. Reported as done, it is the worst
    // outcome available: nothing later looks wrong, and a move would have
    // deleted the original.
    QVERIFY2(!task->failures().isEmpty(), "a copy onto a disk with no room was reported as a success");
    const QString failure = task->failures().first();
    qInfo("it said: %s", qPrintable(failure));
    QVERIFY2(failure.contains(m_name), qPrintable(failure));

    const VfsUri landed = VfsUri(QStringLiteral("dav"), QString(), QStringLiteral("/")).child(m_name);
    QVERIFY2(!dav->stat(landed).ok(), "a copy that ran out of room left a file under its final name");

    TestbedControl::run({ QStringLiteral("empty") });
    const VfsUri partial = partialWriteFor(*dav, landed);
    if (partial.isValid())
        dav->remove(partial, false);
}

void TestInterference::aChangedHostKeyIsRefused()
{
    // The second server, and only ever the second server.
    //
    // Not because of the control channel any more -- that has a third sshd to
    // itself now (ADR-0054) and would not notice. Because every other case in
    // this file talks to port 22, and a server whose identity changed under them
    // would have them all failing for a reason that has nothing to do with the
    // product. `mole-control hostkey` refuses to touch port 22 for the same
    // reason.
    const int port = env("MOLE_TEST_SFTP_REKEY_PORT").toInt();
    if (port <= 0)
        QSKIP("No second server configured. The identity that gets rotated has to belong to a "
              "server no other case in this file is using.");

    // Its own known-hosts file, so this proves the policy rather than whatever
    // the machine running the test happens to have in its home directory -- and
    // so that a rotated key never has to be cleaned out of a real one.
    SftpSettings settings = settingsFromEnvironment(port);
    settings.knownHostsPath = QDir(m_dir->path()).filePath(QStringLiteral("known_hosts"));

    auto first = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settings);
    const VfsUri root(QStringLiteral("sftp"), QString(), remoteBase());
    const Result<FileEntryList> before = first->list(root, CancelToken());
    QVERIFY2(before.ok(), qPrintable(before.error().message));

    TestbedControl::run({ QStringLiteral("hostkey"), QStringLiteral("rotate") });

    auto second = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settings);
    const Result<FileEntryList> after = second->list(root, CancelToken());

    TestbedControl::run({ QStringLiteral("hostkey"), QStringLiteral("restore") });

    // The one SSH warning nobody may wave through. A host we have not met is
    // accepted and remembered; a host whose key has changed is refused, because
    // only the second is evidence of anything.
    QVERIFY2(!after.ok(), "a server whose host key had changed was connected to anyway");
}

void TestInterference::theProcessIsKilledMidUpload()
{
    if (Victim::isThisProcess()) {
        // The victim: upload for ever, and never get to tidy up.
        auto disk = std::make_shared<LocalFileSystem>();
        auto drive = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settingsFromEnvironment(22));

        TransferTask::Request request;
        request.sourceFileSystem = disk;
        request.targetFileSystem = drive;
        request.sources = { VfsUri::fromLocalPath(Victim::instruction()) };
        request.targetDirectory = VfsUri(QStringLiteral("sftp"), QString(), remoteBase());
        request.onConflict = TransferTask::Conflict::Overwrite;

        TaskManager tasks;
        auto* task = new TransferTask(request);
        tasks.submit(task);
        waitForTask(task, 600000);
        return;
    }

    const VfsUri source = makeSource(m_name, m_payload);
    QVERIFY(source.isValid());
    const VfsUri landed = VfsUri(QStringLiteral("sftp"), QString(), remoteBase()).child(m_name);

    Victim victim(QStringLiteral("theProcessIsKilledMidUpload"), source.toLocalPath());
    QVERIFY(victim.started());

    // Killed once the working name has appeared on the server, which is the
    // only moment at which there is something to interrupt.
    //
    // **Found by listing, never by deriving.** This used to compute the name and
    // wait for that -- but a staging name carries a per-open random token since
    // MOLE-359, and the one that matters here is minted inside another process.
    // The name this side computed could not appear, so the wait ran out every
    // time, the child was killed after it had finished uploading, and the case
    // reported "the upload never started" while printing a transcript of the
    // child uploading. See MOLE-433.
    //
    // Thirty seconds, and it is a bound on a hang rather than a measure of
    // anything: the condition decides. 1,200 ms was neither -- it is shorter
    // than a second copy of this binary takes to start and open an SFTP session,
    // so even a derivable name would have been racing.
    QVERIFY2(victim.waitUntil([&] { return partialWriteFor(*m_drive, landed).isValid(); }, 30000, 50),
        qPrintable(QStringLiteral("the upload never started: %1").arg(victim.transcript())));
    victim.kill();

    // What a process killed outright leaves behind: bytes under a name that says
    // what they are, and nothing under the name somebody asked for. It cannot
    // clean up after itself -- no destructor runs -- which is the whole reason
    // the working name exists. See ADR-0020.
    QVERIFY2(
        !m_drive->stat(landed).ok(), "a killed upload left a partial file under the name it was aiming at");

    // And the job runs again over the top of the wreckage.
    TransferTask::Request request;
    request.sourceFileSystem = m_disk;
    request.targetFileSystem = m_drive;
    request.sources = { source };
    request.targetDirectory = VfsUri(QStringLiteral("sftp"), QString(), remoteBase());
    request.onConflict = TransferTask::Conflict::Overwrite;

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 600000));
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));

    Result<std::unique_ptr<QIODevice>> back = m_drive->openRead(landed, m_payload);
    QVERIFY2(back.ok(), qPrintable(back.error().message));
    QString problem;
    QVERIFY2(HeavyPayload::verify(*back.value(), m_payload, &problem), qPrintable(problem));
}

MOLE_TEST_MAIN(TestInterference)
#include "tst_Interference.moc"
