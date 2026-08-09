#include "plugins/network/S3Listing.h"

#include <QXmlStreamReader>

namespace mole::net {
namespace {

    /// S3 timestamps are ISO 8601 with milliseconds and a Z. Qt reads that, but
    /// some compatible implementations leave the milliseconds out, so both are
    /// accepted rather than one being assumed.
    QDateTime parseTimestamp(const QString& text)
    {
        QDateTime stamp = QDateTime::fromString(text, Qt::ISODateWithMs);
        if (!stamp.isValid())
            stamp = QDateTime::fromString(text, Qt::ISODate);
        return stamp;
    }

} // namespace

bool parseListObjectsV2(const QByteArray& xml, S3ListPage* page, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    QXmlStreamReader reader(xml);
    bool sawRoot = false;

    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;

        const QStringView name = reader.name();

        if (!sawRoot) {
            if (name == QLatin1String("Error"))
                return fail(parseS3Error(xml));
            if (name != QLatin1String("ListBucketResult"))
                return fail(QStringLiteral("The server did not answer with a bucket listing"));
            sawRoot = true;
            continue;
        }

        if (name == QLatin1String("Contents")) {
            S3Object object;
            while (!reader.atEnd()) {
                reader.readNext();
                if (reader.isEndElement() && reader.name() == QLatin1String("Contents"))
                    break;
                if (!reader.isStartElement())
                    continue;
                const QStringView field = reader.name();
                if (field == QLatin1String("Key"))
                    object.key = reader.readElementText();
                else if (field == QLatin1String("Size"))
                    object.size = reader.readElementText().toLongLong();
                else if (field == QLatin1String("LastModified"))
                    object.modified = parseTimestamp(reader.readElementText());
                else if (field == QLatin1String("ETag"))
                    object.etag = reader.readElementText().remove(QLatin1Char('"'));
            }
            if (!object.key.isEmpty())
                page->objects.append(object);
        } else if (name == QLatin1String("CommonPrefixes")) {
            while (!reader.atEnd()) {
                reader.readNext();
                if (reader.isEndElement() && reader.name() == QLatin1String("CommonPrefixes"))
                    break;
                if (reader.isStartElement() && reader.name() == QLatin1String("Prefix")) {
                    const QString prefix = reader.readElementText();
                    if (!prefix.isEmpty())
                        page->commonPrefixes.append(prefix);
                }
            }
        } else if (name == QLatin1String("NextContinuationToken")) {
            page->nextContinuationToken = reader.readElementText();
        } else if (name == QLatin1String("IsTruncated")) {
            page->truncated
                = reader.readElementText().trimmed().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
        }
    }

    if (reader.hasError())
        return fail(QStringLiteral("Could not read the bucket listing: %1").arg(reader.errorString()));
    if (!sawRoot)
        return fail(QStringLiteral("The server sent an empty answer"));
    return true;
}

QString parseS3Error(const QByteArray& xml)
{
    QXmlStreamReader reader(xml);
    QString code;
    QString message;

    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        if (reader.name() == QLatin1String("Code"))
            code = reader.readElementText();
        else if (reader.name() == QLatin1String("Message"))
            message = reader.readElementText();
    }

    if (code.isEmpty() && message.isEmpty())
        return {};
    if (message.isEmpty())
        return code;
    if (code.isEmpty())
        return message;
    return QStringLiteral("%1 (%2)").arg(message, code);
}

QStringList parseBucketList(const QByteArray& xml)
{
    QStringList buckets;
    QXmlStreamReader reader(xml);
    bool insideBucket = false;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == QLatin1String("Bucket"))
                insideBucket = true;
            else if (insideBucket && reader.name() == QLatin1String("Name"))
                buckets.append(reader.readElementText());
        } else if (reader.isEndElement() && reader.name() == QLatin1String("Bucket")) {
            insideBucket = false;
        }
    }
    return buckets;
}

QString parseMultipartUploadId(const QByteArray& xml)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement
            && reader.name() == QLatin1String("UploadId")) {
            return reader.readElementText();
        }
    }
    return {};
}

} // namespace mole::net
