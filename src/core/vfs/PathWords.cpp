#include "core/vfs/PathWords.h"

#include <QUrl>

namespace mole {

QString withoutTrailingSlash(QString path)
{
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

QString withoutAnyTrailingSlash(QString path)
{
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

bool looksHidden(const QString& name)
{
    return name.startsWith(QLatin1Char('.'));
}

QString authorityFromLocalPath(const QString& path)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(path));
}

QString localPathFromAuthority(const QString& authority)
{
    return QUrl::fromPercentEncoding(authority.toLatin1());
}

} // namespace mole
