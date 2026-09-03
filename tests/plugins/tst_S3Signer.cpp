#include "plugins/network/S3FileSystem.h"
#include "plugins/network/S3Listing.h"
#include "plugins/network/S3Signer.h"
#include "support/MoleTestMain.h"

using namespace mole;
using namespace mole::net;

namespace {

SigningIdentity exampleIdentity()
{
    // AWS's own documentation credentials. They authenticate nothing; they exist
    // so a signature is reproducible.
    SigningIdentity identity;
    identity.accessKeyId = QStringLiteral("AKIDEXAMPLE");
    identity.secretAccessKey = QStringLiteral("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
    identity.region = QStringLiteral("us-east-1");
    identity.service = QStringLiteral("s3");
    return identity;
}

QDateTime utcStamp(const QString& amzDate)
{
    QDateTime stamp = QDateTime::fromString(amzDate, QStringLiteral("yyyyMMddThhmmssZ"));
    stamp.setTimeSpec(Qt::UTC);
    return stamp;
}

/// Fills in the three headers every S3 request carries, so each case only has to
/// say what makes it different.
SignableRequest requestFor(const QByteArray& host, const QString& amzDate)
{
    SignableRequest request;
    request.payloadSha256 = emptyPayloadSha256();
    request.timestamp = utcStamp(amzDate);
    request.headers = { { "host", host }, { "x-amz-content-sha256", request.payloadSha256 },
        { "x-amz-date", amzDate.toUtf8() } };
    return request;
}

QByteArray signatureOf(const SignableRequest& request, const SigningIdentity& identity)
{
    for (const auto& header : signWithSigV4(request, identity)) {
        if (header.first == "Authorization") {
            const int at = header.second.lastIndexOf("Signature=");
            return header.second.mid(at + 10);
        }
    }
    return {};
}

} // namespace

class TestS3Signer : public QObject
{
    Q_OBJECT

private slots:
    void theEmptyPayloadHashIsTheKnownConstant();
    void aPlainGetMatchesCurl();
    void aQueryIsSortedAndMatchesCurl();
    void anEncodedPathMatchesCurl();
    void theCanonicalRequestHasTheSpecifiedShape();
    void theAmzHeadersAreSignedEvenWhenTheCallerDidNotAddThem();
    void aPresignedUrlMatchesTheDocumentedExample();
    void aPresignedUrlSignsNothingTheRecipientCannotSend();
    void aPresignedUrlIsSpecificToItsObjectItsLifetimeAndItsKey();
    void queryParametersAreSortedRegardlessOfOrder();
    void reservedCharactersAreEncodedAwsStyle();
    void slashesSurviveInAPathButNotInAQuery();
    void aListingIsRead();
    void aTruncatedListingCarriesItsToken();
    void anErrorDocumentIsNotMistakenForAListing();
    void aSizeThatIsNotANumberIsUnknownAndNotZero();
    void anErrorMessageIsPulledOut();
    void bucketNamesAreRead();

    void aVersionListingIsRead();
    void aTruncatedVersionListingCarriesBothItsMarkers();
    void anErrorDocumentIsNotAnEmptyVersionListing();
    void aContainerSaysWhetherItKeepsEarlierObjects();

