#include "plugins/builtin/thumbnails/PdfThumbnailer.h"

#ifdef MOLE_HAVE_QTPDF

#include "plugins/builtin/thumbnails/ThumbnailLimits.h"

#include "core/vfs/VfsManager.h"

#include <QBuffer>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>

namespace mole {

bool PdfThumbnailer::canThumbnail(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;
    // No I/O: the name and the type the listing already sniffed, because this is
    // asked once per file while a folder is being laid out.
    return entry.uri.suffix() == QLatin1String("pdf") || entry.mimeType == QLatin1String("application/pdf");
}

QImage PdfThumbnailer::thumbnail(
    const FileEntry& entry, int size, const PluginServices& services, const CancelToken& cancel) const
{
    if (size <= 0 || !services.vfs || cancel.isCancelled())
        return {};

    const qint64 ceiling = thumbnails::isRemote(entry.uri) ? kRemoteCeiling : kLocalCeiling;
    if (entry.size > ceiling)
        return {};

    FileSystemPtr fs = services.vfs->resolve(entry.uri);
    if (!fs)
        return {};

    Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
    if (!opened.ok() || !opened.value())
        return {};

    // **Into a buffer first, because a remote device is sequential.**
    //
    // QPdfDocument::load(QIODevice*) answers through the status rather than a
    // return value, and the comment here used to say a device is read
    // synchronously -- which is true of a file and false of a stream. For a
    // sequential device Qt connects to readyRead and finishes on an event loop,
    // and a pool thread runs none: the status stayed Loading, the tile came back
    // empty, and the download the ceiling above had just paid for bought
    // nothing. A ceiling that costs the fetch and yields no picture is the worst
    // of both. See MOLE-385.
    //
    // Bounded by the ceiling already applied above, so what is held is at most
    // that -- and only for as long as the render takes.
    std::unique_ptr<QIODevice> device = std::move(opened.value());
    QBuffer buffer;
    QIODevice* source = device.get();
    if (device->isSequential()) {
        QByteArray whole;
        whole.reserve(int(qMin<qint64>(entry.size > 0 ? entry.size : 0, ceiling)));
        while (whole.size() <= ceiling) {
            if (cancel.isCancelled())
                return {};
            const QByteArray piece = device->read(1 << 20);
            if (piece.isEmpty())
                break;
            whole += piece;
        }
        if (whole.isEmpty() || whole.size() > ceiling)
            return {};
        buffer.setData(whole);
        if (!buffer.open(QIODevice::ReadOnly))
            return {};
        source = &buffer;
    }

    QPdfDocument document;
    document.load(source);
    if (document.status() != QPdfDocument::Status::Ready || document.error() != QPdfDocument::Error::None)
        return {}; // encrypted, damaged, or not a PDF after all
    if (document.pageCount() <= 0 || cancel.isCancelled())
        return {};

    // Page 0 at tile size, in the page's own shape. A document is portrait far
    // more often than not, and squashing it to a square would make every tile in
    // a folder of reports look the same again.
    const QSizeF points = document.pagePointSize(0);
    const double aspect
        = points.height() > 0 && points.width() > 0 ? points.height() / points.width() : 1.414; // A4 upright
    QSize target = aspect >= 1.0 ? QSize(qMax(1, qRound(size / aspect)), size)
                                 : QSize(size, qMax(1, qRound(size * aspect)));
    const QImage rendered = document.render(0, target, QPdfDocumentRenderOptions {});
    if (rendered.isNull() || cancel.isCancelled())
        return {};

    // Bounded whatever the renderer did with the size it was given.
    if (rendered.width() > size || rendered.height() > size)
        return rendered.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return rendered;
}

} // namespace mole

#endif // MOLE_HAVE_QTPDF
