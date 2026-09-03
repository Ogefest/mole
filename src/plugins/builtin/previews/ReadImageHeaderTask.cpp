#include "plugins/builtin/previews/ReadImageHeaderTask.h"

#include <QImage>
#include <QImageReader>

namespace mole {

ReadImageHeaderTask::ReadImageHeaderTask(QString path, QObject* parent)
    : Task(QStringLiteral("Read the header of %1").arg(QString(path).section(QLatin1Char('/'), -1)), parent)
    , m_path(std::move(path))
{
    setBackground(true);
}

void ReadImageHeaderTask::run()
{
    QImageReader reader(m_path);
    const QSize pixels = reader.size();

    // What the decode would ask for, in the format the handler says it would
    // produce. One that will not say is assumed to want the widest, because
    // guessing low here is what shows an empty frame.
    int bits = 32;
    const QImage::Format format = reader.imageFormat();
    if (format != QImage::Format_Invalid)
        bits = qMax(1, int(QImage::toPixelFormat(format).bitsPerPixel()));

    emit headerRead(pixels, bits);
    if (pixels.isValid())
        setStatusText(QStringLiteral("%1 × %2").arg(pixels.width()).arg(pixels.height()));
}

} // namespace mole