    void theLinkIsOfferedOnAnObjectAndNotOnAPrefix();
    void aDriveWithNothingToSignWithOffersNoLink();
    void theLinkIsForThatObjectAndSaysWhenItStopsWorking();
};

void TestS3Signer::theEmptyPayloadHashIsTheKnownConstant()
{
    // The published SHA-256 of the empty string. Every request without a body
    // sends it, so a wrong value here would break all of them at once.
    QCOMPARE(
        emptyPayloadSha256(), QByteArray("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

void TestS3Signer::aPlainGetMatchesCurl()
{
    // Every expected signature in this file was produced by curl 8.5's own
    // --aws-sigv4 for the identical request and captured off the wire. They are
    // recorded rather than derived: a signing bug is otherwise indistinguishable
    // from a wrong password, and checking our arithmetic against our own
    // arithmetic would prove nothing.
    SignableRequest request = requestFor("127.0.0.1:18080", QStringLiteral("20260809T090117Z"));
    request.path = QStringLiteral("/test.txt");

    QCOMPARE(signatureOf(request, exampleIdentity()),
        QByteArray("f8d2b0a3eebb37a931fa7d59e93c768b137fa3b11528dd49c3747a97fbe317ae"));
}

void TestS3Signer::aQueryIsSortedAndMatchesCurl()
{
    SignableRequest request = requestFor("127.0.0.1:19381", QStringLiteral("20260809T090944Z"));
    request.path = QStringLiteral("/");
    // Given in the order a listing builds them, which is not sorted order.
    request.queryParameters = { { QStringLiteral("list-type"), QStringLiteral("2") },
        { QStringLiteral("delimiter"), QStringLiteral("/") } };

    QCOMPARE(signatureOf(request, exampleIdentity()),
        QByteArray("49339b34eaeeeab678250666dd34760ccdff4f0595999303ddc2e88691210f47"));
}

void TestS3Signer::anEncodedPathMatchesCurl()
{
    SignableRequest request = requestFor("127.0.0.1:19377", QStringLiteral("20260809T090948Z"));
    // Held unencoded: the signer encodes it, and the url is built from the same
    // encoder so the signed path and the sent path cannot differ.
    request.path = QStringLiteral("/reports/2026/q1 plan.txt");

    QCOMPARE(canonicalPathFor(request), QByteArray("/reports/2026/q1%20plan.txt"));
    QCOMPARE(signatureOf(request, exampleIdentity()),
        QByteArray("c9dbb1b4bd8761e6d88564fbd19feb2e5955040e05a3dbc93e80dbe790817607"));
}

void TestS3Signer::theCanonicalRequestHasTheSpecifiedShape()
{
    SignableRequest request
        = requestFor("examplebucket.s3.amazonaws.com", QStringLiteral("20130524T000000Z"));
    request.path = QStringLiteral("/test.txt");
    request.headers.append(qMakePair(QByteArray("range"), QByteArray("bytes=0-9")));

    // Asserted as text because this is the string the whole algorithm hangs on.
    // A wrong signature says only "denied"; a wrong canonical request says which
    // line went wrong.
    const QByteArray expected = "GET\n"
                                "/test.txt\n"
                                "\n"
                                "host:examplebucket.s3.amazonaws.com\n"
                                "range:bytes=0-9\n"
                                "x-amz-content-sha256:"
                                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
                                "x-amz-date:20130524T000000Z\n"
                                "\n"
                                "host;range;x-amz-content-sha256;x-amz-date\n"
                                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    QCOMPARE(canonicalRequestFor(request), expected);
}

void TestS3Signer::theAmzHeadersAreSignedEvenWhenTheCallerDidNotAddThem()
{
    // Regression. The signer produces x-amz-date and x-amz-content-sha256, and
    // once sent they must also be signed -- S3 refuses the request outright with
    // "header 'x-amz-content-sha256' must be included in signature". Leaving that
    // to the caller meant it worked wherever they were passed by hand and failed
    // against a real bucket, so the signer adds them itself.
    SignableRequest request;
    request.path = QStringLiteral("/");
    request.payloadSha256 = emptyPayloadSha256();
    request.timestamp = utcStamp(QStringLiteral("20260809T090117Z"));
    request.headers = { { "host", "bucket.s3.example.com" } }; // and nothing else

    const QByteArray canonical = canonicalRequestFor(request);
    QVERIFY2(canonical.contains("x-amz-content-sha256:"), canonical.constData());
    QVERIFY2(canonical.contains("x-amz-date:20260809T090117Z"), canonical.constData());
    QVERIFY2(canonical.contains("host;x-amz-content-sha256;x-amz-date"), canonical.constData());

    for (const auto& header : signWithSigV4(request, exampleIdentity())) {
        if (header.first == "Authorization") {
            QVERIFY2(header.second.contains("SignedHeaders=host;x-amz-content-sha256;x-amz-date"),
                header.second.constData());
        }
    }

    // Supplying them explicitly must not duplicate them.
    SignableRequest explicitly = request;
    explicitly.headers.append(qMakePair(QByteArray("x-amz-date"), QByteArray("20260809T090117Z")));
    explicitly.headers.append(qMakePair(QByteArray("x-amz-content-sha256"), request.payloadSha256));
    QCOMPARE(canonicalRequestFor(explicitly), canonical);
}

/// AWS publishes this one worked example, credentials and all, and its answer.
/// An independent check of the whole algorithm: a signature computed here that
/// matches theirs cannot be self-consistently wrong.
void TestS3Signer::aPresignedUrlMatchesTheDocumentedExample()
{
    SigningIdentity identity;
    identity.accessKeyId = QStringLiteral("AKIAIOSFODNN7EXAMPLE");
    identity.secretAccessKey = QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    identity.region = QStringLiteral("us-east-1");
    identity.service = QStringLiteral("s3");

    SignableRequest request;
    request.method = "GET";
    request.path = QStringLiteral("/test.txt");
    request.headers = { { "host", "examplebucket.s3.amazonaws.com" } };
    request.timestamp = utcStamp(QStringLiteral("20130524T000000Z"));

    const QString url = presignedUrl(request, identity, std::chrono::seconds(86400), true);

    QCOMPARE(url,
        QStringLiteral("https://examplebucket.s3.amazonaws.com/test.txt"
                       "?X-Amz-Algorithm=AWS4-HMAC-SHA256"
                       "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2F"
                       "aws4_request"
                       "&X-Amz-Date=20130524T000000Z"
                       "&X-Amz-Expires=86400"
                       "&X-Amz-SignedHeaders=host"
                       "&X-Amz-Signature="
                       "aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404"));
}

/// Whoever is handed one of these sends none of our headers, so signing over
/// them would produce a url that cannot be checked by anybody but us.
void TestS3Signer::aPresignedUrlSignsNothingTheRecipientCannotSend()
{
    SignableRequest request;
    request.method = "GET";
    request.path = QStringLiteral("/reports/q1.pdf");
    request.headers = { { "host", "bucket.s3.example.com" }, { "range", "bytes=0-9" } };
    request.timestamp = utcStamp(QStringLiteral("20260809T090117Z"));

    const QString url = presignedUrl(request, exampleIdentity(), std::chrono::seconds(900), true);

    QVERIFY2(url.contains(QStringLiteral("X-Amz-SignedHeaders=host")), qPrintable(url));
    QVERIFY2(!url.contains(QStringLiteral("range")), qPrintable(url));
    QVERIFY2(!url.contains(QStringLiteral("x-amz-content-sha256")), qPrintable(url));
    // The signature is of the query, so it cannot be inside the query it signs.
    QVERIFY2(url.lastIndexOf(QStringLiteral("&X-Amz-Signature=")) > url.indexOf(QStringLiteral("?")),
        qPrintable(url));
}

void TestS3Signer::aPresignedUrlIsSpecificToItsObjectItsLifetimeAndItsKey()
{
    SignableRequest request;
    request.method = "GET";
    request.path = QStringLiteral("/reports/q1.pdf");
    request.headers = { { "host", "bucket.s3.example.com" } };
    request.timestamp = utcStamp(QStringLiteral("20260809T090117Z"));

    const QString url = presignedUrl(request, exampleIdentity(), std::chrono::seconds(900), true);
    QCOMPARE(presignedUrl(request, exampleIdentity(), std::chrono::seconds(900), true), url);

    SignableRequest other = request;
    other.path = QStringLiteral("/reports/q2.pdf");
    QVERIFY(presignedUrl(other, exampleIdentity(), std::chrono::seconds(900), true) != url);

    QVERIFY(presignedUrl(request, exampleIdentity(), std::chrono::seconds(901), true) != url);

    SigningIdentity elsewhere = exampleIdentity();
    elsewhere.secretAccessKey = QStringLiteral("a different secret entirely");
    QVERIFY(presignedUrl(request, elsewhere, std::chrono::seconds(900), true) != url);
}

void TestS3Signer::queryParametersAreSortedRegardlessOfOrder()
{
    const QList<QPair<QString, QString>> scrambled { { QStringLiteral("prefix"), QStringLiteral("b") },
        { QStringLiteral("delimiter"), QStringLiteral("/") },
        { QStringLiteral("list-type"), QStringLiteral("2") } };
    const QList<QPair<QString, QString>> ordered { { QStringLiteral("delimiter"), QStringLiteral("/") },
        { QStringLiteral("list-type"), QStringLiteral("2") },
        { QStringLiteral("prefix"), QStringLiteral("b") } };

    QCOMPARE(canonicalQuery(scrambled), canonicalQuery(ordered));
    QCOMPARE(canonicalQuery(scrambled), QByteArray("delimiter=%2F&list-type=2&prefix=b"));
}

void TestS3Signer::reservedCharactersAreEncodedAwsStyle()
{
    // AWS's unreserved set is exactly these four punctuation marks plus letters
    // and digits. QUrl disagrees about '~' and '*', which is why this is not
    // delegated to it.
    QCOMPARE(uriEncode("aZ0-_.~", false), QByteArray("aZ0-_.~"));
    QCOMPARE(uriEncode("*", false), QByteArray("%2A"));
    QCOMPARE(uriEncode(" ", false), QByteArray("%20"));
    QCOMPARE(uriEncode("+", false), QByteArray("%2B"));
    QCOMPARE(uriEncode("=", false), QByteArray("%3D"));
    QCOMPARE(uriEncode("&", false), QByteArray("%26"));
}

void TestS3Signer::slashesSurviveInAPathButNotInAQuery()
{
    QCOMPARE(uriEncode("a/b", true), QByteArray("a/b"));
    QCOMPARE(uriEncode("a/b", false), QByteArray("a%2Fb"));
}

void TestS3Signer::aListingIsRead()
{
    const QByteArray xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>bucket</Name>
  <Prefix>work/</Prefix>
  <Delimiter>/</Delimiter>
  <IsTruncated>false</IsTruncated>
  <Contents>
    <Key>work/alpha.txt</Key>
    <LastModified>2026-08-09T08:54:12.000Z</LastModified>
    <ETag>&quot;5d41402abc4b2a76b9719d911017c592&quot;</ETag>
    <Size>5</Size>
  </Contents>
  <CommonPrefixes><Prefix>work/nested/</Prefix></CommonPrefixes>
</ListBucketResult>)";

    S3ListPage page;
    QString error;
    QVERIFY2(parseListObjectsV2(xml, &page, &error), qPrintable(error));
    QCOMPARE(page.objects.size(), 1);
    QCOMPARE(page.objects.first().key, QStringLiteral("work/alpha.txt"));
    QCOMPARE(page.objects.first().size, 5);
    QCOMPARE(page.objects.first().etag, QStringLiteral("5d41402abc4b2a76b9719d911017c592"));
    QCOMPARE(page.objects.first().modified.date(), QDate(2026, 8, 9));
    QCOMPARE(page.commonPrefixes, QStringList { QStringLiteral("work/nested/") });
    QVERIFY(!page.truncated);
}

void TestS3Signer::aTruncatedListingCarriesItsToken()
{
    // S3 pages at a thousand keys. A backend that ignored this would show the
    // first page of a directory and silently lose the rest.
    const QByteArray xml = R"(<?xml version="1.0"?>
<ListBucketResult>
  <IsTruncated>true</IsTruncated>
  <NextContinuationToken>1ueGcxLPRx1Tr</NextContinuationToken>
  <Contents><Key>a</Key><Size>1</Size></Contents>
</ListBucketResult>)";

    S3ListPage page;
    QString error;
    QVERIFY2(parseListObjectsV2(xml, &page, &error), qPrintable(error));
    QVERIFY(page.truncated);
    QCOMPARE(page.nextContinuationToken, QStringLiteral("1ueGcxLPRx1Tr"));
}

/// A Size element that is missing or is not a number is *unknown*.
///
/// Amazon always sends one; the S3-compatible stores do not all agree about
/// that, and a bare toLongLong() turned every disagreement into a file of
/// nought bytes -- which is the value that switches the short-read guard off in
/// TransferTask. See MOLE-344.
void TestS3Signer::aSizeThatIsNotANumberIsUnknownAndNotZero()
{
    const QByteArray xml = R"(<?xml version="1.0"?>
<ListBucketResult>
  <Contents><Key>no-size.bin</Key></Contents>
  <Contents><Key>odd-size.bin</Key><Size>unknown</Size></Contents>
  <Contents><Key>really-empty.bin</Key><Size>0</Size></Contents>
</ListBucketResult>)";

    S3ListPage page;
    QString error;
    QVERIFY2(parseListObjectsV2(xml, &page, &error), qPrintable(error));
    QCOMPARE(page.objects.size(), 3);
    QCOMPARE(page.objects.at(0).size, kUnknownSize);
    QCOMPARE(page.objects.at(1).size, kUnknownSize);
    QCOMPARE(page.objects.at(2).size, 0);
}

void TestS3Signer::anErrorDocumentIsNotMistakenForAListing()
{
    // The dangerous failure: an error document parsed as a listing looks like an
    // empty directory, so a permissions problem would show as "nothing here".
    const QByteArray xml = R"(<?xml version="1.0"?>
<Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>)";

