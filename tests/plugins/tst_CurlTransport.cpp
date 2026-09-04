#include "plugins/network/CurlTransport.h"
#include "support/MoleTestMain.h"
#include "support/ScriptedHttpServer.h"

#include "core/vfs/backends/LocalFileSystem.h"

#include <QElapsedTimer>
#include <QTest>

#include <cerrno>
#include <cstring>
#include <system_error>

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

    void noCodeATransferCanReturnComesBackAsUnknown();
    void aFailingRemoteCommandSaysWhatWentWrong_data();
    void aFailingRemoteCommandSaysWhatWentWrong();
    void everyHttpStatusThatMattersHasItsOwnKind_data();
    void everyHttpStatusThatMattersHasItsOwnKind();
    void aLoopThatBrokeWithoutAnAnswerIsNotASuccess();
    void aPayloadThatCouldNotBeReadIsNotACancel();
    void onlyAFailureThatMightGoTheOtherWayIsRetried();
    void theSystemsOwnReasonsMapOntoTheVocabulary_data();
    void theSystemsOwnReasonsMapOntoTheVocabulary();

    void aSecondTransferOnOnePoolReusesTheConnection();

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
    const qint64 took = clock.elapsed();

    // **Six seconds, the same shape as the guard above.** What this rules out is
    // waiting on the stall guard or on the server's half minute of silence, both
    // of which are thirty seconds -- and the bound used to be one second, which
    // also measured a DNS lookup and a TCP connect on whatever machine is
    // running and left no room for a sanitized build. An order of magnitude
    // under the thing being ruled out is the bound; anything tighter is a
    // stopwatch on the local network. See MOLE-400.
    QVERIFY2(took < 6000,
        qPrintable(QStringLiteral("a cancelled transfer took %1 ms against a stall guard of thirty "
                                  "seconds")
                       .arg(took)));
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
/// Unknown is "the answer that breaks every one of them".
///
/// Those are the conformance suite's words, and they are literal: every caller
/// above IFileSystem branches on the code -- resolveConflict() on NotFound,
/// AlreadyExists and NotEmpty, StreamingDownload on whether to try again, the
/// sidebar on whether a drive is unreachable. A dozen CURLcodes a transfer in
/// this application can really return fell through to `default:`, the worst of
/// them CURLE_QUOTE_ERROR -- which is *every* failing SFTP mkdir, rm, rmdir and
/// rename and every failing FTP MKD, DELE, RMD, RNFR and RNTO. See MOLE-373.
void TestCurlTransport::noCodeATransferCanReturnComesBackAsUnknown()
{
    // What the six backends in this build can be handed back. Not every CURLcode
    // libcurl has: the ones a transfer this application makes can end on.
    const QList<CURLcode> reachable {
        CURLE_UNSUPPORTED_PROTOCOL,
        CURLE_FAILED_INIT,
        CURLE_URL_MALFORMAT,
        CURLE_NOT_BUILT_IN,
        CURLE_COULDNT_RESOLVE_PROXY,
        CURLE_COULDNT_RESOLVE_HOST,
        CURLE_COULDNT_CONNECT,
        CURLE_WEIRD_SERVER_REPLY,
        CURLE_REMOTE_ACCESS_DENIED,
        CURLE_FTP_ACCEPT_FAILED,
        CURLE_FTP_WEIRD_PASS_REPLY,
        CURLE_FTP_ACCEPT_TIMEOUT,
        CURLE_FTP_WEIRD_PASV_REPLY,
        CURLE_FTP_WEIRD_227_FORMAT,
        CURLE_FTP_CANT_GET_HOST,
        CURLE_HTTP2,
        CURLE_FTP_COULDNT_SET_TYPE,
        CURLE_PARTIAL_FILE,
        CURLE_FTP_COULDNT_RETR_FILE,
        CURLE_QUOTE_ERROR,
        CURLE_WRITE_ERROR,
        CURLE_UPLOAD_FAILED,
        CURLE_READ_ERROR,
        CURLE_OUT_OF_MEMORY,
        CURLE_OPERATION_TIMEDOUT,
        CURLE_FTP_PORT_FAILED,
        CURLE_FTP_COULDNT_USE_REST,
        CURLE_RANGE_ERROR,
        CURLE_HTTP_POST_ERROR,
        CURLE_SSL_CONNECT_ERROR,
        CURLE_BAD_DOWNLOAD_RESUME,
        CURLE_FILE_COULDNT_READ_FILE,
        CURLE_TOO_MANY_REDIRECTS,
        CURLE_UNKNOWN_OPTION,
        CURLE_GOT_NOTHING,
        CURLE_SSL_ENGINE_NOTFOUND,
        CURLE_SSL_ENGINE_SETFAILED,
        CURLE_SEND_ERROR,
        CURLE_RECV_ERROR,
        CURLE_SSL_CERTPROBLEM,
        CURLE_SSL_CIPHER,
        CURLE_PEER_FAILED_VERIFICATION,
        CURLE_BAD_CONTENT_ENCODING,
        CURLE_FILESIZE_EXCEEDED,
        CURLE_USE_SSL_FAILED,
        CURLE_SEND_FAIL_REWIND,
        CURLE_SSL_CACERT_BADFILE,
        CURLE_SSH,
        CURLE_SSL_SHUTDOWN_FAILED,
        CURLE_AGAIN,
        CURLE_SSL_CRL_BADFILE,
        CURLE_SSL_ISSUER_ERROR,
        CURLE_AUTH_ERROR,
        CURLE_HTTP2_STREAM,
        CURLE_REMOTE_FILE_NOT_FOUND,
        CURLE_REMOTE_DISK_FULL,
        CURLE_REMOTE_FILE_EXISTS,
        CURLE_LOGIN_DENIED,
        CURLE_INTERFACE_FAILED,
    };

    QStringList unknown;
    for (const CURLcode code : reachable) {
        net::Response response;
        response.code = code;
        response.detail = QStringLiteral("something the server said");
        const VfsError error = net::errorFor(response, QStringLiteral("Reading /x"));
        if (error.code == VfsError::Unknown)
            unknown.append(QString::number(int(code)));
        QVERIFY2(error.isError(), qPrintable(QStringLiteral("code %1 was not an error").arg(int(code))));
    }
    QVERIFY2(unknown.isEmpty(),
        qPrintable(QStringLiteral("these CURLcodes still map to Unknown: %1")
                       .arg(unknown.join(QStringLiteral(", ")))));
}

