#include "plugins/builtin/TimeWords.h"

namespace mole {

QString ageInWords(const QDateTime& when)
{
    if (!when.isValid())
        return QStringLiteral("never");

    const qint64 seconds = when.secsTo(QDateTime::currentDateTime());
    if (seconds < 120)
        return QStringLiteral("just now");
    if (seconds < 7200)
        return QStringLiteral("%1 minutes ago").arg(seconds / 60);
    if (seconds < 172800)
        return QStringLiteral("%1 hours ago").arg(seconds / 3600);
    return QStringLiteral("%1 days ago").arg(seconds / 86400);
}

} // namespace mole
