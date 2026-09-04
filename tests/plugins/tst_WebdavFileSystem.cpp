#include "plugins/network/TransferStreams.h"
#include "plugins/network/WebdavFileSystem.h"
#include "plugins/network/WebdavListing.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/ScriptedHttpServer.h"

#include "core/vfs/PathWords.h"

#include <QUrl>

#include <algorithm>
#include <cstring>
#include <curl/curl.h>

using namespace mole;
using namespace mole::net;
using namespace mole::test;

namespace {

struct Account
{
    QString url;
    QString user;
    QString password;

    bool isConfigured() const { return !url.isEmpty(); }
};

Account accountFromEnvironment()
{
    const auto value = [](const char* name) { return QString::fromLocal8Bit(qgetenv(name)); };

    Account account;
    account.url = value("MOLE_TEST_WEBDAV_URL");
    account.user = value("MOLE_TEST_WEBDAV_USER");
    account.password = value("MOLE_TEST_WEBDAV_PASS");
    return account;
}

/// Seeds through plain libcurl, independently of the backend under test.
class RawWebdav
{
public:
    explicit RawWebdav(const Account& account)
        : m_account(account)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    bool request(
        const QByteArray& method, const QString& relative, const QByteArray& payload = QByteArray()) const
    {
        CURL* handle = curl_easy_init();
        if (!handle)
            return false;

        const QByteArray url = (m_account.url + QLatin1Char('/') + relative).toUtf8();
        Payload body { payload, 0 };

        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_USERNAME, m_account.user.toUtf8().constData());
        curl_easy_setopt(handle, CURLOPT_PASSWORD, m_account.password.toUtf8().constData());
        // Basic, and not CURLAUTH_ANY. Anything else makes curl probe with no
        // credentials, take the 401 and retry -- and a PUT cannot be retried,
        // because the read callback has already been drained and there is no
        // seek function to rewind it. MKCOL has no body and so worked, which is
        // exactly the shape this fault presented in: the working collection was
        // created and then nothing could be put in it.
        curl_easy_setopt(handle, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

        if (method == "PUT") {
            curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(handle, CURLOPT_READFUNCTION, readPayload);
            curl_easy_setopt(handle, CURLOPT_READDATA, &body);
            curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(payload.size()));
        } else {
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.constData());
        }

        const bool performed = curl_easy_perform(handle) == CURLE_OK;
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(handle);
        return performed && status >= 200 && status < 300;
    }

private:
    struct Payload
    {
        QByteArray data;
        qint64 offset = 0;
    };

    static size_t readPayload(char* buffer, size_t size, size_t count, void* userData)
    {
        auto* payload = static_cast<Payload*>(userData);
        const qint64 remaining = payload->data.size() - payload->offset;
        const qint64 wanted = std::min<qint64>(static_cast<qint64>(size * count), remaining);
        if (wanted <= 0)
            return 0;
        memcpy(buffer, payload->data.constData() + payload->offset, static_cast<size_t>(wanted));
        payload->offset += wanted;
        return static_cast<size_t>(wanted);
    }

    static size_t discard(char*, size_t size, size_t count, void*) { return size * count; }

    Account m_account;
};

/// A megabyte of content that depends on where it sits in the file.
///
/// Repeated zeroes would prove only that the right number of bytes arrived;
/// content that differs block by block also proves they arrived in the right
/// order and none was skipped over.
constexpr int kBlockSize = 1024 * 1024;

QByteArray blockAt(int index)
{
    QByteArray block(kBlockSize, Qt::Uninitialized);
    for (int i = 0; i < kBlockSize; ++i)
        block[i] = static_cast<char>((i * 31 + index * 17) & 0xff);
    return block;
}

} // namespace

