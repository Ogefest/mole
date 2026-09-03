#include "plugins/network/CurlTransport.h"
#include "support/MoleTestMain.h"
#include "support/ScriptedHttpServer.h"

#include <QElapsedTimer>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The transport's judgement of a finished transfer, offline.
///
/// Everything here is about the one question a backend cannot answer for
/// itself: the protocol said the transfer worked, so is the file whole?
class TestCurlTransport : public QObject
{
    Q_OBJECT

private slots:
    void aCompleteDownloadIsAnOk();
    void aDownloadThatStoppedShortIsAnError();
    void anUnknownLengthIsNotTreatedAsShort();
    void aRequestThatAskedForNoBodyIsNotShort();
    void anHttpStatusStillWins();

    void aTransferThatKeepsMovingIsNeverGivenUpOn();
    void aTransferThatStopsMovingIsGivenUpOnAfterTheWait();
    void aSlowButLivingTransferSurvives();
    void theWatchCanBeTurnedOff();

    void aTransferThatGoesQuietIsEndedByTheGuardItself();
    void aCancelTakesEffectAtOnce();

    void nothingWithACredentialInItReachesTheLog();
};

void TestCurlTransport::aCompleteDownloadIsAnOk()
{
    net::Response response;
    response.code = CURLE_OK;
    response.expectedBytes = 4096;
    response.receivedBytes = 4096;

    const VfsError error
        = net::errorFor(response, QStringLiteral("Reading /big.bin"), net::StatusMeaning::ProtocolReply);
    QVERIFY2(!error.isError(), qPrintable(error.message));
}

void TestCurlTransport::aDownloadThatStoppedShortIsAnError()
{
    // What a dropped SFTP channel looks like from up here: libssh2 calls the cut
    // an end of file, so curl reports success and hands back half the file.
    net::Response response;
    response.code = CURLE_OK;
    response.expectedBytes = 4096;
    response.receivedBytes = 1500;

    const VfsError error
        = net::errorFor(response, QStringLiteral("Reading /big.bin"), net::StatusMeaning::ProtocolReply);
    QVERIFY(error.isError());
    QCOMPARE(error.code, VfsError::IoError);
    // The numbers belong in the message: "it failed" does not tell anybody
    // whether they lost a byte or a gigabyte.
    QVERIFY(error.message.contains(QStringLiteral("1500")));
    QVERIFY(error.message.contains(QStringLiteral("4096")));
}

void TestCurlTransport::anUnknownLengthIsNotTreatedAsShort()
{
    // Plenty of servers never say how long the answer is. That is not evidence
    // of anything, and must not turn every such transfer into a failure.
    net::Response response;
    response.code = CURLE_OK;
    response.expectedBytes = -1;
    response.receivedBytes = 900;

    const VfsError error
        = net::errorFor(response, QStringLiteral("Reading /x"), net::StatusMeaning::ProtocolReply);
    QVERIFY2(!error.isError(), qPrintable(error.message));
}

void TestCurlTransport::aRequestThatAskedForNoBodyIsNotShort()
{
    // A HEAD is told the length and asks for none of it. perform() leaves both
    // numbers alone for exactly this reason.
    net::Response response;
    response.code = CURLE_OK;
    response.status = 200;
    response.expectedBytes = -1;
    response.receivedBytes = -1;

    const VfsError error = net::errorFor(response, QStringLiteral("Checking /x"), net::StatusMeaning::Http);
    QVERIFY2(!error.isError(), qPrintable(error.message));
}

void TestCurlTransport::anHttpStatusStillWins()
{
    // A complete transfer of a 404 page is not a file. The length check must not
    // let one through by answering first.
    net::Response response;
    response.code = CURLE_OK;
    response.status = 404;
    response.expectedBytes = 120;
    response.receivedBytes = 120;

    const VfsError error = net::errorFor(response, QStringLiteral("Reading /gone"), net::StatusMeaning::Http);
    QCOMPARE(error.code, VfsError::NotFound);
}

