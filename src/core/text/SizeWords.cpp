#include "core/text/SizeWords.h"

#include <QLocale>

namespace mole {

QString sizeInWords(qint64 bytes)
{
    if (bytes < 0)
        return {};
    return QLocale().formattedDataSize(bytes);
}

QString rateInWords(double bytesPerSecond)
{
    if (bytesPerSecond < 0)
        return {};
    return QStringLiteral("%1/s").arg(sizeInWords(static_cast<qint64>(bytesPerSecond)));
}

} // namespace mole