class TestWebdavFileSystem : public QObject
{
    Q_OBJECT

private slots:
    void aNextcloudAnswerIsRead();
    void anHttpDateIsReadWhateverWeekdayItClaims();
    void anyNamespacePrefixIsAccepted();
    void aFileAnswersWithOnlyItself();
    void aFailedPropstatDoesNotHideAGoodOne();
    void aDeleteTheServerOnlyHalfDidIsAFailureThatNamesTheMember();
    void aMoveRefusedWithA409IsAMissingParentAndNotANameClash();
    void aCollectionAnsweringUnderAnAliasDoesNotListItselfAsItsOwnChild();
    void aSizeTheServerDidNotGiveIsUnknownAndNotZero();
    void anAbsoluteUrlHrefIsReducedToItsPath();
    void anEncodedHrefIsDecoded();
    void somethingThatIsNotAMultistatusIsRejected();
    void pathHelpersBehave();
    void theBaseUrlIsSplitIntoOriginAndPath();
    void anAddressThatIsNotAUrlIsRefused();
    void itSatisfiesTheConformanceSuite();
    void aLargeFileGoesUpWholeThroughAChunkedPut();
    void aLargeFileIsStreamedInRangesRatherThanDownloadedWhole();
    void aStagedWriteTooBigToBufferAlsoArrives();
};

void TestWebdavFileSystem::aNextcloudAnswerIsRead()
{
    // Shaped like what Nextcloud actually returns for PROPFIND Depth: 1.
    const QByteArray xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
  <d:response>
    <d:href>/remote.php/dav/files/lukasz/work/</d:href>
    <d:propstat>
      <d:prop>
        <d:resourcetype><d:collection/></d:resourcetype>
        <d:getlastmodified>Sun, 09 Aug 2026 08:54:12 GMT</d:getlastmodified>
      </d:prop>
      <d:status>HTTP/1.1 200 OK</d:status>
    </d:propstat>
  </d:response>
  <d:response>
    <d:href>/remote.php/dav/files/lukasz/work/notes.txt</d:href>
    <d:propstat>
      <d:prop>
        <d:resourcetype/>
        <d:getcontentlength>1234</d:getcontentlength>
        <d:getlastmodified>Sun, 09 Aug 2026 09:15:00 GMT</d:getlastmodified>
      </d:prop>
      <d:status>HTTP/1.1 200 OK</d:status>
    </d:propstat>
  </d:response>
</d:multistatus>)";

    QList<WebdavEntry> entries;
    QString error;
    QVERIFY2(parseMultistatus(xml, &entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), 2);

    QVERIFY(entries.at(0).isCollection);
    QCOMPARE(entries.at(0).path, QStringLiteral("/remote.php/dav/files/lukasz/work/"));

    QVERIFY(!entries.at(1).isCollection);
    QCOMPARE(entries.at(1).size, 1234);
    QCOMPARE(nameFromPath(entries.at(1).path), QStringLiteral("notes.txt"));
    QCOMPARE(entries.at(1).modified.date(), QDate(2026, 8, 9));
    QCOMPARE(entries.at(1).status, 200);
}

void TestWebdavFileSystem::anHttpDateIsReadWhateverWeekdayItClaims()
{
    // getlastmodified is an HTTP date: it ends in "GMT", which Qt's RFC 2822
    // reader rejects outright, and it starts with a weekday, which Qt validates
    // against the date. Both had to be dealt with -- and the weekday is simply
    // dropped, because a server whose arithmetic is a day out would otherwise
    // have its timestamps thrown away rather than being slightly wrong.
    const auto dateOf = [](const char* modified) {
        const QByteArray xml = QByteArray(R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:"><d:response><d:href>/dav/a.txt</d:href>
<d:propstat><d:prop><d:getlastmodified>)")
            + modified + R"(</d:getlastmodified></d:prop>
<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>)";
        QList<WebdavEntry> entries;
        QString error;
        if (!parseMultistatus(xml, &entries, &error) || entries.isEmpty())
            return QDateTime();
        return entries.first().modified;
    };

    // 9 August 2026 really is a Sunday.
    QCOMPARE(dateOf("Sun, 09 Aug 2026 08:54:12 GMT").toUTC().date(), QDate(2026, 8, 9));
    // The same instant, asserted wrongly as a Saturday, still parses.
    QCOMPARE(dateOf("Sat, 09 Aug 2026 08:54:12 GMT").toUTC().date(), QDate(2026, 8, 9));
    // And with no weekday at all, or a numeric offset instead of GMT.
    QCOMPARE(dateOf("09 Aug 2026 08:54:12 GMT").toUTC().date(), QDate(2026, 8, 9));
    QCOMPARE(dateOf("Sun, 09 Aug 2026 08:54:12 +0000").toUTC().date(), QDate(2026, 8, 9));
    QCOMPARE(dateOf("2026-08-09T08:54:12Z").toUTC().date(), QDate(2026, 8, 9));
}

