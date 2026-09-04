#include "plugins/builtin/ImageFormats.h"

#include <QImageReader>

namespace mole {

QStringList imageSuffixes()
{
    // Once: the answer cannot change while the process is running, and both
    // callers ask it per file while a folder is being laid out.
    static const QStringList suffixes = [] {
        QStringList out;
        const QList<QByteArray> formats = QImageReader::supportedImageFormats();
        out.reserve(formats.size());
        for (const QByteArray& format : formats)
            out.append(QString::fromLatin1(format).toLower());
        return out;
    }();
    return suffixes;
}

} // namespace mole
