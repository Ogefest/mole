#include "plugins/network/S3FileSystem.h"
#include "plugins/network/WebdavFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/ScriptedHttpServer.h"

#include <QTest>

using namespace mole;
using namespace mole::test;

namespace {

/// A well-formed answer to PROPFIND, so a test can cut it about and see what
/// the backend makes of the pieces.
QByteArray multistatusFor(const QStringList& names)
{
    QByteArray xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
  <d:response><d:href>/dav/</d:href><d:propstat><d:prop>
    <d:resourcetype><d:collection/></d:resourcetype>
  </d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>
)";
    for (const QString& name : names) {
        xml += "  <d:response><d:href>/dav/" + name.toUtf8() + R"(</d:href><d:propstat><d:prop>
    <d:resourcetype/><d:getcontentlength>10</d:getcontentlength>
  </d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>
)";
    }
    xml += "</d:multistatus>";
    return xml;
}

WebdavSettings webdavAgainst(const ScriptedHttpServer& server)
{
    WebdavSettings settings;
    settings.baseUrl = server.url() + QStringLiteral("/dav");
    settings.username = QStringLiteral("someone");
    settings.password = QStringLiteral("secret");
    return settings;
}

S3Settings s3Against(const ScriptedHttpServer& server)
{
    S3Settings settings;
    settings.accessKeyId = QStringLiteral("key");
    settings.secretAccessKey = QStringLiteral("secret");
    settings.endpoint = server.url().mid(QStringLiteral("http://").size());
    settings.bucket = QStringLiteral("bucket");
    settings.pathStyleAddressing = true;
    settings.useHttps = false;
    return settings;
}

QByteArray payloadOf(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 13 + (i >> 6)) & 0xff);
    return data;
}

} // namespace

/// What a server that answers wrongly does to a file manager.
///
/// Every case here is a real answer some server gives — a 411 to a chunked PUT,
/// a `Content-Length` that does not match the body, a listing cut off half way
/// through, a redirect aimed at a request carrying a file. None of them can be
/// obtained from a server that is behaving, which is why they are scripted; and
/// because they are scripted they run offline in milliseconds, on every change.
///
/// The loss each one prevents is in its name.
class TestServersThatMisbehave : public QObject
{
    Q_OBJECT

private slots:
    void aListingCutOffMidDocumentIsAnErrorNotAShortDirectory();
    void aFileCutOffMidTransferIsAnErrorNotAShortFile();
    void aChunkedPutRefusedWith411SaysSoRatherThanHanging();
    void aRedirectAnsweredToAPutDoesNotSendTheFileSomewhereElse();
    void aSlowDownFromS3IsReportedRatherThanLosingTheUploadInSilence();
    void anErrorDocumentInsideA200IsStillAFailure();
};

/// The one that would delete a user's files.
///
/// A mirror sync asks what is on the far side and removes anything that is not.
/// A listing that arrives half-finished and is reported as a short directory is
/// therefore not a display problem — it is the instruction to delete everything
/// the answer was cut off before mentioning.
void TestServersThatMisbehave::aListingCutOffMidDocumentIsAnErrorNotAShortDirectory()
{
    const QByteArray whole = multistatusFor({ QStringLiteral("one.txt"), QStringLiteral("two.txt"),
        QStringLiteral("three.txt"), QStringLiteral("four.txt") });

    ScriptedHttpServer server([&whole](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.status = 207;
        reply.reason = "Multi-Status";
        reply.headers.append("Content-Type: application/xml; charset=utf-8");
        reply.body = whole;
        // The length is honest and the delivery is not: the connection dies
        // after two thirds of the document, which is what a server restarting
        // or a proxy giving up actually looks like.
        reply.hangUpAfter = whole.size() * 2 / 3;
        return reply;
    });
    QVERIFY(server.start());

    WebdavFileSystem fileSystem(QStringLiteral("webdav"), webdavAgainst(server));
    const Result<FileEntryList> listing
        = fileSystem.list(VfsUri(QStringLiteral("webdav"), QString(), QStringLiteral("/")), CancelToken());

    QVERIFY2(!listing.ok(),
        qPrintable(QStringLiteral("a truncated listing was reported as a directory of %1 entries")
                       .arg(listing.ok() ? listing.value().size() : 0)));
    QVERIFY2(!listing.error().message.isEmpty(), "a failure nobody can read is a failure nobody can act on");
}