void TestWebdavFileSystem::anyNamespacePrefixIsAccepted()
{
    // Servers disagree about the prefix for the DAV namespace. A parser that
    // matched on it would work against one server and silently return nothing
    // for the next, which is the worst kind of failure: an empty folder.
    const QByteArray withLp = R"(<?xml version="1.0"?>
<lp1:multistatus xmlns:lp1="DAV:">
  <lp1:response><lp1:href>/dav/a.txt</lp1:href>
    <lp1:propstat><lp1:prop><lp1:getcontentlength>7</lp1:getcontentlength></lp1:prop>
    <lp1:status>HTTP/1.1 200 OK</lp1:status></lp1:propstat>
  </lp1:response>
</lp1:multistatus>)";
    const QByteArray withNone = R"(<?xml version="1.0"?>
<multistatus xmlns="DAV:">
  <response><href>/dav/a.txt</href>
    <propstat><prop><getcontentlength>7</getcontentlength></prop>
    <status>HTTP/1.1 200 OK</status></propstat>
  </response>
</multistatus>)";

    for (const QByteArray& xml : { withLp, withNone }) {
        QList<WebdavEntry> entries;
        QString error;
        QVERIFY2(parseMultistatus(xml, &entries, &error), qPrintable(error));
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().size, 7);
        QCOMPARE(entries.first().path, QStringLiteral("/dav/a.txt"));
    }
}

void TestWebdavFileSystem::aFileAnswersWithOnlyItself()
{
    // PROPFIND Depth: 1 on a file returns one response -- the file. That single
    // entry is the only signal that the target is not a directory, so losing it
    // would show a file as an empty folder.
    const QByteArray xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
  <d:response><d:href>/dav/notes.txt</d:href>
    <d:propstat><d:prop><d:resourcetype/><d:getcontentlength>5</d:getcontentlength></d:prop>
    <d:status>HTTP/1.1 200 OK</d:status></d:propstat>
  </d:response>
</d:multistatus>)";

    QList<WebdavEntry> entries;
    QString error;
    QVERIFY2(parseMultistatus(xml, &entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), 1);
    QVERIFY(!entries.first().isCollection);
}

void TestWebdavFileSystem::aFailedPropstatDoesNotHideAGoodOne()
{
    // Apache mod_dav sends two propstat blocks: 200 for the properties it has and
    // 404 for the ones it does not. Taking the last one would report every entry
    // as missing.
    const QByteArray xml = R"(<?xml version="1.0"?>
<D:multistatus xmlns:D="DAV:">
  <D:response><D:href>/dav/a.txt</D:href>
    <D:propstat><D:prop><D:getcontentlength>3</D:getcontentlength></D:prop>
      <D:status>HTTP/1.1 200 OK</D:status></D:propstat>
    <D:propstat><D:prop><D:getcontenttype/></D:prop>
      <D:status>HTTP/1.1 404 Not Found</D:status></D:propstat>
  </D:response>
</D:multistatus>)";

    QList<WebdavEntry> entries;
    QString error;
    QVERIFY2(parseMultistatus(xml, &entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().status, 200);
    QCOMPARE(entries.first().size, 3);
}

