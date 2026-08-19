#include "plugins/builtin/thumbnails/ImageThumbnailer.h"

#include "core/vfs/VfsManager.h"

#include <QBuffer>
#include <QImageReader>

namespace mole {

QStringList ImageThumbnailer::imageSuffixes()
{
    QStringList suffixes;
    const QList<QByteArray> formats = QImageReader::supportedImageFormats();
    suffixes.reserve(formats.size());
    for (const QByteArray& format : formats)
        suffixes.append(QString::fromLatin1(format).toLower());
    return suffixes;
}

bool ImageThumbnailer::canThumbnail(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;
    // No I/O: the suffix and nothing else, because this is asked once per file
    // while a folder is being laid out.
    static const QStringList supported = imageSuffixes();
    return supported.contains(entry.uri.suffix());
}

QImage ImageThumbnailer::thumbnail(
    const FileEntry& entry, int size, PluginServices services, const CancelToken& cancel) const
{
    if (size <= 0 || !services.vfs || cancel.isCancelled())
        return {};

    FileSystemPtr fs = services.vfs->resolve(entry.uri);
    if (!fs)
        return {}; // an unplugged drive, which is an ordinary answer here

    Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
    if (!opened.ok() || !opened.value())
        return {};
    QIODevice* device = opened.value().get();

    QImageReader reader(device);
    // In portrait if it was taken in portrait. Without this every picture from a
    // phone is on its side.
    reader.setAutoTransform(true);

    const QSize full = reader.size();
    if (full.isValid() && !full.isEmpty()) {
        // Decoded at the size actually wanted rather than decoded and then
        // thrown away: for JPEG this is libjpeg's own scaler.
        const QSize bounded = full.scaled(size, size, Qt::KeepAspectRatio);
        reader.setScaledSize(bounded.isEmpty() ? QSize(1, 1) : bounded);
    }

    if (cancel.isCancelled())
        return {};

    QImage image = reader.read();
    if (image.isNull())
        return {}; // corrupt, empty, or a format this build cannot decode
    if (cancel.isCancelled())
        return {};

    // A format that cannot scale while reading comes back full size, so the
    // bound is applied here as well: what leaves this function is at most `size`
    // pixels on its longest edge, whatever route it took.
    if (image.width() > size || image.height() > size)
        image = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

} // namespace mole