    S3ListPage page;
    QString error;
    QVERIFY(!parseListObjectsV2(xml, &page, &error));
    QVERIFY2(error.contains(QStringLiteral("AccessDenied")), qPrintable(error));
    QVERIFY(page.objects.isEmpty());
}

void TestS3Signer::anErrorMessageIsPulledOut()
{
    const QByteArray xml = R"(<?xml version="1.0"?>
<Error><Code>SignatureDoesNotMatch</Code>
<Message>The request signature we calculated does not match.</Message></Error>)";

    const QString message = parseS3Error(xml);
    QVERIFY2(message.contains(QStringLiteral("SignatureDoesNotMatch")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("does not match")), qPrintable(message));
    QVERIFY(parseS3Error(QByteArray("not xml at all")).isEmpty());
}

void TestS3Signer::bucketNamesAreRead()
{
    const QByteArray xml = R"(<?xml version="1.0"?>
<ListAllMyBucketsResult><Owner><ID>x</ID></Owner><Buckets>
<Bucket><Name>svh-test1</Name><CreationDate>2024-01-05T15:39:19.666Z</CreationDate></Bucket>
<Bucket><Name>testbucket2312</Name><CreationDate>2023-11-19T11:07:20.994Z</CreationDate></Bucket>
</Buckets></ListAllMyBucketsResult>)";

    QCOMPARE(
        parseBucketList(xml), QStringList({ QStringLiteral("svh-test1"), QStringLiteral("testbucket2312") }));
}