/// A size that is absent, empty or not a number is *unknown*, and zero is a
/// file of nought bytes.
///
/// getcontentlength is optional in RFC 4918 and servers leave it out for
/// generated resources. Reading it with a bare toLongLong() gave 0 for all
/// three, and 0 is what switches TransferTask's short-read guard off -- so the
/// files whose length the server would not state were exactly the ones copied
/// with no check that the whole of them arrived. See MOLE-344.
void TestWebdavFileSystem::aSizeTheServerDidNotGiveIsUnknownAndNotZero()
{
    const QByteArray xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
  <d:response>
    <d:href>/dav/generated.csv</d:href>
    <d:propstat><d:prop><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>
  </d:response>
  <d:response>
    <d:href>/dav/nonsense.bin</d:href>
    <d:propstat><d:prop><d:getcontentlength>n/a</d:getcontentlength></d:prop>
      <d:status>HTTP/1.1 200 OK</d:status></d:propstat>
  </d:response>
  <d:response>
    <d:href>/dav/empty.txt</d:href>
    <d:propstat><d:prop><d:getcontentlength>0</d:getcontentlength></d:prop>
      <d:status>HTTP/1.1 200 OK</d:status></d:propstat>
  </d:response>
</d:multistatus>)";

    QList<WebdavEntry> entries;
    QString error;
    QVERIFY2(parseMultistatus(xml, &entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries.at(0).size, kUnknownSize);
    QCOMPARE(entries.at(1).size, kUnknownSize);
    QCOMPARE(entries.at(2).size, 0);
}

/// Settings pointing at a scripted server, for the three cases below.
static WebdavSettings against(const ScriptedHttpServer& server)
{
    WebdavSettings settings;
    settings.baseUrl = server.url() + QStringLiteral("/dav");
    settings.username = QStringLiteral("someone");
    settings.password = QStringLiteral("secret");
    return settings;
}

/// A depth-0 answer saying the path is a collection, which is what stat() wants
/// before a delete or a move gets as far as sending anything.
static QByteArray aCollectionAt(const QByteArray& href)
{
    return "<?xml version=\"1.0\"?>\n"
           "<d:multistatus xmlns:d=\"DAV:\">\n"
           "  <d:response><d:href>"
        + href
        + "</d:href><d:propstat><d:prop>\n"
          "    <d:resourcetype><d:collection/></d:resourcetype>\n"
          "  </d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>\n"
          "</d:multistatus>";
}

/// 207 is success for PROPFIND and the opposite for DELETE.
///
/// RFC 4918 §9.6.1: a DELETE that could not finish for some members of a
/// collection answers 207 with a <response> per member that failed, and a
/// complete one answers 204. The status went to errorFor(), where 207 is
/// unconditional success, and the body was never read -- so remove(dir, true)
/// answered ok with files still on the server. A move built on it then believes
/// it has deleted a source tree it did not delete. Nextcloud locks files while
/// it syncs, so this is the ordinary case rather than an exotic one.
void TestWebdavFileSystem::aDeleteTheServerOnlyHalfDidIsAFailureThatNamesTheMember()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request& request) {
        ScriptedHttpServer::Reply reply;
        if (request.method == "PROPFIND") {
            reply.status = 207;
            reply.reason = "Multi-Status";
            reply.headers.append("Content-Type: application/xml; charset=utf-8");
            reply.body = aCollectionAt("/dav/work/");
            return reply;
        }
        // The delete: everything but one file went, and that one is locked.
        reply.status = 207;
        reply.reason = "Multi-Status";
        reply.headers.append("Content-Type: application/xml; charset=utf-8");
        reply.body = "<?xml version=\"1.0\"?>\n"
                     "<d:multistatus xmlns:d=\"DAV:\">\n"
                     "  <d:response><d:href>/dav/work/ledger.ods</d:href>\n"
                     "    <d:status>HTTP/1.1 423 Locked</d:status></d:response>\n"
                     "</d:multistatus>";
        return reply;
    });
    QVERIFY(server.start());

    WebdavFileSystem fileSystem(QStringLiteral("webdav"), against(server));
    const Result<void> removed
        = fileSystem.remove(VfsUri(QStringLiteral("webdav"), QString(), QStringLiteral("/work")), true);

    QVERIFY2(!removed.ok(), "a delete the server said it did not finish was reported as done");
    QVERIFY2(
        removed.error().message.contains(QStringLiteral("ledger.ods")), qPrintable(removed.error().message));
    QCOMPARE(removed.error().code, VfsError::AccessDenied);
}

