#include "plugins/network/CurlTransport.h"
#include "support/MoleTestMain.h"

#include <QTest>

using namespace mole;

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

MOLE_TEST_MAIN(TestCurlTransport)

#include "tst_CurlTransport.moc"