void TestS3Signer::aVersionListingIsRead()
{
    const QByteArray xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListVersionsResult>
  <Name>papers</Name>
  <Prefix>reports/</Prefix>
  <Delimiter>/</Delimiter>
  <MaxKeys>1000</MaxKeys>
  <IsTruncated>false</IsTruncated>
  <Version>
    <Key>reports/q1.pdf</Key>
    <VersionId>3HL4kqtJvjVBH40Nrjfkd</VersionId>
    <IsLatest>true</IsLatest>
    <LastModified>2026-08-20T09:15:00.000Z</LastModified>
    <Size>4096</Size>
  </Version>
  <Version>
    <Key>reports/q1.pdf</Key>
    <VersionId>QUpfdndhfd8438MNFDN93jd</VersionId>
    <IsLatest>false</IsLatest>
    <LastModified>2026-08-01T11:00:00.000Z</LastModified>
    <Size>2048</Size>
  </Version>
  <DeleteMarker>
    <Key>reports/gone.pdf</Key>
    <VersionId>MarkerVersionId</VersionId>
    <IsLatest>true</IsLatest>
    <LastModified>2026-08-19T08:00:00.000Z</LastModified>
  </DeleteMarker>
  <CommonPrefixes><Prefix>reports/archive/</Prefix></CommonPrefixes>
</ListVersionsResult>)";

    S3VersionPage page;
    QString error;
    QVERIFY2(parseListObjectVersions(xml, &page, &error), qPrintable(error));

    QCOMPARE(page.versions.size(), 3);
    QVERIFY(!page.truncated);
    QCOMPARE(page.commonPrefixes, QStringList { QStringLiteral("reports/archive/") });

    QCOMPARE(page.versions.at(0).key, QStringLiteral("reports/q1.pdf"));
    QVERIFY(page.versions.at(0).latest);
    QCOMPARE(page.versions.at(0).size, qint64(4096));

    // The one that is worth having: an earlier state, told apart from the object
    // as it is by IsLatest and nothing else.
    QVERIFY(!page.versions.at(1).latest);
    QVERIFY(!page.versions.at(1).deleteMarker);
    QCOMPARE(page.versions.at(1).versionId, QStringLiteral("QUpfdndhfd8438MNFDN93jd"));
    QCOMPARE(
        page.versions.at(1).modified.toUTC().toString(Qt::ISODate), QStringLiteral("2026-08-01T11:00:00Z"));

    // A deletion is a record, not a state anybody can open. Read, and marked as
    // what it is, so nothing offers it as something to look at.
    QVERIFY(page.versions.at(2).deleteMarker);
    QCOMPARE(page.versions.at(2).key, QStringLiteral("reports/gone.pdf"));
}