// ---- giving up on a transfer that has stopped -----------------------------
//
// The guard exists because libcurl's own does not fire for SFTP. That was
// measured against a server whose link was cut mid-transfer: the progress
// callback went on being called twice a second with the byte count frozen, and
// the transfer was still running long after CURLOPT_LOW_SPEED_TIME had passed.
// A transfer that neither finishes nor fails is the one outcome a file manager
// may not produce.
//
// The clock is a parameter of hasStalled() rather than something it reads, which
// is what lets these run in microseconds and give the same answer every time. A
// test that slept for the wait would be a test that passes on one machine.

void TestCurlTransport::aTransferThatKeepsMovingIsNeverGivenUpOn()
{
    net::StallWatch watch(120);
    qint64 moved = 0;
    // Two hours of steady progress, checked twice a second the way libcurl calls
    // the callback. Nothing here may ever be given up on.
    for (qint64 ms = 0; ms < 2 * 60 * 60 * 1000; ms += 500) {
        moved += 1024;
        QVERIFY2(!watch.hasStalled(moved, ms),
            qPrintable(QStringLiteral("gave up at %1 ms on a transfer that was moving").arg(ms)));
    }
}

void TestCurlTransport::aTransferThatStopsMovingIsGivenUpOnAfterTheWait()
{
    net::StallWatch watch(120);

    // Thirty-four megabytes arrive, and then the link goes away. This is the
    // shape of the outage that found the fault, byte for byte.
    QVERIFY(!watch.hasStalled(34078720, 5000));

    // Nothing for the whole of the wait: still going, because a transfer is
    // allowed to be quiet for as long as the guard says and not a moment less.
    QVERIFY2(!watch.hasStalled(34078720, 5000 + 119999),
        "given up on one millisecond early, which would make a slow link a failure");

    // And then it is over.
    QVERIFY2(watch.hasStalled(34078720, 5000 + 120000),
        "a transfer that has moved nothing for two minutes has to be given up on");
}

void TestCurlTransport::aSlowButLivingTransferSurvives()
{
    // The case the guard must not catch, and the reason it counts movement
    // rather than speed: a single byte every ninety seconds is a ruinous
    // connection, but it is a connection, and a file manager that gave up on it
    // would be giving up on somebody's transfer over a link they cannot change.
    net::StallWatch watch(120);
    qint64 moved = 0;
    for (int step = 0; step < 20; ++step) {
        const qint64 at = static_cast<qint64>(step) * 90000;
        // Checked at the moment before the byte arrives, when the wait is at its
        // longest, and then again as it lands.
        QVERIFY(!watch.hasStalled(moved, at + 89999));
        ++moved;
        QVERIFY(!watch.hasStalled(moved, at + 90000));
    }
}

void TestCurlTransport::theWatchCanBeTurnedOff()
{
    // Zero means "libcurl's guard and nothing else", which is what a caller
    // asking for no wait at all gets rather than one that fires immediately.
    net::StallWatch watch(0);
    QCOMPARE(watch.patienceMs(), -1);
    QVERIFY(!watch.hasStalled(0, 0));
    QVERIFY2(!watch.hasStalled(0, 10LL * 365 * 24 * 60 * 60 * 1000), "off has to mean off");
}

// ---- the loop Mole owns ---------------------------------------------------
//
// Against a real socket, because what is being tested is that the transport
// stops waiting -- and the thing it used to wait on was libcurl's own loop.
// A server that hangs up is a failure the client is told about; one that goes
// quiet and stays open tells it nothing, which is the case a guard has to be
// the answer to.

