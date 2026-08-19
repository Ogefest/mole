#include "plugins/network/S3FileSystem.h"
#include "plugins/network/S3Listing.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "support/Victim.h"

#include <QUrl>

#include <algorithm>
#include <cstring>
#include <curl/curl.h>

using namespace mole;
using namespace mole::test;

namespace {

struct Account
{
    QString keyId;
    QString secret;
    QString region;
    QString endpoint;
    QString bucket;

    /// The configuration the application would build from this account.
    ///
    /// Through the factory rather than field by field, because assigning the
    /// endpoint straight across skips the parsing every real drive goes
    /// through: an endpoint carrying a scheme -- which is what anybody pastes --
    /// became a host name of "http" with the bucket glued to the front.
    QVariantMap asConfig() const
    {
        QVariantMap config { { QStringLiteral("accessKeyId"), keyId },
            { QStringLiteral("secretAccessKey"), secret }, { QStringLiteral("bucket"), bucket },
            { QStringLiteral("region"), region } };
        if (!endpoint.isEmpty()) {
            config.insert(QStringLiteral("endpoint"), endpoint);
            // An endpoint that says http:// means http. The factory strips the
            // scheme to get a host and then defaults to https, which is right
            // for a pasted bucket address and wrong for a server on a desk.
            if (endpoint.startsWith(QLatin1String("http://")))
                config.insert(QStringLiteral("useHttps"), false);
        }
        if (!addressing.isEmpty())
            config.insert(QStringLiteral("addressing"), addressing);
        return config;
    }

    /// The endpoint with a scheme on the front, for the raw client below. The
    /// settings the backend gets have the scheme stripped off again by the
    /// factory; this is the same address written the way curl wants it.
    QString endpointUrl() const
    {
        if (endpoint.isEmpty())
            return QStringLiteral("https://s3.%1.amazonaws.com").arg(region);
        if (endpoint.startsWith(QLatin1String("http://")) || endpoint.startsWith(QLatin1String("https://")))
            return endpoint;
        return QStringLiteral("https://") + endpoint;
    }

    /// "path" or "virtual". MinIO and most Ceph installs require path-style,
    /// and a suite that cannot ask for it can only ever be pointed at AWS.
    QString addressing;

    bool isConfigured() const { return !keyId.isEmpty() && !secret.isEmpty() && !bucket.isEmpty(); }
};

Account accountFromEnvironment()
{
    const auto value = [](const char* name) { return QString::fromLocal8Bit(qgetenv(name)); };

    Account account;
    account.keyId = value("MOLE_TEST_S3_KEY_ID");
    account.secret = value("MOLE_TEST_S3_SECRET");
    account.bucket = value("MOLE_TEST_S3_BUCKET");
    account.endpoint = value("MOLE_TEST_S3_ENDPOINT");
    account.region = value("MOLE_TEST_S3_REGION");
    account.addressing = value("MOLE_TEST_S3_ADDRESSING");
    if (account.region.isEmpty())
        account.region = QStringLiteral("us-east-1");
    return account;
}

/// Seeds and cleans up through libcurl's own SigV4 rather than through ours.
///
/// This is the point of the exercise: if the fixtures were signed by the code
/// under test, a signing bug would break the fixtures and the backend in the
/// same way and the suite would still pass.
class RawS3
{
public:
    explicit RawS3(const Account& account)
        : m_account(account)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    bool put(const QString& key, const QByteArray& contents) const
    {
        Payload payload { contents, 0 };
        CURL* handle = prepare(key, {});
        if (!handle)
            return false;
        curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(handle, CURLOPT_READFUNCTION, readPayload);
        curl_easy_setopt(handle, CURLOPT_READDATA, &payload);
        curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(contents.size()));
        const bool ok = run(handle);
        curl_easy_cleanup(handle);
        return ok;
    }