/// Two markers rather than one, because several states of a key can straddle a
/// page boundary and the key alone cannot say where to carry on from.
void TestS3Signer::aTruncatedVersionListingCarriesBothItsMarkers()
{
    const QByteArray xml = R"(<ListVersionsResult>
  <IsTruncated>true</IsTruncated>
  <NextKeyMarker>reports/q1.pdf</NextKeyMarker>
  <NextVersionIdMarker>QUpfdndhfd8438MNFDN93jd</NextVersionIdMarker>
  <Version>
    <Key>reports/q1.pdf</Key><VersionId>one</VersionId><IsLatest>false</IsLatest><Size>1</Size>
  </Version>
</ListVersionsResult>)";

    S3VersionPage page;
    QString error;
    QVERIFY2(parseListObjectVersions(xml, &page, &error), qPrintable(error));
    QVERIFY(page.truncated);
    QCOMPARE(page.nextKeyMarker, QStringLiteral("reports/q1.pdf"));
    QCOMPARE(page.nextVersionIdMarker, QStringLiteral("QUpfdndhfd8438MNFDN93jd"));
}

/// An error document must not read as "nothing earlier is kept", which is the
/// answer that quietly hides what is there.
void TestS3Signer::anErrorDocumentIsNotAnEmptyVersionListing()
{
    const QByteArray xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<Error><Code>AccessDenied</Code><Message>Access Denied.</Message></Error>)";

    S3VersionPage page;
    QString error;
    QVERIFY(!parseListObjectVersions(xml, &page, &error));
    QVERIFY2(error.contains(QStringLiteral("Access Denied")), qPrintable(error));
    QVERIFY(page.versions.isEmpty());
}