void TestCurlTransport::aTransferThatGoesQuietIsEndedByTheGuardItself()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.status = 200;
        reply.body = QByteArray(64 * 1024, 'x');
        // A quarter of the body, and then nothing at all for half a minute --
        // far longer than the guard, so the guard is what ends this or nothing
        // does.
        reply.goQuietAfter = 16 * 1024;
        reply.stayQuietMs = 30000;
        return reply;
    });
    QVERIFY2(server.start(), "could not take a port for the scripted server");

    net::TransportOptions options;
    options.stallSeconds = 2;
    net::CurlPool pool(std::move(options));

    auto lease = pool.take();
    QVERIFY(lease);
    lease.setUrl((server.url() + QStringLiteral("/quiet")).toUtf8());

    QElapsedTimer clock;
    clock.start();
    CancelToken nobodyCancelled;
    const net::Response response = pool.perform(lease, nobodyCancelled);
    const qint64 took = clock.elapsed();

    // At the guard, not at whatever the operating system eventually notices.
    QVERIFY2(
        took < 6000, qPrintable(QStringLiteral("waited %1 ms against a guard of two seconds").arg(took)));
    QVERIFY2(took >= 2000, qPrintable(QStringLiteral("gave up after %1 ms, inside its own guard").arg(took)));

    // And reported as the timeout it is rather than as a cancellation: one of
    // those is something somebody did on purpose.
    QCOMPARE(response.code, CURLE_OPERATION_TIMEDOUT);
    QVERIFY2(!net::wasCancelled(response), "a stall must not read as a cancellation");
    const VfsError said = net::errorFor(response, QStringLiteral("Reading"), net::StatusMeaning::Http);
    QVERIFY2(said.message.contains(QStringLiteral("nothing arrived")), qPrintable(said.message));
}

void TestCurlTransport::aCancelTakesEffectAtOnce()
{
    // The same server, and a token already pulled. Cancellation is felt at the
    // next turn of the loop rather than whenever libcurl next asks -- so this
    // comes back in well under the guard, and well under a second.
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.status = 200;
        reply.body = QByteArray(64 * 1024, 'x');
        reply.goQuietAfter = 16 * 1024;
        reply.stayQuietMs = 30000;
        return reply;
    });
    QVERIFY2(server.start(), "could not take a port for the scripted server");

    net::TransportOptions options;
    options.stallSeconds = 30;
    net::CurlPool pool(std::move(options));

    auto lease = pool.take();
    QVERIFY(lease);
    lease.setUrl((server.url() + QStringLiteral("/quiet")).toUtf8());

    CancelToken cancel;
    cancel.cancel();

    QElapsedTimer clock;
    clock.start();
    const net::Response response = pool.perform(lease, cancel);
    QVERIFY2(clock.elapsed() < 1000,
        qPrintable(QStringLiteral("a cancelled transfer took %1 ms").arg(clock.elapsed())));
    QVERIFY2(net::wasCancelled(response), "a cancelled transfer has to read as cancelled");
}

/// The password, in the log, in plain text.
///
/// The redactor looks for `name: value` header lines, and libcurl traces FTP
/// control commands through the same channel: `> USER alice`, `> PASS s3cret`. A
/// command has no colon in it, so the line went into the log verbatim -- and
/// MOLE_LOG=curl is what somebody turns up to diagnose an FTP problem, into the
/// session log that ADR-0012 says gets sent to whoever is helping. Nothing could
/// hold this before, because the redactor was private to its own file. See
/// MOLE-349.
void TestCurlTransport::nothingWithACredentialInItReachesTheLog()
{
    // The one that was being written down.
    QCOMPARE(net::withoutSecrets("PASS hunter2"), QByteArray("PASS <redacted>"));
    QCOMPARE(net::withoutSecrets("pass hunter2"), QByteArray("pass <redacted>"));
    QCOMPARE(net::withoutSecrets("ACCT department-4"), QByteArray("ACCT <redacted>"));

    // The ones that already were.
    QCOMPARE(net::withoutSecrets("Authorization: Basic YWxpY2U6aHVudGVyMg=="),
        QByteArray("Authorization: <redacted>"));
    QCOMPARE(net::withoutSecrets("x-amz-security-token: FwoGZXIvYXdz"),
        QByteArray("x-amz-security-token: <redacted>"));

    // And everything else is left alone, because a log with the useful part
    // taken out is the other way to be useless.
    QCOMPARE(net::withoutSecrets("USER alice"), QByteArray("USER alice"));
    QCOMPARE(net::withoutSecrets("PASV"), QByteArray("PASV"));
    QCOMPARE(net::withoutSecrets("Content-Length: 12"), QByteArray("Content-Length: 12"));
    QCOMPARE(net::withoutSecrets("PASSWORD-POLICY: strict"), QByteArray("PASSWORD-POLICY: strict"));
}

MOLE_TEST_MAIN(TestCurlTransport)

#include "tst_CurlTransport.moc"