void TestCurlTransport::aFailingRemoteCommandSaysWhatWentWrong_data()
{
    QTest::addColumn<QString>("detail");
    QTest::addColumn<int>("expected");

    // What SFTP and FTP actually put in the error buffer for a failed command.
    QTest::newRow("sftp permission") << QStringLiteral("Permission denied") << int(VfsError::AccessDenied);
    QTest::newRow("ftp 550") << QStringLiteral("RMD command failed: 550 Permission denied")
                             << int(VfsError::AccessDenied);
    QTest::newRow("read-only") << QStringLiteral("mkdir failed: read-only file system")
                               << int(VfsError::AccessDenied);
    QTest::newRow("missing") << QStringLiteral("rename failed: No such file") << int(VfsError::NotFound);
    QTest::newRow("taken") << QStringLiteral("RNTO failed: 553 File already exists")
                           << int(VfsError::AccessDenied);
    QTest::newRow("not empty") << QStringLiteral("rmdir failed: Directory not empty")
                               << int(VfsError::NotEmpty);
    QTest::newRow("quota") << QStringLiteral("552 Quota exceeded") << int(VfsError::IoError);
    // And the one nobody has a phrase for: an I/O error, never Unknown.
    QTest::newRow("unrecognised") << QStringLiteral("something nobody has seen before")
                                  << int(VfsError::IoError);
}