void TestS3Signer::aContainerSaysWhetherItKeepsEarlierObjects()
{
    QVERIFY(parseVersioningEnabled(
        QByteArrayLiteral("<VersioningConfiguration><Status>Enabled</Status></VersioningConfiguration>")));

    // Suspended is not enabled: what is already kept stays, and nothing new is,
    // so offering it as a drive that keeps earlier objects would be a promise
    // about the next edit that the container has stopped making.
    QVERIFY(!parseVersioningEnabled(
        QByteArrayLiteral("<VersioningConfiguration><Status>Suspended</Status></VersioningConfiguration>")));

    // What a container that has never had it switched on answers with.
    QVERIFY(!parseVersioningEnabled(QByteArrayLiteral("<VersioningConfiguration/>")));
    QVERIFY(!parseVersioningEnabled(QByteArrayLiteral("<Error><Code>NoSuchBucket</Code></Error>")));
    QVERIFY(!parseVersioningEnabled(QByteArray()));
}

namespace {

/// A drive built the way the application builds one, and pointed at nothing:
/// making a link asks the far end nothing, so none of this needs a server.
FileSystemPtr driveFor(const QString& keyId, const QString& secret)
{
    QVariantMap config { { QStringLiteral("accessKeyId"), keyId },
        { QStringLiteral("secretAccessKey"), secret }, { QStringLiteral("bucket"), QStringLiteral("papers") },
        { QStringLiteral("region"), QStringLiteral("us-east-1") },
        { QStringLiteral("endpoint"), QStringLiteral("s3.example.com") },
        { QStringLiteral("addressing"), QStringLiteral("path") } };

    QString error;
    return S3FileSystemFactory().create(config, &error);
}

FileEntry objectAt(const VfsUri& uri)
{
    FileEntry entry;
    entry.name = uri.fileName();
    entry.uri = uri;
    return entry;
}

} // namespace