    /// Begins a multipart upload and leaves it unfinished, which is the fixture
    /// a killed process leaves behind: parts on the server, nothing that will
    /// ever complete them, and the bucket charging for them until somebody says
    /// otherwise. Seeded through plain libcurl rather than through the backend,
    /// like every other fixture here -- what is being tested is whether the
    /// backend can *find* one.
    QString beginUpload(const QString& key) const
    {
        QByteArray body;
        CURL* handle = prepare(key, { { QStringLiteral("uploads"), QString() } });
        if (!handle)
            return {};
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, 0L);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
        const bool ok = run(handle);
        curl_easy_cleanup(handle);
        return ok ? net::parseMultipartUploadId(body) : QString();
    }

    /// One part of an upload begun above. S3 keeps it, and charges for it.
    bool putPart(const QString& key, const QString& uploadId, int number, const QByteArray& contents) const
    {
        Payload payload { contents, 0 };
        CURL* handle = prepare(key,
            { { QStringLiteral("partNumber"), QString::number(number) },
                { QStringLiteral("uploadId"), uploadId } });
        if (!handle)
            return false;
        curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(handle, CURLOPT_READFUNCTION, readPayload);
        curl_easy_setopt(handle, CURLOPT_READDATA, &payload);
        curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(contents.size()));
        const bool ok = run(handle);
        curl_easy_cleanup(handle);
        return ok;
    }

    bool remove(const QString& key) const
    {
        CURL* handle = prepare(key, {});
        if (!handle)
            return false;
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
        const bool ok = run(handle);
        curl_easy_cleanup(handle);
        return ok;
    }

    /// Every key under a prefix. Used only to clean up, and deliberately scoped:
    /// these buckets hold real files and nothing outside the prefix is touched.
    QStringList keysUnder(const QString& prefix) const
    {
        QByteArray body;
        CURL* handle = prepare(QString(),
            { { QStringLiteral("list-type"), QStringLiteral("2") }, { QStringLiteral("prefix"), prefix } });
        if (!handle)
            return {};
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
        const bool ok = run(handle);
        curl_easy_cleanup(handle);
        if (!ok)
            return {};

        QStringList keys;
        mole::net::S3ListPage page;
        QString error;
        if (mole::net::parseListObjectsV2(body, &page, &error)) {
            for (const mole::net::S3Object& object : page.objects)
                keys.append(object.key);
        }
        return keys;
    }

    void removeTree(const QString& prefix) const
    {
        for (const QString& key : keysUnder(prefix))
            remove(key);
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

    static size_t collect(char* data, size_t size, size_t count, void* userData)
    {
        static_cast<QByteArray*>(userData)->append(data, static_cast<int>(size * count));
        return size * count;
    }

    static size_t discard(char*, size_t size, size_t count, void*) { return size * count; }

    CURL* prepare(const QString& key, const QList<QPair<QString, QString>>& query) const
    {
        CURL* handle = curl_easy_init();
        if (!handle)
            return nullptr;

        // The endpoint as given, scheme and all. It used to be "https://" glued
        // to whatever was in the variable, so an endpoint that already carried
        // one produced https://http://host and resolved nothing.
        QByteArray url = m_account.endpointUrl().toUtf8() + '/' + m_account.bucket.toUtf8();
        if (!key.isEmpty())
            url += '/' + QUrl::toPercentEncoding(key, "/");
        bool first = true;
        for (const auto& parameter : query) {
            url += first ? '?' : '&';
            first = false;
            url += QUrl::toPercentEncoding(parameter.first) + '=' + QUrl::toPercentEncoding(parameter.second);
        }

        const QByteArray credentials = (m_account.keyId + QLatin1Char(':') + m_account.secret).toUtf8();
        const QByteArray provider = ("aws:amz:" + m_account.region + ":s3").toUtf8();

        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_USERPWD, credentials.constData());
        curl_easy_setopt(handle, CURLOPT_AWS_SIGV4, provider.constData());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        return handle;
    }

    static bool run(CURL* handle)
    {
        if (curl_easy_perform(handle) != CURLE_OK)
            return false;
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        return status >= 200 && status < 300;
    }

    Account m_account;
};

} // namespace

