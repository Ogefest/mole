#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

namespace mole::net {

/// One object in a bucket.
struct S3Object
{
    QString key;
    qint64 size = 0;
    QDateTime modified;
    QString etag;
};

/// One page of a ListObjectsV2 answer.
///
/// S3 pages at a thousand keys and says so with a continuation token. A backend
/// that ignored it would show the first thousand entries of a directory and
/// silently drop the rest, which is worse than failing.
struct S3ListPage
{
    QList<S3Object> objects;
    /// Sub-"directories", as the delimiter found them. Still carrying the full
    /// prefix and its trailing slash, exactly as S3 reports them.
    QStringList commonPrefixes;
    QString nextContinuationToken;
    bool truncated = false;
};

/// Parses a ListObjectsV2 response. Returns false and fills `errorOut` when the
/// document is not one -- which is what an error document from the server looks
/// like, and it must not be mistaken for an empty bucket.
bool parseListObjectsV2(const QByteArray& xml, S3ListPage* page, QString* errorOut);

/// Pulls the human-readable part out of an S3 error document, so a failure can
/// say "SignatureDoesNotMatch" rather than "the server answered 403".
QString parseS3Error(const QByteArray& xml);

/// Names of the buckets in a ListAllMyBuckets response.
QStringList parseBucketList(const QByteArray& xml);

/// The upload id out of an InitiateMultipartUpload answer. Empty when the
/// document is not one, which is how an error document arrives.
QString parseMultipartUploadId(const QByteArray& xml);

} // namespace mole::net
