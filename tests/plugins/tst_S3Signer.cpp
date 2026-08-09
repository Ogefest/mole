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
    void queryParametersAreSortedRegardlessOfOrder();
    void reservedCharactersAreEncodedAwsStyle();
    void slashesSurviveInAPathButNotInAQuery();
    void aListingIsRead();
    void aTruncatedListingCarriesItsToken();
    void anErrorDocumentIsNotMistakenForAListing();
    void anErrorMessageIsPulledOut();
    void bucketNamesAreRead();
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

MOLE_TEST_MAIN(TestS3Signer)

#include "tst_S3Signer.moc"