class TestS3FileSystem : public QObject
{
    Q_OBJECT

private slots:
    void anEndpointIsDerivedFromTheRegionWhenNotGiven();
    void aPastedUrlIsAcceptedAsAnEndpoint();
    void theAddressingStyleDecidesWhereTheBucketGoes();
    void aBucketWithADotInItAlwaysGoesInThePath();
    void anEndpointThatAlreadyCarriesTheBucketIsNotDoubled();
    void aFormWithoutABucketIsRefused();
    void theFormAsksOnlyWhatS3Needs();
    void spaceAndAccessAreRefusedRatherThanInvented();
    void itSatisfiesTheConformanceSuite();
    void anObjectTooBigForOneRequestGoesUpInParts();

    void aListOfUnfinishedUploadsIsRead();
    void anErrorDocumentIsNotAnEmptyListOfLeftovers();
    void anUploadInterruptedByAKilledProcessCanBeFoundAndRemoved();
    void anUploadKilledOutrightIsFoundAndRemovedAfterwards();
};

void TestS3FileSystem::anEndpointIsDerivedFromTheRegionWhenNotGiven()
{
    QVariantMap config { { QStringLiteral("region"), QStringLiteral("eu-central-1") } };
    const S3Settings settings = S3FileSystemFactory::settingsFrom(config);
    QCOMPARE(settings.resolvedEndpoint(), QStringLiteral("s3.eu-central-1.amazonaws.com"));
}

void TestS3FileSystem::aPastedUrlIsAcceptedAsAnEndpoint()
{
    // The field asks for a host and people paste a url. Taking the host out of
    // one is kinder than refusing it, and it is what a B2 console gives you.
    QVariantMap config { { QStringLiteral("endpoint"),
        QStringLiteral("https://s3.us-east-005.backblazeb2.com/") } };
    const S3Settings settings = S3FileSystemFactory::settingsFrom(config);
    QCOMPARE(settings.endpoint, QStringLiteral("s3.us-east-005.backblazeb2.com"));
}

void TestS3FileSystem::theAddressingStyleDecidesWhereTheBucketGoes()
{
    QVariantMap config { { QStringLiteral("endpoint"), QStringLiteral("s3.example.com") },
        { QStringLiteral("bucket"), QStringLiteral("photos") } };

    const S3Settings hosted = S3FileSystemFactory::settingsFrom(config);
    QVERIFY(!hosted.pathStyleAddressing);
    QCOMPARE(hosted.hostName(), QStringLiteral("photos.s3.example.com"));

    config.insert(QStringLiteral("addressing"), QStringLiteral("path"));
    const S3Settings pathStyle = S3FileSystemFactory::settingsFrom(config);
    QVERIFY(pathStyle.pathStyleAddressing);
    // The bucket moves into the path, so the host stays as configured -- which is
    // what MinIO and most Ceph installs require.
    QCOMPARE(pathStyle.hostName(), QStringLiteral("s3.example.com"));
}

void TestS3FileSystem::aBucketWithADotInItAlwaysGoesInThePath()
{
    // Regression. Virtual-hosted addressing put the bucket in the host name, and a
    // certificate wildcard covers exactly one name part -- so "my.backups" produced
    // "my.backups.s3.us-east-005.backblazeb2.com" and the connection failed with
    // "no alternative certificate subject name matches target host name". That
    // looks like a broken server; it is a name that could never have worked.
    QVariantMap config { { QStringLiteral("endpoint"), QStringLiteral("s3.us-east-005.backblazeb2.com") },
        { QStringLiteral("bucket"), QStringLiteral("my.backups") },
        // Explicitly asking for host addressing must not override it.
        { QStringLiteral("addressing"), QStringLiteral("host") } };

    const S3Settings dotted = S3FileSystemFactory::settingsFrom(config);
    QVERIFY(!dotted.bucketFitsInHostName());
    QVERIFY2(dotted.usesPathStyle(), "a dotted bucket has to go in the path");
    QCOMPARE(dotted.hostName(), QStringLiteral("s3.us-east-005.backblazeb2.com"));

    // A name that fits still goes in the host, so nothing changes for the ordinary
    // case.
    config.insert(QStringLiteral("bucket"), QStringLiteral("example-bucket"));
    const S3Settings plain = S3FileSystemFactory::settingsFrom(config);
    QVERIFY(plain.bucketFitsInHostName());
    QVERIFY(!plain.usesPathStyle());
    QCOMPARE(plain.hostName(), QStringLiteral("example-bucket.s3.us-east-005.backblazeb2.com"));
}