/// 409 on MOVE is a missing parent, not an occupied name.
///
/// rename() mapped 412 and 409 alike to AlreadyExists. 412 really is the name
/// being taken -- it is what `Overwrite: F` answers -- but RFC 4918 §9.9.4 gives
/// 409 for a destination whose parent collection does not exist. errorFor()
/// already says NotFound for a 409 on MKCOL and PUT, so the backend was
/// disagreeing with its own transport, and a rename into a folder somebody had
/// deleted came back as "already exists".
void TestWebdavFileSystem::aMoveRefusedWithA409IsAMissingParentAndNotANameClash()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request& request) {
        ScriptedHttpServer::Reply reply;
        if (request.method == "PROPFIND") {
            reply.status = 207;
            reply.reason = "Multi-Status";
            reply.headers.append("Content-Type: application/xml; charset=utf-8");
            reply.body = aCollectionAt("/dav/notes.txt");
            return reply;
        }
        reply.status = 409;
        reply.reason = "Conflict";
        return reply;
    });
    QVERIFY(server.start());

    WebdavFileSystem fileSystem(QStringLiteral("webdav"), against(server));
    const Result<void> renamed
        = fileSystem.rename(VfsUri(QStringLiteral("webdav"), QString(), QStringLiteral("/notes.txt")),
            VfsUri(QStringLiteral("webdav"), QString(), QStringLiteral("/gone/notes.txt")));

    QVERIFY(!renamed.ok());
    QCOMPARE(renamed.error().code, VfsError::NotFound);
}

/// The folder that listed itself as its own child.
///
/// Depth 1 answers with the collection and its members, and the collection was
/// recognised by comparing its href with the path we asked about. A server
/// answering under another name -- a reverse proxy rewrite, or Nextcloud's
/// /remote.php/dav/files/<user> where the request went to /remote.php/webdav --
/// never matches, so the folder appears inside itself and going into it goes
/// nowhere.
void TestWebdavFileSystem::aCollectionAnsweringUnderAnAliasDoesNotListItselfAsItsOwnChild()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.status = 207;
        reply.reason = "Multi-Status";
        reply.headers.append("Content-Type: application/xml; charset=utf-8");
        // Asked about /dav/work; answered about /remote.php/dav/files/someone/work.
        reply.body = "<?xml version=\"1.0\"?>\n"
                     "<d:multistatus xmlns:d=\"DAV:\">\n"
                     "  <d:response><d:href>/remote.php/dav/files/someone/work/</d:href>\n"
                     "    <d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop>\n"
                     "    <d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>\n"
                     "  <d:response><d:href>/remote.php/dav/files/someone/work/ledger.ods</d:href>\n"
                     "    <d:propstat><d:prop><d:resourcetype/><d:getcontentlength>12</d:getcontentlength>\n"
                     "    </d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>\n"
                     "</d:multistatus>";
        return reply;
    });
    QVERIFY(server.start());

    WebdavFileSystem fileSystem(QStringLiteral("webdav"), against(server));
    const Result<FileEntryList> listing = fileSystem.list(
        VfsUri(QStringLiteral("webdav"), QString(), QStringLiteral("/work")), CancelToken());

    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 1);
    QCOMPARE(listing.value().first().name, QStringLiteral("ledger.ods"));
}

void TestWebdavFileSystem::anAbsoluteUrlHrefIsReducedToItsPath()
{
    const QByteArray xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
  <d:response><d:href>https://cloud.example.com/dav/work/a.txt</d:href>
    <d:propstat><d:prop><d:getcontentlength>1</d:getcontentlength></d:prop>
    <d:status>HTTP/1.1 200 OK</d:status></d:propstat>
  </d:response>
</d:multistatus>)";

    QList<WebdavEntry> entries;
    QString error;
    QVERIFY2(parseMultistatus(xml, &entries, &error), qPrintable(error));
    QCOMPARE(entries.first().path, QStringLiteral("/dav/work/a.txt"));
}

void TestWebdavFileSystem::anEncodedHrefIsDecoded()
{
    const QByteArray xml = R"(<?xml version="1.0"?>
<d:multistatus xmlns:d="DAV:">
  <d:response><d:href>/dav/q1%20plan%20-%20fina%C5%82.txt</d:href>
    <d:propstat><d:prop><d:getcontentlength>1</d:getcontentlength></d:prop>
    <d:status>HTTP/1.1 200 OK</d:status></d:propstat>
  </d:response>
</d:multistatus>)";

    QList<WebdavEntry> entries;
    QString error;
    QVERIFY2(parseMultistatus(xml, &entries, &error), qPrintable(error));
    QCOMPARE(nameFromPath(entries.first().path), QString::fromUtf8("q1 plan - finał.txt"));
}

