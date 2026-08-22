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

/// One upload that was started and never finished or abandoned.
struct S3Upload
{
    QString key;
    QString uploadId;
    QDateTime initiated;
};

/// One page of a ListMultipartUploads answer.
///
/// Paged like every other S3 listing, and with two tokens rather than one --
/// a key and an upload id, because two uploads of the same key can be in
/// flight. Ignoring them would report the first thousand leftovers and leave
/// the rest being charged for, which is the whole fault this is here to fix.
struct S3UploadPage
{
    QList<S3Upload> uploads;
    QString nextKeyMarker;
    QString nextUploadIdMarker;
    bool truncated = false;
};

/// Parses a ListMultipartUploads response. False and `errorOut` when the
/// document is not one -- an error document must not read as "nothing left
/// behind", which is the answer that would quietly keep somebody paying.
bool parseListMultipartUploads(const QByteArray& xml, S3UploadPage* page, QString* errorOut);

/// One earlier state of an object, as a versioned container reports it.
struct S3Version
{
    QString key;
    /// The container's own identifier for this state of the object. Opaque:
    /// it goes back to the same container and nothing else reads it.
    QString versionId;
    /// Whether this is the object as it is now rather than an earlier state.
    bool latest = false;
    /// Whether this entry is the record of the object having been deleted. It
    /// is not a state anybody can read, and it is not a version to offer.
    bool deleteMarker = false;
    qint64 size = 0;
    QDateTime modified;
};

/// One page of a ListObjectVersions answer.
///
/// Paged with two markers rather than one, like the uploads listing above and
/// for the same reason: several states of one key can straddle a page boundary,
/// so the key alone cannot say where to carry on from.
struct S3VersionPage
{
    QList<S3Version> versions;
    /// Sub-"directories", as the delimiter found them, prefix and trailing
    /// slash included -- the same shape ListObjectsV2 reports.
    QStringList commonPrefixes;
    QString nextKeyMarker;
    QString nextVersionIdMarker;
    bool truncated = false;
};

/// Parses a ListObjectVersions response. False and `errorOut` when the document
/// is not one -- an error document must not read as "nothing earlier is kept",
/// which is the answer that quietly hides what is there.
bool parseListObjectVersions(const QByteArray& xml, S3VersionPage* page, QString* errorOut);

/// Whether a GetBucketVersioning answer says the container keeps earlier
/// objects. Anything else -- Suspended, absent, an error document -- is no.
bool parseVersioningEnabled(const QByteArray& xml);

} // namespace mole::net
