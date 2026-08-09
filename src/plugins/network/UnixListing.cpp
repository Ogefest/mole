#include "plugins/network/UnixListing.h"

#include <QTimeZone>

namespace mole::net {
namespace {

    /// Month names are matched by hand rather than through QLocale: the text
    /// comes from a server or from libcurl, both of which always say "Sep",
    /// while QLocale would answer in whatever language the user is running in.
    int monthFromName(const QString& name)
    {
        static const char* const months[]
            = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
        const QString wanted = name.left(3).toLower();
        for (int i = 0; i < 12; ++i) {
            if (wanted == QLatin1String(months[i]))
                return i + 1;
        }
        return 0;
    }

    /// `ls` prints a time for recent entries and a year for older ones, and never
    /// both. A time therefore means "within the last six months or so", which is
    /// resolved against the current year -- and rolled back a year when that
    /// would put the entry in the future.
    QDateTime resolveTimestamp(int month, int day, const QString& yearOrTime, const QDateTime& now)
    {
        if (month == 0 || day <= 0)
            return {};

        if (yearOrTime.contains(QLatin1Char(':'))) {
            const QStringList parts = yearOrTime.split(QLatin1Char(':'));
            if (parts.size() < 2)
                return {};
            const QTime time(parts.at(0).toInt(), parts.at(1).toInt());
            QDateTime stamp(QDate(now.date().year(), month, day), time, now.timeZone());
            if (stamp > now.addDays(1))
                stamp = stamp.addYears(-1);
            return stamp;
        }

        const int year = yearOrTime.toInt();
        if (year <= 0)
            return {};
        return QDateTime(QDate(year, month, day), QTime(0, 0), now.timeZone());
    }

    bool looksLikePermissions(const QString& token)
    {
        if (token.size() < 10)
            return false;
        return QStringLiteral("-dlbcps").contains(token.at(0));
    }

} // namespace

ListingRow parseListingLine(const QString& line, const QDateTime& now)
{
    ListingRow row;

    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return row;

    // Walk the leading columns by hand, remembering where each one started, so
    // the name can be taken as the untouched remainder of the line. Splitting on
    // whitespace would lose every filename with a space in it.
    QStringList columns;
    QList<int> starts;
    int index = 0;
    while (index < trimmed.size() && columns.size() < 8) {
        while (index < trimmed.size() && trimmed.at(index).isSpace())
            ++index;
        if (index >= trimmed.size())
            break;
        const int start = index;
        while (index < trimmed.size() && !trimmed.at(index).isSpace())
            ++index;
        columns.append(trimmed.mid(start, index - start));
        starts.append(start);
    }

    if (columns.size() < 8 || !looksLikePermissions(columns.at(0)))
        return row;

    while (index < trimmed.size() && trimmed.at(index).isSpace())
        ++index;
    QString name = trimmed.mid(index);
    if (name.isEmpty())
        return row;

    const QString mode = columns.at(0);
    row.isDir = mode.at(0) == QLatin1Char('d');
    row.isSymlink = mode.at(0) == QLatin1Char('l');
    row.permissions = mode.mid(1, 9);
    row.owner = columns.at(2);
    row.group = columns.at(3);

    bool sizeOk = false;
    row.size = columns.at(4).toLongLong(&sizeOk);
    if (!sizeOk)
        row.size = 0;

    row.modified = resolveTimestamp(monthFromName(columns.at(5)), columns.at(6).toInt(), columns.at(7), now);

    // "link -> /somewhere/else": the name is the part before the arrow, and the
    // target is not something the listing layer has any use for.
    if (row.isSymlink) {
        const int arrow = name.indexOf(QStringLiteral(" -> "));
        if (arrow > 0)
            name = name.left(arrow);
    }

    row.name = name;
    row.valid = !name.isEmpty();
    return row;
}

bool isDotEntry(const ListingRow& row)
{
    return row.name == QLatin1String(".") || row.name == QLatin1String("..");
}

QList<ListingRow> parseUnixListing(const QByteArray& text, const QDateTime& now)
{
    QList<ListingRow> rows;
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const ListingRow row = parseListingLine(line, now);
        if (row.valid)
            rows.append(row);
    }
    return rows;
}

} // namespace mole::net
