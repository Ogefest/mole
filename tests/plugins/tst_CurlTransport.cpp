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

MOLE_TEST_MAIN(TestCurlTransport)

#include "tst_CurlTransport.moc"