void TestServersThatMisbehave::aFileCutOffMidTransferIsAnErrorNotAShortFile()
{
    const QByteArray payload = payloadOf(200 * 1024);

    ScriptedHttpServer server([&payload](const ScriptedHttpServer::Request& request) {
        ScriptedHttpServer::Reply reply;
        if (request.method == "PROPFIND") {
            reply.status = 207;
            reply.reason = "Multi-Status";
            reply.body = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:"><d:response><d:href>/dav/big.bin</d:href>
<d:propstat><d:prop><d:resourcetype/><d:getcontentlength>204800</d:getcontentlength></d:prop>
<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>)";
            return reply;
        }
        reply.body = payload;
        // Claims the whole file and delivers half of it. A caller that believes
        // the bytes it was handed writes half a file and reports success.
        reply.hangUpAfter = payload.size() / 2;
        return reply;
    });
    QVERIFY(server.start());

    WebdavFileSystem fileSystem(QStringLiteral("webdav"), webdavAgainst(server));
    const VfsUri target(QStringLiteral("webdav"), QString(), QStringLiteral("/big.bin"));

    Result<std::unique_ptr<QIODevice>> opened = fileSystem.openRead(target, payload.size());
    if (!opened.ok()) {
        // Refused outright is a perfectly good answer, and the one worth having.
        QVERIFY(!opened.error().message.isEmpty());
        return;
    }

    // Or it opened, and then the short read has to be visible in what comes
    // back rather than passing for the end of the file.
    const QByteArray got = opened.value()->readAll();
    QVERIFY2(got.size() != payload.size(), "sanity: the server only sent half");
    QVERIFY2(got.size() < payload.size(), "more arrived than was sent");
    QVERIFY2(!opened.value()->errorString().isEmpty() || got.isEmpty(),
        qPrintable(QStringLiteral("handed over %1 of %2 bytes and reported nothing")
                       .arg(got.size())
                       .arg(payload.size())));
}

/// The known risk of the streaming write, met head on.
///
/// A write too large to stage has to go out with a chunked transfer encoding,
/// and a WebDAV server is entitled to answer 411 and demand a length. There is
/// no fixing that from this side — but a refusal has to arrive as a refusal,
/// with the status in it, rather than as a hang or as a success.
void TestServersThatMisbehave::aChunkedPutRefusedWith411SaysSoRatherThanHanging()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request& request) {
        ScriptedHttpServer::Reply reply;
        if (request.method == "PUT") {
            reply.status = 411;
            reply.reason = "Length Required";
            // And it does not read the body first, which is the whole point of
            // answering 411: the client is being stopped before it sends.
            reply.readRequestBody = false;
            return reply;
        }
        reply.status = 404;
        reply.reason = "Not Found";
        return reply;
    });
    QVERIFY(server.start());

    WebdavFileSystem fileSystem(QStringLiteral("webdav"), webdavAgainst(server));
    const VfsUri target(QStringLiteral("webdav"), QString(), QStringLiteral("/big.bin"));

    // Above the staging threshold, so this takes the chunked route.
    Result<std::unique_ptr<QIODevice>> opened = fileSystem.openWrite(target, 128LL * 1024 * 1024);
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));

    const QByteArray block = payloadOf(256 * 1024);
    for (int i = 0; i < 8; ++i)
        opened.value()->write(block);

    const Result<void> written = closeAndReport(*opened.value());

    QVERIFY2(!written.ok(), "a 411 was reported as a successful upload");
    QVERIFY2(written.error().message.contains(QStringLiteral("411"))
            || written.error().message.contains(QStringLiteral("Length")),
        qPrintable(QStringLiteral("the refusal has to be diagnosable, and this says: %1")
                       .arg(written.error().message)));
}

/// A body is not silently replayed to somewhere else.
///
/// A redirect answered to a request carrying a file is the shape of both an
/// accident and an attack: the file goes to whatever host the answer names. The
/// only safe answer is to not follow it.
void TestServersThatMisbehave::aRedirectAnsweredToAPutDoesNotSendTheFileSomewhereElse()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request& request) {
        ScriptedHttpServer::Reply reply;
        if (request.method == "PUT" && !request.path.contains("elsewhere")) {
            reply.status = 302;
            reply.reason = "Found";
            reply.headers.append("Location: /dav/elsewhere/moved.bin");
            return reply;
        }
        reply.status = 201;
        reply.reason = "Created";
        return reply;
    });
    QVERIFY(server.start());

    WebdavFileSystem fileSystem(QStringLiteral("webdav"), webdavAgainst(server));
    const VfsUri target(QStringLiteral("webdav"), QString(), QStringLiteral("/secret.bin"));

    Result<std::unique_ptr<QIODevice>> opened = fileSystem.openWrite(target, 1024);
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));
    opened.value()->write(payloadOf(1024));
    const Result<void> written = closeAndReport(*opened.value());

    QVERIFY2(!written.ok(), "a redirect was treated as a completed upload");

    for (const ScriptedHttpServer::Request& seen : server.received()) {
        QVERIFY2(!seen.path.contains("elsewhere"),
            "the file was sent to the address the redirect named, which is where it must never go");
    }
}