void TestS3FileSystem::anEndpointThatAlreadyCarriesTheBucketIsNotDoubled()
{
    // The address a B2 console shows includes the bucket, so it gets pasted into
    // the endpoint field. Prepending the bucket again gave
    // "bucket.bucket.s3.…" and failed TLS for the same reason.
    const QVariantMap config { { QStringLiteral("endpoint"),
                                   QStringLiteral("example-bucket.s3.us-east-005.backblazeb2.com") },
        { QStringLiteral("bucket"), QStringLiteral("example-bucket") } };

    const S3Settings settings = S3FileSystemFactory::settingsFrom(config);
    QCOMPARE(settings.hostName(), QStringLiteral("example-bucket.s3.us-east-005.backblazeb2.com"));
    // Still host-style, so the bucket must not appear in the path either.
    QVERIFY(!settings.usesPathStyle());
}

void TestS3FileSystem::aFormWithoutABucketIsRefused()
{
    S3FileSystemFactory factory;
    QString error;
    const QVariantMap config { { QStringLiteral("accessKeyId"), QStringLiteral("key") },
        { QStringLiteral("secretAccessKey"), QStringLiteral("secret") } };

    QVERIFY(factory.create(config, &error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("bucket")), qPrintable(error));
}

void TestS3FileSystem::theFormAsksOnlyWhatS3Needs()
{
    const S3FileSystemFactory factory;
    const QList<ConnectionField> fields = factory.connectionFields();

    int required = 0;
    for (const ConnectionField& field : fields) {
        if (field.required && !field.advanced)
            ++required;
    }
    // Key, secret, bucket, region. rclone's generated S3 form had dozens.
    QCOMPARE(required, 4);
    QVERIFY2(fields.size() <= 10, "the S3 form should stay short enough to fill in");

    // One engine for every S3-compatible store means the endpoint and the
    // addressing style have to be ordinary fields rather than a variant each.
    bool hasEndpoint = false;
    bool hasAddressing = false;
    for (const ConnectionField& field : fields) {
        hasEndpoint = hasEndpoint || field.key == QLatin1String("endpoint");
        hasAddressing = hasAddressing || field.key == QLatin1String("addressing");
    }
    QVERIFY(hasEndpoint);
    QVERIFY(hasAddressing);
}

void TestS3FileSystem::spaceAndAccessAreRefusedRatherThanInvented()
{
    S3Settings settings;
    settings.accessKeyId = QStringLiteral("key");
    settings.secretAccessKey = QStringLiteral("secret");
    settings.bucket = QStringLiteral("bucket");
    S3FileSystem fs(QStringLiteral("s3"), settings);

    QVERIFY(!fs.capabilities().testFlag(VfsCapability::ReportsAccess));
    QVERIFY(!fs.capabilities().testFlag(VfsCapability::ReportsSpace));

    // No network is touched: a bucket has no capacity in any useful sense and no
    // per-key permissions, so both are refused before any request is made.
    const VfsUri root(QStringLiteral("s3"), QString(), QStringLiteral("/"));
    QCOMPARE(fs.space(root).error().code, VfsError::NotSupported);
    QCOMPARE(fs.access(root).error().code, VfsError::NotSupported);
}

// ---- what a killed process leaves behind ---------------------------------

