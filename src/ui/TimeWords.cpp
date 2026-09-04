#include "ui/TimeWords.h"

#include <cstdlib>

namespace mole {
namespace {

    /// The distance itself, without a direction: "12 min", "3 h", "40 days".
    /// Empty for anything under a minute, which the callers say in words of their
    /// own ("just now", "in a moment").
    QString distanceInWords(qint64 seconds)
    {
        const qint64 magnitude = std::llabs(seconds);
        if (magnitude < 60)
            return {};
        if (magnitude < 3600)
            return QStringLiteral("%1 min").arg(magnitude / 60);
        if (magnitude < 86400)
            return QStringLiteral("%1 h").arg(magnitude / 3600);
        return QStringLiteral("%1 days").arg(magnitude / 86400);
    }

} // namespace

QString ageInWords(const QDateTime& when)
{
    if (!when.isValid())
        return QStringLiteral("never");

    const qint64 seconds = when.secsTo(QDateTime::currentDateTime());
    // A time in the future is a clock that disagrees with the file's, not a
    // sentence about the future: the index and the reports both record when they
    // ran, and a machine whose clock moved backwards should not read "in 3 days".
    if (seconds < 60)
        return QStringLiteral("just now");
    if (seconds < 172800)
        return distanceInWords(seconds) + QStringLiteral(" ago");
    if (seconds < 259200)
        return QStringLiteral("yesterday");
    return distanceInWords(seconds) + QStringLiteral(" ago");
}

QString timeInWords(const QDateTime& when, const QDateTime& now)
{
    if (!when.isValid())
        return QStringLiteral("never");

    const qint64 seconds = now.secsTo(when);
    const bool ahead = seconds > 0;
    const QString distance = distanceInWords(seconds);
    if (distance.isEmpty())
        return ahead ? QStringLiteral("in a moment") : QStringLiteral("just now");
    return ahead ? QStringLiteral("in %1").arg(distance) : distance + QStringLiteral(" ago");
}

} // namespace mole