/// What happens on a throttle, said out loud.
///
/// `503 SlowDown` is routine on S3 under load rather than exceptional. Nothing
/// retries it today, and this test exists to say so in a way that will notice
/// when that changes: an upload dies, and it dies with the reason in it rather
/// than as a mystery.
void TestServersThatMisbehave::aSlowDownFromS3IsReportedRatherThanLosingTheUploadInSilence()
{
    std::atomic<int> parts { 0 };

    ScriptedHttpServer server([&parts](const ScriptedHttpServer::Request& request) {
        ScriptedHttpServer::Reply reply;
        if (request.method == "POST" && request.path.contains("uploads")) {
            reply.headers.append("Content-Type: application/xml");
            reply.body = "<InitiateMultipartUploadResult><UploadId>an-upload</UploadId>"
                         "</InitiateMultipartUploadResult>";
            return reply;
        }
        if (request.method == "PUT" && request.path.contains("partNumber")) {
            if (++parts >= 3) {
                reply.status = 503;
                reply.reason = "Service Unavailable";
                reply.body = "<Error><Code>SlowDown</Code><Message>Please reduce your request rate."
                             "</Message></Error>";
                return reply;
            }
            reply.headers.append("ETag: \"an-etag\"");
            return reply;
        }
        reply.headers.append("ETag: \"an-etag\"");
        return reply;
    });
    QVERIFY(server.start());

    S3FileSystem fileSystem(QStringLiteral("s3"), s3Against(server));
    const VfsUri target(QStringLiteral("s3"), QString(), QStringLiteral("/big.bin"));

    // Large enough to go up in parts, so there is a third part to be throttled.
    Result<std::unique_ptr<QIODevice>> opened = fileSystem.openWrite(target, 256LL * 1024 * 1024);
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));

    const QByteArray block = payloadOf(4 * 1024 * 1024);
    for (int i = 0; i < 48 && opened.value()->write(block) > 0; ++i) { }
    const Result<void> written = closeAndReport(*opened.value());

    QVERIFY2(parts >= 3, "the upload never got as far as a third part, so nothing was throttled");
    QVERIFY2(!written.ok(), "a throttled part was reported as a finished upload");
    QVERIFY2(!written.error().message.isEmpty(), "a failure nobody can read is a failure nobody can act on");
}

/// The one place a status code cannot be trusted.
///
/// S3 answers CompleteMultipartUpload with 200 and puts the failure in the
/// body, so a backend that reads the status and stops reports an upload that
/// does not exist. Already handled; this is what keeps it handled.
void TestServersThatMisbehave::anErrorDocumentInsideA200IsStillAFailure()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request& request) {
        ScriptedHttpServer::Reply reply;
        if (request.method == "POST" && request.path.contains("uploads")) {
            reply.body = "<InitiateMultipartUploadResult><UploadId>an-upload</UploadId>"
                         "</InitiateMultipartUploadResult>";
            return reply;
        }
        if (request.method == "POST" && request.path.contains("uploadId")) {
            // 200, and a failure inside it.
            reply.body = "<Error><Code>InternalError</Code><Message>We encountered an internal error."
                         "</Message></Error>";
            return reply;
        }
        reply.headers.append("ETag: \"an-etag\"");
        return reply;
    });
    QVERIFY(server.start());

    S3FileSystem fileSystem(QStringLiteral("s3"), s3Against(server));
    const VfsUri target(QStringLiteral("s3"), QString(), QStringLiteral("/big.bin"));

    Result<std::unique_ptr<QIODevice>> opened = fileSystem.openWrite(target, 128LL * 1024 * 1024);
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));

    const QByteArray block = payloadOf(4 * 1024 * 1024);
    for (int i = 0; i < 20 && opened.value()->write(block) > 0; ++i) { }
    const Result<void> written = closeAndReport(*opened.value());

    QVERIFY2(!written.ok(), "a 200 carrying an error document was reported as a finished upload");
    QVERIFY2(written.error().message.contains(QStringLiteral("InternalError"))
            || written.error().message.contains(QStringLiteral("internal error")),
        qPrintable(QStringLiteral("what the server said has to reach the user, and this says: %1")
                       .arg(written.error().message)));
}

MOLE_TEST_MAIN(TestServersThatMisbehave)

#include "tst_ServersThatMisbehave.moc"
