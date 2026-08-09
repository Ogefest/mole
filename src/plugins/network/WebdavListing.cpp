#include "plugins/network/WebdavListing.h"

#include <QUrl>
#include <QXmlStreamReader>

namespace mole::net {
namespace {

    int statusFromLine(const QString& line)
    {
        // "HTTP/1.1 200 OK"
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString& part : parts) {
            bool ok = false;
            const int value = part.toInt(&ok);
            if (ok && value >= 100 && value < 600)
                return value;
        }
        return 0;
    }

    /// WebDAV dates are RFC 1123 in getlastmodified and ISO 8601 in creationdate.
    ///
    /// The "GMT" is spelled out first, because Qt's RFC 2822 reader accepts a
    /// numeric offset and nothing else -- and every HTTP date ends in "GMT", so
    /// without this every WebDAV timestamp would come back empty.
    QDateTime parseDate(const QString& text)
    {
        QString normalised = text.trimmed();

        // The weekday goes first. It carries no information -- the date is fully
        // specified without it -- and Qt *validates* it, so a server whose
        // arithmetic disagrees by a day has its timestamp rejected outright
        // rather than being off by a little. Dropping it cannot lose anything.
        const int comma = normalised.indexOf(QLatin1Char(','));
        if (comma >= 0 && comma <= 4)
            normalised = normalised.mid(comma + 1).trimmed();

        if (normalised.endsWith(QLatin1String("GMT"), Qt::CaseInsensitive)
            || normalised.endsWith(QLatin1String("UTC"), Qt::CaseInsensitive)) {
            normalised.chop(3);
            normalised = normalised.trimmed() + QStringLiteral(" +0000");
        }

        QDateTime stamp = QDateTime::fromString(normalised, Qt::RFC2822Date);
        if (!stamp.isValid())
            stamp = QDateTime::fromString(text, Qt::RFC2822Date);
        if (!stamp.isValid())
            stamp = QDateTime::fromString(text, Qt::ISODateWithMs);
        if (!stamp.isValid())
            stamp = QDateTime::fromString(text, Qt::ISODate);
        return stamp;
    }

} // namespace

QString withoutTrailingSlash(const QString& path)
{
    QString out = path;
    while (out.size() > 1 && out.endsWith(QLatin1Char('/')))
        out.chop(1);
    return out;
}

QString nameFromPath(const QString& path)
{
    const QString trimmed = withoutTrailingSlash(path);
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? trimmed : trimmed.mid(slash + 1);
}

bool parseMultistatus(const QByteArray& xml, QList<WebdavEntry>* entries, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    QXmlStreamReader reader(xml);
    bool sawMultistatus = false;

    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;

        if (!sawMultistatus) {
            if (reader.name() != QLatin1String("multistatus"))
                return fail(QStringLiteral("The server did not answer with a PROPFIND result"));
            sawMultistatus = true;
            continue;
        }

        if (reader.name() != QLatin1String("response"))
            continue;

        WebdavEntry entry;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isEndElement()) {
                if (reader.name() == QLatin1String("response"))
                    break;
                continue;
            }
            if (!reader.isStartElement())
                continue;

            const QStringView name = reader.name();
            if (name == QLatin1String("href")) {
                const QString href = reader.readElementText();
                // Servers answer with a full url as readily as with a path, and
                // the href is percent-encoded either way.
                entry.path = QUrl(href).path();
                if (entry.path.isEmpty())
                    entry.path = QUrl::fromPercentEncoding(href.toUtf8());
            } else if (name == QLatin1String("collection")) {
                entry.isCollection = true;
            } else if (name == QLatin1String("getcontentlength")) {
                entry.size = reader.readElementText().toLongLong();
            } else if (name == QLatin1String("getlastmodified")) {
                entry.modified = parseDate(reader.readElementText());
            } else if (name == QLatin1String("status")) {
                const int status = statusFromLine(reader.readElementText());
                // A propstat for properties the server could not supply carries
                // 404; it must not overwrite the good one from the same response.
                if (entry.status == 0 || status == 200)
                    entry.status = status;
            } else if (name == QLatin1String("resourcetype")) {
                // Entered rather than skipped, so a <collection/> inside is seen.
                continue;
            }
        }

        if (!entry.path.isEmpty())
            entries->append(entry);
    }

    if (reader.hasError())
        return fail(QStringLiteral("Could not read the PROPFIND result: %1").arg(reader.errorString()));
    if (!sawMultistatus)
        return fail(QStringLiteral("The server sent an empty answer"));
    return true;
}

} // namespace mole::net
