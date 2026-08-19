#include "plugins/builtin/thumbnails/PdfThumbnailer.h"

#ifdef MOLE_HAVE_QTPDF

#include "plugins/builtin/thumbnails/ThumbnailLimits.h"

#include "core/vfs/VfsManager.h"

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
    const FileEntry& entry, int size, PluginServices services, const CancelToken& cancel) const
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

    // The device has to outlive the document: QPdfDocument reads through it while
    // rendering rather than taking a copy.
    std::unique_ptr<QIODevice> device = std::move(opened.value());
    QPdfDocument document;
    // load(QIODevice*) answers through the status rather than a return value, and
    // a device is read synchronously -- there is no asynchronous state to wait on.
    document.load(device.get());
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