void TestCurlTransport::aFailingRemoteCommandSaysWhatWentWrong()
{
    QFETCH(QString, detail);
    QFETCH(int, expected);

    net::Response response;
    response.code = CURLE_QUOTE_ERROR;
    response.detail = detail;
    const VfsError error
        = net::errorFor(response, QStringLiteral("Removing /x"), net::StatusMeaning::ProtocolReply);
    QCOMPARE(int(error.code), expected);
    // The server's own words travel: "it failed" is not something anybody can
    // act on.
    QVERIFY2(error.message.contains(detail), qPrintable(error.message));
}

void TestCurlTransport::everyHttpStatusThatMattersHasItsOwnKind_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<int>("expected");

    QTest::newRow("200") << 200 << int(VfsError::None);
    QTest::newRow("206") << 206 << int(VfsError::None);
    // Past the end of the object, which is what the end of a file looks like to
    // a reader asking span by span.
    QTest::newRow("416") << 416 << int(VfsError::None);
    QTest::newRow("401") << 401 << int(VfsError::AccessDenied);
    QTest::newRow("403") << 403 << int(VfsError::AccessDenied);
    QTest::newRow("404") << 404 << int(VfsError::NotFound);
    QTest::newRow("405") << 405 << int(VfsError::NotSupported);
    QTest::newRow("409") << 409 << int(VfsError::NotFound);
    // A condition the caller never set.
    QTest::newRow("412 without one") << 412 << int(VfsError::AccessDenied);
    QTest::newRow("423") << 423 << int(VfsError::AccessDenied);
    QTest::newRow("413") << 413 << int(VfsError::IoError);
    QTest::newRow("507") << 507 << int(VfsError::IoError);
    QTest::newRow("501") << 501 << int(VfsError::NotSupported);
    // The server is there and cannot serve this now. Not a disk error: these are
    // the same conditions as a socket that would not open, and every one of them
    // used to be "the server answered 503" -- which the sidebar and the copy
    // layer both read as something wrong with the storage.
    QTest::newRow("408") << 408 << int(VfsError::NetworkError);
    QTest::newRow("429") << 429 << int(VfsError::NetworkError);
    QTest::newRow("502") << 502 << int(VfsError::NetworkError);
    QTest::newRow("503") << 503 << int(VfsError::NetworkError);
    QTest::newRow("504") << 504 << int(VfsError::NetworkError);
    // Anything else is still an I/O error, which is the honest last resort.
    QTest::newRow("418") << 418 << int(VfsError::IoError);
}

void TestCurlTransport::everyHttpStatusThatMattersHasItsOwnKind()
{
    QFETCH(int, status);
    QFETCH(int, expected);

    net::Response response;
    response.code = CURLE_OK;
    response.status = status;
    const VfsError error = net::errorFor(response, QStringLiteral("Reading /x"));
    QCOMPARE(int(error.code), expected);

    // 412 is the one that depends on what was asked: with `Overwrite: F` it is
    // the destination existing, which is what WebDAV's MOVE sends.
    if (status == 412) {
        const VfsError asked = net::errorFor(
            response, QStringLiteral("Renaming /x"), net::StatusMeaning::Http, net::Precondition::Sent);
        QCOMPARE(asked.code, VfsError::AlreadyExists);
    }
}

/// A loop that ended without an answer used to answer "fine".
///
/// perform() stored curl_multi_strerror()'s text in `detail` and broke, and
/// `code` is only ever set from a CURLMSG_DONE that never came -- so it stayed
/// CURLE_OK, this said ok(), and the half-driven connection went back to the
/// pool. A sendSpan() that hit it would have committed a partial write of a file
/// that was never sent. See MOLE-373.
void TestCurlTransport::aLoopThatBrokeWithoutAnAnswerIsNotASuccess()
{
    net::Response response;
    response.code = CURLE_OK;
    response.status = 0;
    response.detail = QStringLiteral("curl_multi_perform: internal error");

    const VfsError error
        = net::errorFor(response, QStringLiteral("Writing /x"), net::StatusMeaning::ProtocolReply);
    QVERIFY2(error.isError(), "a transfer that never finished was reported as having worked");
    QCOMPARE(error.code, VfsError::IoError);
    QVERIFY2(error.message.contains(QStringLiteral("internal error")), qPrintable(error.message));

    // And a transfer that really did finish with nothing to say is still fine:
    // an empty detail is the ordinary case.
    net::Response fine;
    fine.code = CURLE_OK;
    QVERIFY(!net::errorFor(fine, QStringLiteral("Writing /x"), net::StatusMeaning::ProtocolReply).isError());
}

