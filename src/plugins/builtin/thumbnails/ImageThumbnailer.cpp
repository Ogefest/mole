#include "plugins/builtin/thumbnails/ImageThumbnailer.h"

#include "plugins/builtin/previews/ImageMetadata.h"

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

namespace {

    /// A bounded prefix of the file, for looking at what a camera wrote near the
    /// front of it. One read, and never the rest of the file however large it is.
    QByteArray prefixOf(const FileSystemPtr& fs, const VfsUri& uri, qint64 bytes)
    {
        Result<std::unique_ptr<QIODevice>> opened = fs->openRead(uri, bytes);
        if (!opened.ok() || !opened.value())
            return {};
        return opened.value()->read(bytes);
    }

    /// Decodes `device` at no more than `size` pixels on its longest edge. The two
    /// calls that are the whole of the quality of this live here.
    QImage decodeBounded(QIODevice* device, int size, const CancelToken& cancel)
    {
        QImageReader reader(device);
        // In portrait if it was taken in portrait. Without this every picture from
        // a phone is on its side.
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
        if (image.isNull() || cancel.isCancelled())
            return {}; // corrupt, empty, or a format this build cannot decode

        // A format that cannot scale while reading comes back full size, so the
        // bound is applied here as well: what leaves this function is at most
        // `size` pixels on its longest edge, whatever route it took.
        if (image.width() > size || image.height() > size)
            image = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return image;
    }

} // namespace

QImage ImageThumbnailer::thumbnail(
    const FileEntry& entry, int size, PluginServices services, const CancelToken& cancel) const
{
    if (size <= 0 || !services.vfs || cancel.isCancelled())
        return {};

    FileSystemPtr fs = services.vfs->resolve(entry.uri);
    if (!fs)
        return {}; // an unplugged drive, which is an ordinary answer here

    const bool remote = thumbnails::isRemote(entry.uri);

    // On a drive where reading is downloading, the picture a camera already put
    // in the file comes first: almost every camera JPEG carries a 160x120
    // thumbnail in its first 64 kB, and that is a sixty-fourth of a megabyte
    // instead of eight. Softer than the tile, and it identifies the picture,
    // which is the whole job.
    //
    // Local files skip straight to the decode, which is the upgrade: reading the
    // whole file costs nothing there and the result is sharp.
    if (remote) {
        const QByteArray prefix = prefixOf(fs, entry.uri, thumbnails::kPrefixBytes);
        if (cancel.isCancelled())
            return {};
        if (const QByteArray embedded = ImageMetadataReader::embeddedThumbnail(prefix); !embedded.isEmpty()) {
            QBuffer buffer;
            buffer.setData(embedded);
            if (buffer.open(QIODevice::ReadOnly)) {
                if (QImage found = decodeBounded(&buffer, size, cancel); !found.isNull()) {
                    // **Turned upright before it leaves.** The small picture is
                    // stored the way the sensor read it, and the orientation that
                    // makes it upright is a tag in IFD0 -- not in the thumbnail's
                    // own stream, so QImageReader's autotransform on the decoded
                    // path cannot see it either. A portrait phone photograph came
                    // back sideways from a remote drive and upright locally,
                    // which is the same file answering two ways. See MOLE-385.
                    const int orientation = ImageMetadataReader::orientationOf(prefix);
                    if (orientation != 1)
                        found = found.transformed(ImageMetadataReader::transformFor(orientation));
                    return found;
                }
            }
        }
    }

    // The whole file, but only when it is worth fetching. A folder of RAW files on
    // a bucket shows tiles with names on them and costs nothing, and that is the
    // right outcome rather than an apology.
    //
    // A size of nought means nobody said how big it is. Locally that is fine --
    // reading it is cheap either way -- and remotely it is not: the point of this
    // ceiling is not to be surprised, and an unknown size is a surprise.
    const qint64 ceiling = remote ? kRemoteCeiling : kLocalCeiling;
    if (entry.size > ceiling || (remote && entry.size <= 0))
        return {};

    Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
    if (!opened.ok() || !opened.value())
        return {};
    QIODevice* device = opened.value().get();

    return decodeBounded(device, size, cancel);
}

} // namespace mole