void TestWebdavFileSystem::somethingThatIsNotAMultistatusIsRejected()
{
    QList<WebdavEntry> entries;
    QString error;
    // An HTML error page is the usual thing to receive from a misconfigured
    // server, and it must not read as an empty directory.
    QVERIFY(!parseMultistatus("<html><body>403 Forbidden</body></html>", &entries, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(entries.isEmpty());
}

void TestWebdavFileSystem::pathHelpersBehave()
{
    QCOMPARE(withoutTrailingSlash(QStringLiteral("/a/b/")), QStringLiteral("/a/b"));
    QCOMPARE(withoutTrailingSlash(QStringLiteral("/a/b")), QStringLiteral("/a/b"));
    QCOMPARE(withoutTrailingSlash(QStringLiteral("/")), QStringLiteral("/"));
    QCOMPARE(nameFromPath(QStringLiteral("/a/b/c.txt")), QStringLiteral("c.txt"));
    QCOMPARE(nameFromPath(QStringLiteral("/a/b/")), QStringLiteral("b"));
}

void TestWebdavFileSystem::theBaseUrlIsSplitIntoOriginAndPath()
{
    const QVariantMap config { { QStringLiteral("url"),
        QStringLiteral("https://cloud.example.com:8443/remote.php/dav/files/lukasz/") } };
    const WebdavSettings settings = WebdavFileSystemFactory::settingsFrom(config);

    QCOMPARE(settings.origin(), QStringLiteral("https://cloud.example.com:8443"));
    QCOMPARE(settings.basePath(), QStringLiteral("/remote.php/dav/files/lukasz"));
}

void TestWebdavFileSystem::anAddressThatIsNotAUrlIsRefused()
{
    WebdavFileSystemFactory factory;
    QString error;

    QVERIFY(factory.create(QVariantMap {}, &error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("address")), qPrintable(error));

    error.clear();
    const QVariantMap notAUrl { { QStringLiteral("url"), QStringLiteral("cloud.example.com/dav") } };
    QVERIFY(factory.create(notAUrl, &error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("https://")), qPrintable(error));
}

void TestWebdavFileSystem::itSatisfiesTheConformanceSuite()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No WebDAV server in the environment; set MOLE_TEST_WEBDAV_URL, "
              "MOLE_TEST_WEBDAV_USER and MOLE_TEST_WEBDAV_PASS to run this against a real one.");
    }

    const RawWebdav raw(account);
    const QString base = QStringLiteral("mole-dav-%1").arg(QCoreApplication::applicationPid());
    QVERIFY2(raw.request("MKCOL", base), "could not create the working collection on the server");

    WebdavSettings settings;
    settings.baseUrl = account.url;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = base;

    ConformanceContext context;
    context.fileSystem = std::make_shared<WebdavFileSystem>(QStringLiteral("webdav"), settings);
    context.root = VfsUri(QStringLiteral("webdav"), QString(), QStringLiteral("/"));
    context.seedFile = [&raw, &base](const QString& relative, const QByteArray& contents) {
        return raw.request("PUT", base + QLatin1Char('/') + relative, contents);
    };
    context.seedDir = [&raw, &base](const QString& relative) {
        return raw.request("MKCOL", base + QLatin1Char('/') + relative);
    };

    runFileSystemConformance(context);

    raw.request("DELETE", base);
}