/// "Writing X was cancelled" when nobody cancelled anything.
///
/// A read callback can only abort, and libcurl reports that as
/// CURLE_ABORTED_BY_CALLBACK -- the same code as the user pressing Cancel. So a
/// staging-disk read error part way through an upload was reported to the user
/// as their own cancellation. See MOLE-373.
void TestCurlTransport::aPayloadThatCouldNotBeReadIsNotACancel()
{
    /// A device that opens, answers one read, and then fails.
    class FailsPartWay : public QIODevice
    {
    public:
        FailsPartWay() { open(QIODevice::ReadOnly); }
        bool isSequential() const override { return true; }

    protected:
        qint64 readData(char* data, qint64 maxSize) override
        {
            if (m_served >= 16) {
                setErrorString(QStringLiteral("the staging file could not be read"));
                return -1;
            }
            const qint64 give = qMin<qint64>(maxSize, 16);
            std::memset(data, 'x', size_t(give));
            m_served += give;
            return give;
        }
        qint64 writeData(const char*, qint64) override { return -1; }

    private:
        qint64 m_served = 0;
    };

    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.status = 200;
        return reply;
    });
    QVERIFY2(server.start(), "could not take a port for the scripted server");

    net::TransportOptions options;
    net::CurlPool pool(options);
    net::CurlPool::Lease lease = pool.take();
    QVERIFY(lease);
    lease.setUrl((server.url() + QStringLiteral("/upload")).toUtf8());

    FailsPartWay payload;
    pool.sendFrom(lease, payload, -1);
    const net::Response response = pool.perform(lease, CancelToken());

    const VfsError error = net::errorFor(response, QStringLiteral("Writing /x"));
    QVERIFY(error.isError());
    QVERIFY2(error.code != VfsError::Cancelled, "a read error was reported as the user's cancellation");
    QCOMPARE(error.code, VfsError::IoError);
    QVERIFY2(error.message.contains(QStringLiteral("could not be read")), qPrintable(error.message));
}

/// Which failures are worth another go.
///
/// StreamingDownload retried any non-cancel failure for the whole of its budget
/// with the kind unread, so a 403 whose credentials had expired mid-copy, a 404
/// for an object deleted between spans and a NotSupported each waited two
/// minutes per file before saying what the first attempt already knew.
/// See MOLE-373.
void TestCurlTransport::onlyAFailureThatMightGoTheOtherWayIsRetried()
{
    QVERIFY(net::isWorthRetrying(VfsError::make(VfsError::NetworkError, QStringLiteral("x"))));
    QVERIFY(net::isWorthRetrying(VfsError::make(VfsError::IoError, QStringLiteral("x"))));
    QVERIFY(net::isWorthRetrying(VfsError::make(VfsError::Unknown, QStringLiteral("x"))));

    QVERIFY(!net::isWorthRetrying(VfsError::make(VfsError::AccessDenied, QStringLiteral("x"))));
    QVERIFY(!net::isWorthRetrying(VfsError::make(VfsError::NotFound, QStringLiteral("x"))));
    QVERIFY(!net::isWorthRetrying(VfsError::make(VfsError::NotSupported, QStringLiteral("x"))));
    QVERIFY(!net::isWorthRetrying(VfsError::make(VfsError::AlreadyExists, QStringLiteral("x"))));
    QVERIFY(!net::isWorthRetrying(VfsError::make(VfsError::NotEmpty, QStringLiteral("x"))));
    QVERIFY(!net::isWorthRetrying(VfsError::make(VfsError::Cancelled, QStringLiteral("x"))));
    QVERIFY(!net::isWorthRetrying(VfsError::ok()));
}

