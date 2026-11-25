#include "core/text/DateWords.h"

namespace mole {

QString dateTimeInWords(const QDateTime& when)
{
    if (!when.isValid())
        return {};
    return when.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString dateInWords(const QDate& day)
{
    if (!day.isValid())
        return {};
    return day.toString(QStringLiteral("yyyy-MM-dd"));
}

QString clockInWords(const QTime& time)
{
    if (!time.isValid())
        return {};
    return time.toString(QStringLiteral("HH:mm"));
}

} // namespace mole