/// The write that has no alternative, against a server entitled to refuse it.
///
/// A file too large to stage locally goes out with a chunked transfer encoding,
/// because there is no other way to send something whose length nobody knows
/// yet. WebDAV servers are not uniformly happy with that -- some answer 411 and
/// demand a `Content-Length` -- which is exactly why a small write keeps the
/// staged PUT: that risk is not worth taking when there is a choice. The large
/// case has no choice, and until this ran there was nothing anywhere saying it
/// worked against a server at all.
/// A large file could not be read on a machine without room for a copy.
///
/// openRead() downloaded the whole file into a QTemporaryFile before handing
/// back a device, and its size parameter was unnamed and unused. ADR-0014's
/// second amendment says "No backend stages a whole file in either direction any
/// more" and TransferStreams.h says anything large is streamed -- neither was
/// true of WebDAV or S3, so a 94 GB backup on a machine with 84 GB free could not
/// be read at all, and a preview of a large file downloaded all of it. The write
/// side has been streamed since MOLE-34; this is the other direction.
/// See MOLE-370.
void TestWebdavFileSystem::aLargeFileIsStreamedInRangesRatherThanDownloadedWhole()
{
    constexpr qint64 kBig = 512LL * 1024 * 1024;
    const QByteArray filler(64 * 1024, 'x');

    ScriptedHttpServer server([&filler](const ScriptedHttpServer::Request& request) {
        ScriptedHttpServer::Reply reply;
        if (request.method == "HEAD") {
            reply.headers.append("Content-Length: " + QByteArray::number(kBig));
            reply.headers.append("ETag: \"the-file\"");
            return reply;
        }
        const QByteArray range = request.header("Range");
        if (request.method == "GET" && !range.isEmpty()) {
            reply.status = 206;
            reply.reason = "Partial Content";
            reply.body = filler;
            return reply;
        }
        if (request.method == "GET") {
            // The whole file, which is the thing under test: it must not happen.
            reply.body = QByteArray(int(kBig), 'x');
            return reply;
        }
        return reply;
    });
    QVERIFY2(server.start(), "the scripted server could not take a port");

    auto fileSystem = std::make_shared<WebdavFileSystem>(QStringLiteral("webdav"), against(server));
    Result<std::unique_ptr<QIODevice>> opened
        = fileSystem->openRead(VfsUri(QStringLiteral("webdav"), QString(), QStringLiteral("/backup.tar")));
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));
    QVERIFY2(dynamic_cast<net::StreamingDownload*>(opened.value().get()) != nullptr,
        "a large WebDAV file has to stream; staging is what made a file bigger than the scratch "
        "space unreadable");

    QByteArray buffer(4096, Qt::Uninitialized);
    QVERIFY(opened.value()->read(buffer.data(), buffer.size()) > 0);
    opened.value().reset();

    bool sawRangedGet = false;
    bool sawWholeGet = false;
    for (const ScriptedHttpServer::Request& asked : server.received()) {
        if (asked.method != "GET")
            continue;
        if (asked.header("Range").isEmpty()) {
            sawWholeGet = true;
            continue;
        }
        sawRangedGet = true;
        // The file this read began on, and not whatever has since taken its
        // place: the check the cross-span validator needs, at no extra request.
        QCOMPARE(asked.header("If-Match"), QByteArray("\"the-file\""));
        QVERIFY2(asked.header("Range").startsWith("bytes=0-"), asked.header("Range").constData());
    }
    QVERIFY2(sawRangedGet, "the read never asked for a range");
    QVERIFY2(!sawWholeGet, "the read asked for the whole file");
}