void TestS3FileSystem::aListOfUnfinishedUploadsIsRead()
{
    // The document S3 answers ListMultipartUploads with, including the two
    // markers -- a key and an upload id, because two uploads of one key can be
    // in flight and the key alone does not say where to carry on from. Ignoring
    // the paging would report the first thousand leftovers and leave the rest
    // being charged for, which is the fault this is here to end.
    const QByteArray xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListMultipartUploadsResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Bucket>photos</Bucket>
  <KeyMarker></KeyMarker>
  <NextKeyMarker>holiday/sunset.raw</NextKeyMarker>
  <NextUploadIdMarker>abc-002</NextUploadIdMarker>
  <IsTruncated>true</IsTruncated>
  <Upload>
    <Key>holiday/sunset.raw</Key>
    <UploadId>abc-001</UploadId>
    <Initiated>2026-08-01T10:11:12.000Z</Initiated>
  </Upload>
  <Upload>
    <Key>holiday/sunset.raw</Key>
    <UploadId>abc-002</UploadId>
    <Initiated>2026-08-02T10:11:12.000Z</Initiated>
  </Upload>
  <Upload>
    <Key>no-id-so-nothing-can-be-done</Key>
    <Initiated>2026-08-03T10:11:12.000Z</Initiated>
  </Upload>
</ListMultipartUploadsResult>)";

    net::S3UploadPage page;
    QString problem;
    QVERIFY2(net::parseListMultipartUploads(xml, &page, &problem), qPrintable(problem));

    // The same key twice, told apart by the id -- which is why the id is half
    // of the handle a leftover carries.
    QCOMPARE(page.uploads.size(), 2);
    QCOMPARE(page.uploads.at(0).key, QStringLiteral("holiday/sunset.raw"));
    QCOMPARE(page.uploads.at(0).uploadId, QStringLiteral("abc-001"));
    QCOMPARE(page.uploads.at(1).uploadId, QStringLiteral("abc-002"));
    QCOMPARE(page.uploads.at(0).initiated.toUTC(), QDateTime(QDate(2026, 8, 1), QTime(10, 11, 12), Qt::UTC));

    // An entry with no upload id is dropped: nothing can be done about it, and
    // offering to abandon it would offer an action that fails.
    QVERIFY(page.truncated);
    QCOMPARE(page.nextKeyMarker, QStringLiteral("holiday/sunset.raw"));
    QCOMPARE(page.nextUploadIdMarker, QStringLiteral("abc-002"));
}

void TestS3FileSystem::anErrorDocumentIsNotAnEmptyListOfLeftovers()
{
    // The one answer that must never be mistaken for "nothing left behind":
    // that answer is what keeps somebody paying for storage they cannot see.
    const QByteArray denied = R"(<?xml version="1.0" encoding="UTF-8"?>
<Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>)";

    net::S3UploadPage page;
    QString problem;
    QVERIFY2(!net::parseListMultipartUploads(denied, &page, &problem),
        "an error document was read as a bucket with nothing left behind");
    QVERIFY2(problem.contains(QStringLiteral("Access Denied")), qPrintable(problem));
    QVERIFY(page.uploads.isEmpty());

    // And so must a document that is not S3's at all.
    net::S3UploadPage nonsense;
    QVERIFY(!net::parseListMultipartUploads("<html><body>proxy says no</body></html>", &nonsense, &problem));
    QVERIFY(!net::parseListMultipartUploads(QByteArray(), &nonsense, &problem));
}

