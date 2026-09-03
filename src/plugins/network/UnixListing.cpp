#include "plugins/network/UnixListing.h"

#include <QTimeZone>

#include <algorithm>

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

QList<ListingRow> parseMlsdListing(const QByteArray& text, const QDateTime& now)
{
    Q_UNUSED(now)
    QList<ListingRow> rows;
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        // Facts, a single space, then the pathname -- which may itself contain
        // spaces, so the split is at the *first* one and nothing else.
        const int space = line.indexOf(QLatin1Char(' '));
        if (space <= 0)
            continue;

        ListingRow row;
        row.name = line.mid(space + 1);
        if (row.name.isEmpty())
            continue;

        QString type;
        qint64 size = -1;
        for (const QString& fact : line.left(space).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const int equals = fact.indexOf(QLatin1Char('='));
            if (equals <= 0)
                continue;
            const QString key = fact.left(equals).toLower();
            const QString value = fact.mid(equals + 1);

            if (key == QLatin1String("type")) {
                type = value.toLower();
            } else if (key == QLatin1String("size") || key == QLatin1String("sizd")) {
                bool ok = false;
                const qint64 number = value.toLongLong(&ok);
                if (ok)
                    size = number;
            } else if (key == QLatin1String("modify")) {
                // YYYYMMDDHHMMSS, in UTC and with an optional fraction. Said to be
                // UTC by the standard, which is the whole reason this format is
                // worth asking for: an `ls -l` date is in the server's timezone
                // and carries no year for anything older than six months.
                QDateTime when = QDateTime::fromString(value.left(14), QStringLiteral("yyyyMMddHHmmss"));
                if (when.isValid()) {
                    when.setTimeSpec(Qt::UTC);
                    row.modified = when.toLocalTime();
                }
            } else if (key == QLatin1String("unix.mode")) {
                bool ok = false;
                const uint mode = value.toUInt(&ok, 8);
                if (ok) {
                    const char* const bits[] = { "---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx" };
                    row.permissions = QLatin1String(bits[(mode >> 6) & 7])
                        + QLatin1String(bits[(mode >> 3) & 7]) + QLatin1String(bits[mode & 7]);
                }
            }
        }

        // A row with no type fact says nothing about what it is, and guessing is
        // what the human format already does badly.
        if (type.isEmpty())
            continue;
        if (type == QLatin1String("cdir"))
            row.name = QStringLiteral(".");
        else if (type == QLatin1String("pdir"))
            row.name = QStringLiteral("..");

        row.isDir
            = type == QLatin1String("dir") || type == QLatin1String("cdir") || type == QLatin1String("pdir");
        row.isSymlink = type.startsWith(QLatin1String("os.unix=slink"));
        row.size = row.isDir ? 0 : std::max<qint64>(0, size);
        row.valid = true;
        rows.append(row);
    }
    return rows;
}

} // namespace mole::net