/// A prefix is not an object: there is nothing to hand anybody a link to, and
/// offering one there would be an entry that cannot work.
void TestS3Signer::theLinkIsOfferedOnAnObjectAndNotOnAPrefix()
{
    FileSystemPtr drive = driveFor(QStringLiteral("AKIDEXAMPLE"), QStringLiteral("a secret"));
    QVERIFY(drive);

    const VfsUri object = VfsUri::fromString(QStringLiteral("s3://papers/reports/q1.pdf"));
    const FileActionList offered = drive->actionsFor(object, objectAt(object));
    QCOMPARE(offered.size(), 1);
    QCOMPARE(offered.first().id, S3FileSystem::linkActionId());
    QCOMPARE(offered.first().answers, FileActionKind::Text);

    FileEntry folder = objectAt(VfsUri::fromString(QStringLiteral("s3://papers/reports")));
    folder.isDir = true;
    QVERIFY(drive->actionsFor(folder.uri, folder).isEmpty());

    const VfsUri root = VfsUri::fromString(QStringLiteral("s3://papers/"));
    QVERIFY(drive->actionsFor(root, objectAt(root)).isEmpty());
}

void TestS3Signer::aDriveWithNothingToSignWithOffersNoLink()
{
    // Built directly rather than through the factory, which insists on a key:
    // what is being held here is that the backend refuses on its own rather than
    // relying on nobody being able to configure one.
    S3Settings settings;
    settings.bucket = QStringLiteral("papers");
    settings.endpoint = QStringLiteral("s3.example.com");
    settings.pathStyleAddressing = true;
    auto drive = std::make_shared<S3FileSystem>(QStringLiteral("s3"), settings);

    const VfsUri object = VfsUri::fromString(QStringLiteral("s3://papers/reports/q1.pdf"));
    QVERIFY2(drive->actionsFor(object, objectAt(object)).isEmpty(),
        "a drive reading a public container has no key to sign a link with");
    QCOMPARE(drive->invoke(S3FileSystem::linkActionId(), object, CancelToken()).error().code,
        VfsError::AccessDenied);
}

void TestS3Signer::theLinkIsForThatObjectAndSaysWhenItStopsWorking()
{
    FileSystemPtr drive = driveFor(QStringLiteral("AKIDEXAMPLE"), QStringLiteral("a secret"));
    QVERIFY(drive);

    const VfsUri object = VfsUri::fromString(QStringLiteral("s3://papers/reports/q1 plan.pdf"));
    const Result<FileActionOutcome> outcome
        = drive->invoke(S3FileSystem::linkActionId(), object, CancelToken());
    QVERIFY2(outcome.ok(), qPrintable(outcome.error().message));

    QCOMPARE(outcome.value().kind, FileActionKind::Text);
    const QString link = outcome.value().text;
    QVERIFY2(link.startsWith(QStringLiteral("https://s3.example.com/papers/reports/q1%20plan.pdf?")),
        qPrintable(link));
    QVERIFY2(link.contains(QStringLiteral("X-Amz-Signature=")), qPrintable(link));
    QVERIFY2(link.contains(QStringLiteral("X-Amz-Expires=900")), qPrintable(link));

    // The lifetime travels with it. A link with no stated expiry is one somebody
    // pastes somewhere and is surprised by later.
    QVERIFY(outcome.value().validUntil.isValid());
    const qint64 seconds = QDateTime::currentDateTime().secsTo(outcome.value().validUntil);
    QVERIFY2(seconds > 800 && seconds <= 900, qPrintable(QString::number(seconds)));

    // And nothing else on the drive answers to that id.
    QCOMPARE(drive->invoke(QStringLiteral("org.mole.s3.nothing"), object, CancelToken()).error().code,
        VfsError::NotSupported);
}

MOLE_TEST_MAIN(TestS3Signer)

#include "tst_S3Signer.moc"