void TestS3FileSystem::anUploadInterruptedByAKilledProcessCanBeFoundAndRemoved()
{
    // The whole of MOLE-96 against a real bucket: an upload is begun and never
    // finished -- which is what a process killed mid-copy leaves -- and then it
    // has to be findable and removable without leaving Mole. S3 charges for the
    // parts until one or the other happens.
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No S3 account in the environment; set MOLE_TEST_S3_KEY_ID, MOLE_TEST_S3_SECRET "
              "and MOLE_TEST_S3_BUCKET to run this against a real bucket.");
    }

    S3Settings settings = S3FileSystemFactory::settingsFrom(account.asConfig());
    settings.prefix = QStringLiteral("mole-leftover-%1").arg(QCoreApplication::applicationPid());
    auto fileSystem = std::make_shared<S3FileSystem>(QStringLiteral("s3"), settings);

    QVERIFY2(fileSystem->capabilities().testFlag(VfsCapability::ReportsLeftovers),
        "a drive that can find its leftovers has to say so");

    // Nothing yet, and that answer has to be reachable: a drive with nothing
    // left behind must say so rather than fail.
    const Result<QList<DriveLeftover>> before = fileSystem->leftovers(std::chrono::seconds(0), {});
    QVERIFY2(before.ok(), qPrintable(before.error().message));
    QCOMPARE(before.value().size(), 0);

    // An upload begun and never finished, seeded raw. This is what a process
    // killed mid-copy leaves: a part on the server, and nothing alive that
    // knows the upload id.
    const RawS3 raw(account);
    const QString key = settings.prefix + QStringLiteral("/interrupted.bin");
    const QString uploadId = raw.beginUpload(key);
    QVERIFY2(!uploadId.isEmpty(), "could not begin an upload on the server");
    // Five mebibytes, which is S3's floor for any part but the last.
    QVERIFY2(raw.putPart(key, uploadId, 1, QByteArray(5 * 1024 * 1024, 'x')),
        "could not put a part on the server");

    const VfsUri target(QStringLiteral("s3"), QString(), QStringLiteral("/interrupted.bin"));

    // Found, with a threshold of nothing -- an age of zero is what a test uses
    // and never what a sweep would.
    const Result<QList<DriveLeftover>> found = fileSystem->leftovers(std::chrono::seconds(0), {});
    QVERIFY2(found.ok(), qPrintable(found.error().message));
    QVERIFY2(found.value().size() == 1,
        qPrintable(QStringLiteral("%1 leftovers, expected one").arg(found.value().size())));

    const DriveLeftover& leftover = found.value().first();
    QVERIFY2(leftover.path.contains(QStringLiteral("interrupted.bin")), qPrintable(leftover.path));
    QVERIFY(!leftover.handle.isEmpty());
    QVERIFY2(!leftover.what.isEmpty(), "a leftover has to say what it is");

    // An age threshold hides it, which is the guard that stops a sweep from
    // abandoning an upload another Mole has in flight this minute.
    const Result<QList<DriveLeftover>> tooYoung = fileSystem->leftovers(std::chrono::hours(24), {});
    QVERIFY2(tooYoung.ok(), qPrintable(tooYoung.error().message));
    QCOMPARE(tooYoung.value().size(), 0);

    // And removed.
    const Result<void> discarded = fileSystem->discardLeftover(leftover);
    QVERIFY2(discarded.ok(), qPrintable(discarded.error().message));

    const Result<QList<DriveLeftover>> after = fileSystem->leftovers(std::chrono::seconds(0), {});
    QVERIFY2(after.ok(), qPrintable(after.error().message));
    QCOMPARE(after.value().size(), 0);

    // The object never appeared, because the upload never completed.
    QVERIFY2(!fileSystem->stat(target).ok(), "an upload nobody finished must not become an object");
}

