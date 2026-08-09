#include "plugins/network/S3FileSystem.h"
#include "plugins/network/S3Listing.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"

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

        QByteArray url = "https://" + m_account.endpoint.toUtf8() + '/' + m_account.bucket.toUtf8();
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
    config.insert(QStringLiteral("bucket"), QStringLiteral("testbucket2312"));
    const S3Settings plain = S3FileSystemFactory::settingsFrom(config);
    QVERIFY(plain.bucketFitsInHostName());
    QVERIFY(!plain.usesPathStyle());
    QCOMPARE(plain.hostName(), QStringLiteral("testbucket2312.s3.us-east-005.backblazeb2.com"));
}

void TestS3FileSystem::anEndpointThatAlreadyCarriesTheBucketIsNotDoubled()
{
    // The address a B2 console shows includes the bucket, so it gets pasted into
    // the endpoint field. Prepending the bucket again gave
    // "bucket.bucket.s3.…" and failed TLS for the same reason.
    const QVariantMap config { { QStringLiteral("endpoint"),
                                   QStringLiteral("testbucket2312.s3.us-east-005.backblazeb2.com") },
        { QStringLiteral("bucket"), QStringLiteral("testbucket2312") } };

    const S3Settings settings = S3FileSystemFactory::settingsFrom(config);
    QCOMPARE(settings.hostName(), QStringLiteral("testbucket2312.s3.us-east-005.backblazeb2.com"));
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

    S3Settings settings;
    settings.accessKeyId = account.keyId;
    settings.secretAccessKey = account.secret;
    settings.region = account.region;
    settings.endpoint = account.endpoint;
    settings.bucket = account.bucket;
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

    S3Settings settings;
    settings.accessKeyId = account.keyId;
    settings.secretAccessKey = account.secret;
    settings.region = account.region;
    settings.endpoint = account.endpoint;
    settings.bucket = account.bucket;
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

MOLE_TEST_MAIN(TestS3FileSystem)

#include "tst_S3FileSystem.moc"