void TestCurlTransport::theSystemsOwnReasonsMapOntoTheVocabulary_data()
{
    QTest::addColumn<int>("errnoValue");
    QTest::addColumn<int>("expected");

    QTest::newRow("ENOENT") << int(ENOENT) << int(VfsError::NotFound);
    QTest::newRow("EACCES") << int(EACCES) << int(VfsError::AccessDenied);
    QTest::newRow("EPERM") << int(EPERM) << int(VfsError::AccessDenied);
    QTest::newRow("ENOTEMPTY") << int(ENOTEMPTY) << int(VfsError::NotEmpty);
    QTest::newRow("EISDIR") << int(EISDIR) << int(VfsError::IsADirectory);
    QTest::newRow("ENOTDIR") << int(ENOTDIR) << int(VfsError::NotADirectory);
    QTest::newRow("EEXIST") << int(EEXIST) << int(VfsError::AlreadyExists);
    // Not a fault: the kernel saying this call cannot express the operation,
    // which every caller with a slower way round is already watching for.
    QTest::newRow("EXDEV") << int(EXDEV) << int(VfsError::NotSupported);
    QTest::newRow("EIO") << int(EIO) << int(VfsError::IoError);
    QTest::newRow("ENOSPC") << int(ENOSPC) << int(VfsError::IoError);
}

void TestCurlTransport::theSystemsOwnReasonsMapOntoTheVocabulary()
{
    QFETCH(int, errnoValue);
    QFETCH(int, expected);
    const std::error_code failed(errnoValue, std::generic_category());
    QCOMPARE(int(LocalFileSystem::codeForSystemError(failed)), expected);
}

/// The pool kept no connections at all.
///
/// In libcurl the connection cache belongs to the *multi* handle:
/// curl_multi_add_handle() points the easy handle's cache at the multi's, and
/// curl_multi_cleanup() closes every connection in it. perform() makes a multi
/// per transfer -- deliberately, so the decision to stop is ours (ADR-0049) --
/// so the connection each transfer used was closed before the lease came back
/// and the idle handles carried nothing. The class comment promised "a warm
/// connection to come back to" and ARCHITECTURE.md said the pool "keeps
/// libcurl's connection cache, which is what stops an SFTP drive renegotiating
/// SSH for every listing"; neither was true. At 0.58 s a handshake (ADR-0013)
/// and four or five handshakes per small SFTP upload, that is what a folder of
/// ten thousand files cost. See MOLE-369.
void TestCurlTransport::aSecondTransferOnOnePoolReusesTheConnection()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = "hello";
        // A server that closed every connection would make this test pass or
        // fail for its own reasons rather than the pool's.
        reply.keepAlive = true;
        return reply;
    });
    QVERIFY2(server.start(), "could not take a port for the scripted server");

    net::TransportOptions options;
    net::CurlPool pool(options);

    const auto fetch = [&] {
        net::CurlPool::Lease lease = pool.take();
        Q_ASSERT(lease);
        lease.setUrl((server.url() + QStringLiteral("/thing")).toUtf8());
        return pool.perform(lease, CancelToken());
    };

    const net::Response first = fetch();
    QVERIFY2(!net::errorFor(first, QStringLiteral("Reading /thing")).isError(), "the first fetch failed");
    QCOMPARE(first.connectionsOpened, 1L);

    // The second one must find the first one's connection still there. This is
    // the whole assertion: it read 1 before the share handle existed, on every
    // transfer, including a second listing of the same directory.
    const net::Response second = fetch();
    QVERIFY2(!net::errorFor(second, QStringLiteral("Reading /thing")).isError(), "the second fetch failed");
    QCOMPARE(second.connectionsOpened, 0L);

    // And a third, because "the second reuses it" could be true of a cache that
    // holds exactly one transfer's worth.
    QCOMPARE(fetch().connectionsOpened, 0L);

    // A pool of its own starts cold, which is what says the cache belongs to the
    // pool rather than to the process.
    net::CurlPool other(options);
    net::CurlPool::Lease lease = other.take();
    QVERIFY(lease);
    lease.setUrl((server.url() + QStringLiteral("/thing")).toUtf8());
    QCOMPARE(other.perform(lease, CancelToken()).connectionsOpened, 1L);
}

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