void TestS3FileSystem::anObjectTooBigForOneRequestGoesUpInParts()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No S3 account in the environment; set MOLE_TEST_S3_KEY_ID, MOLE_TEST_S3_SECRET "
              "and MOLE_TEST_S3_BUCKET to run this against a real bucket.");
    }

    // Past the part size, so this is a multipart upload: begin, several parts,
    // complete. Written block by block and never held whole, because the point
    // of the exercise is an object bigger than the machine sending it.
    int megabytes = qEnvironmentVariableIntValue("MOLE_TEST_S3_LARGE_MB");
    if (megabytes <= 0)
        megabytes = 150;

    // Through the factory, for the same reason the conformance case is: an
    // endpoint assigned straight across never gets parsed, and the second copy
    // of that mistake is how it survived the first one being fixed.
    S3Settings settings = S3FileSystemFactory::settingsFrom(account.asConfig());
    settings.prefix = QStringLiteral("mole-multipart-%1").arg(QCoreApplication::applicationPid());

    auto fileSystem = std::make_shared<S3FileSystem>(QStringLiteral("s3"), settings);
    const VfsUri target(QStringLiteral("s3"), QString(), QStringLiteral("/big.bin"));

    constexpr int kBlockSize = 1024 * 1024;
    const auto blockAt = [](int index) {
        QByteArray block(kBlockSize, Qt::Uninitialized);
        for (int i = 0; i < kBlockSize; ++i)
            block[i] = static_cast<char>((i * 31 + index * 17) & 0xff);
        return block;
    };

    Result<std::unique_ptr<QIODevice>> stream = fileSystem->openWrite(target);
    QVERIFY2(stream.ok(), qPrintable(stream.error().message));

    bool wroteEverything = true;
    for (int block = 0; block < megabytes && wroteEverything; ++block) {
        const QByteArray content = blockAt(block);
        wroteEverything = stream.value()->write(content) == content.size();
    }
    const Result<void> committed = closeAndReport(*stream.value());

    const auto cleanUp = [&fileSystem, &target] { fileSystem->remove(target, false); };

    if (!wroteEverything || !committed.ok()) {
        cleanUp();
        QVERIFY2(wroteEverything, "the stream stopped accepting bytes part way through");
        QVERIFY2(committed.ok(), qPrintable(committed.error().message));
    }

    // What the bucket says it now holds, which is the only opinion that counts.
    const Result<FileEntry> what = fileSystem->stat(target);
    if (!what.ok())
        cleanUp();
    QVERIFY2(what.ok(), qPrintable(what.error().message));
    const qint64 announced = what.value().size;

    const Result<std::unique_ptr<QIODevice>> back = fileSystem->openRead(target, announced);
    bool contentsMatch = back.ok();
    qint64 read = 0;
    if (back.ok()) {
        for (int block = 0; block < megabytes; ++block) {
            const QByteArray chunk = back.value()->read(kBlockSize);
            read += chunk.size();
            if (chunk != blockAt(block)) {
                contentsMatch = false;
                break;
            }
        }
    }

    cleanUp();

    QCOMPARE(announced, static_cast<qint64>(megabytes) * kBlockSize);
    QCOMPARE(read, static_cast<qint64>(megabytes) * kBlockSize);
    QVERIFY2(contentsMatch, "what came back is not what was sent");
}

void TestS3FileSystem::itSatisfiesTheConformanceSuite()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No S3 account in the environment; set MOLE_TEST_S3_KEY_ID, MOLE_TEST_S3_SECRET "
              "and MOLE_TEST_S3_BUCKET to run this against a real bucket.");
    }

    const RawS3 raw(account);
    const QString prefix = QStringLiteral("mole-conformance-%1").arg(QCoreApplication::applicationPid());

    raw.removeTree(prefix + QStringLiteral("/"));

    // Through the factory, not field by field. Assigning endpoint straight
    // across skipped the parsing every real drive goes through, so an endpoint
    // carrying a scheme -- which is what anybody pastes -- became a host name
    // of "http" with the bucket glued to the front of it. Building the settings
    // the way the application builds them also means this suite exercises that
    // path rather than a hand-made shortcut around it.
    S3Settings settings = S3FileSystemFactory::settingsFrom(account.asConfig());
    settings.prefix = prefix;

    ConformanceContext context;
    context.fileSystem = std::make_shared<S3FileSystem>(QStringLiteral("s3"), settings);
    context.root = VfsUri(QStringLiteral("s3"), QString(), QStringLiteral("/"));
    context.seedFile = [&raw, &prefix](const QString& relative, const QByteArray& contents) {
        return raw.put(prefix + QLatin1Char('/') + relative, contents);
    };
    context.seedDir = [&raw, &prefix](const QString& relative) {
        // A directory in a bucket is a zero-byte object whose key ends in '/',
        // which is what the AWS console writes and what everything reads.
        return raw.put(prefix + QLatin1Char('/') + relative + QLatin1Char('/'), QByteArray());
    };

    runFileSystemConformance(context);

    raw.removeTree(prefix + QStringLiteral("/"));
}