void TestWebdavFileSystem::aLargeFileGoesUpWholeThroughAChunkedPut()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured())
        QSKIP("No WebDAV server in the environment.");

    int megabytes = qEnvironmentVariableIntValue("MOLE_TEST_WEBDAV_LARGE_MB");
    if (megabytes <= 0)
        megabytes = 96; // above the size at which the backend stops staging

    const RawWebdav raw(account);
    const QString base = QStringLiteral("mole-dav-large-%1").arg(QCoreApplication::applicationPid());
    QVERIFY2(raw.request("MKCOL", base), "could not create the working collection on the server");

    WebdavSettings settings;
    settings.baseUrl = account.url;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = base;

    auto fileSystem = std::make_shared<WebdavFileSystem>(QStringLiteral("webdav"), settings);
    const VfsUri target(QStringLiteral("webdav"), QString(), QStringLiteral("/large.bin"));
    const qint64 size = static_cast<qint64>(megabytes) * kBlockSize;

    Result<std::unique_ptr<QIODevice>> opened = fileSystem->openWrite(target, size);
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));
    std::unique_ptr<QIODevice> stream = std::move(opened.value());

    // Which route was taken is the whole question, so it is asserted rather than
    // assumed. A threshold quietly raised past this size would otherwise turn
    // this into a second test of the staged PUT, passing while covering nothing.
    QVERIFY2(dynamic_cast<net::StreamingUpload*>(stream.get()) != nullptr,
        "a write this size should stream; staging it locally is the thing that cannot scale");

    for (int block = 0; block < megabytes; ++block) {
        const QByteArray payload = blockAt(block);
        QCOMPARE(stream->write(payload), static_cast<qint64>(payload.size()));
    }

    const Result<void> written = closeAndReport(*stream);
    stream.reset();
    QVERIFY2(written.ok(), qPrintable(written.error().message));

    // Asked of the server, not of the stream: a write is finished when the file
    // is there, and the length it reports is the only claim that matters.
    const Result<FileEntry> what = fileSystem->stat(target);
    QVERIFY2(what.ok(), qPrintable(what.error().message));
    QCOMPARE(what.value().size, size);

    Result<std::unique_ptr<QIODevice>> back = fileSystem->openRead(target, size);
    QVERIFY2(back.ok(), qPrintable(back.error().message));

    qint64 read = 0;
    bool contentsMatch = true;
    for (int block = 0; block < megabytes && contentsMatch; ++block) {
        const QByteArray chunk = back.value()->read(kBlockSize);
        read += chunk.size();
        if (chunk != blockAt(block))
            contentsMatch = false;
    }
    back.value().reset();

    raw.request("DELETE", base);

    QCOMPARE(read, size);
    QVERIFY2(contentsMatch, "what came back is not what was sent");
}

/// The staged PUT, at a size nothing can hold in memory for a second try.
///
/// The conformance suite writes files of a few bytes, and a body that small is
/// one curl keeps a copy of -- so a 401 followed by a retry costs nothing and
/// the fault below never shows. A few megabytes is past that, and the staged
/// route is what almost every real write takes, so the case worth covering is
/// the one between "trivially small" and "too large to stage".
void TestWebdavFileSystem::aStagedWriteTooBigToBufferAlsoArrives()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured())
        QSKIP("No WebDAV server in the environment.");

    const RawWebdav raw(account);
    const QString base = QStringLiteral("mole-dav-staged-%1").arg(QCoreApplication::applicationPid());
    QVERIFY2(raw.request("MKCOL", base), "could not create the working collection on the server");

    WebdavSettings settings;
    settings.baseUrl = account.url;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = base;

    auto fileSystem = std::make_shared<WebdavFileSystem>(QStringLiteral("webdav"), settings);
    const VfsUri target(QStringLiteral("webdav"), QString(), QStringLiteral("/staged.bin"));

    constexpr int kMegabytes = 8;
    const qint64 size = static_cast<qint64>(kMegabytes) * kBlockSize;

    Result<std::unique_ptr<QIODevice>> opened = fileSystem->openWrite(target, size);
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));
    std::unique_ptr<QIODevice> stream = std::move(opened.value());

    // The other route, asserted for the same reason: this test is only worth
    // running if it is testing the one the last one is not.
    QVERIFY2(dynamic_cast<net::BufferedUpload*>(stream.get()) != nullptr,
        "a write this size should still be staged and sent with an exact length");

    for (int block = 0; block < kMegabytes; ++block) {
        const QByteArray payload = blockAt(block);
        QCOMPARE(stream->write(payload), static_cast<qint64>(payload.size()));
    }

    const Result<void> written = closeAndReport(*stream);
    stream.reset();
    QVERIFY2(written.ok(), qPrintable(written.error().message));

    Result<std::unique_ptr<QIODevice>> back = fileSystem->openRead(target, size);
    QVERIFY2(back.ok(), qPrintable(back.error().message));
    const QByteArray returned = back.value()->readAll();
    back.value().reset();

    raw.request("DELETE", base);

    QCOMPARE(returned.size(), size);
    QCOMPARE(returned.left(kBlockSize), blockAt(0));
    QCOMPARE(returned.right(kBlockSize), blockAt(kMegabytes - 1));
}

MOLE_TEST_MAIN(TestWebdavFileSystem)

#include "tst_WebdavFileSystem.moc"