/// The same thing, with a process that really is killed.
///
/// It lives here rather than in tst_KilledOutright, where the rest of the kill
/// scenarios are: that suite is a core one and this needs a backend from the
/// network plugin, which is not built everywhere. Pointing a core suite at a
/// plugin to keep the scenarios together would be the wrong way round.
///
/// What it adds over the case above is that nothing tidies up. SIGKILL runs no
/// destructor, so abandonMultipart() -- which handles every failure the process
/// is alive to see -- never runs, and the parts stay on the server being charged
/// for with nothing left that knows their upload id.
void TestS3FileSystem::anUploadKilledOutrightIsFoundAndRemovedAfterwards()
{
    const Account account = accountFromEnvironment();

    if (Victim::isThisProcess()) {
        // Uploads until somebody stops it. Nothing here is asserted: this
        // process exists to be killed. The prefix to work under arrives as the
        // instruction, so the parent knows where to look afterwards.
        if (!account.isConfigured())
            return;
        S3Settings settings = S3FileSystemFactory::settingsFrom(account.asConfig());
        settings.prefix = Victim::instruction();
        auto fs = std::make_shared<S3FileSystem>(QStringLiteral("s3"), settings);
        const VfsUri target(QStringLiteral("s3"), QString(), QStringLiteral("/killed.bin"));
        Result<std::unique_ptr<QIODevice>> stream = fs->openWrite(target, 400LL * 1024 * 1024);
        if (!stream.ok())
            return;
        const QByteArray block(1024 * 1024, 'k');
        for (int i = 0; i < 400; ++i) {
            if (stream.value()->write(block) != block.size())
                break;
        }
        return;
    }

    if (!account.isConfigured()) {
        QSKIP("No S3 account in the environment; set MOLE_TEST_S3_KEY_ID, MOLE_TEST_S3_SECRET "
              "and MOLE_TEST_S3_BUCKET to run this against a real bucket.");
    }

    const QString prefix = QStringLiteral("mole-killed-%1").arg(QCoreApplication::applicationPid());
    S3Settings settings = S3FileSystemFactory::settingsFrom(account.asConfig());
    settings.prefix = prefix;
    auto fileSystem = std::make_shared<S3FileSystem>(QStringLiteral("s3"), settings);

    Victim victim(QStringLiteral("anUploadKilledOutrightIsFoundAndRemovedAfterwards"), prefix);
    QVERIFY2(victim.started(), "could not start a second copy of this test binary");

    // Killed once the upload exists on the server -- until then there is nothing
    // to interrupt. Waited for on the condition rather than for a duration: how
    // long the server takes to answer is its business, not this test's.
    //
    // What is proved is that an upload a killed process began can be found and
    // removed. Whether a part had already landed is not asked, because nothing
    // in the interface reports it and adding a call for the sake of a test would
    // be the wrong way round -- and it does not change the answer: abandoning an
    // upload takes its parts with it, which is what stops the charging.
    const bool begun = victim.waitUntil([&fileSystem] {
        const Result<QList<DriveLeftover>> now = fileSystem->leftovers(std::chrono::seconds(0), {});
        return now.ok() && !now.value().isEmpty();
    });
    victim.kill();
    QVERIFY2(begun,
        qPrintable(
            QStringLiteral("no part ever reached the server. The victim said: %1").arg(victim.transcript())));

    // Nothing in the listing, because parts are not objects. This is exactly why
    // the leftovers could not be found before: there was nowhere to see them.
    const Result<FileEntryList> listing
        = fileSystem->list(VfsUri(QStringLiteral("s3"), QString(), QStringLiteral("/")), {});
    QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    QCOMPARE(listing.value().size(), 0);

    const Result<QList<DriveLeftover>> found = fileSystem->leftovers(std::chrono::seconds(0), {});
    QVERIFY2(found.ok(), qPrintable(found.error().message));
    QVERIFY2(!found.value().isEmpty(), "the upload a killed process left behind could not be found");
    QVERIFY2(found.value().first().path.contains(QStringLiteral("killed.bin")),
        qPrintable(found.value().first().path));

    // And removed, which is the half that stops the charging.
    for (const DriveLeftover& leftover : found.value()) {
        const Result<void> discarded = fileSystem->discardLeftover(leftover);
        QVERIFY2(discarded.ok(), qPrintable(discarded.error().message));
    }

    const Result<QList<DriveLeftover>> after = fileSystem->leftovers(std::chrono::seconds(0), {});
    QVERIFY2(after.ok(), qPrintable(after.error().message));
    QCOMPARE(after.value().size(), 0);
}

MOLE_TEST_MAIN(TestS3FileSystem)

#include "tst_S3FileSystem.moc"
