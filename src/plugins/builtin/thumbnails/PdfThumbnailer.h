#pragma once

#include "sdk/IThumbnailer.h"

namespace mole {

#ifdef MOLE_HAVE_QTPDF

/// The first page of a document, which is what a folder of PDFs is trying to tell
/// you apart by.
///
/// A wall of identical icons is what a folder of reports looks like without this,
/// and the application already renders these pages: `PdfPreviewController` shows
/// how, and this is the same `QPdfDocument` asked for page 0 at tile size and
/// nothing else.
///
/// A document that is encrypted or damaged returns nothing, which is an ordinary
/// answer -- the tile keeps its icon. Without Qt Pdf the thumbnailer is absent
/// altogether and a PDF gets the icon tile, exactly as it gets the information
/// viewer today.
class PdfThumbnailer final : public IThumbnailer
{
public:
    QString id() const override { return QStringLiteral("mole.thumb.pdf"); }
    /// Above the image thumbnailer, which would never claim a PDF anyway. Stated
    /// so that a plugin wanting to render these differently has somewhere to sit.
    int priority() const override { return 100; }

    bool canThumbnail(const FileEntry& entry) const override;
    QImage thumbnail(
        const FileEntry& entry, int size, PluginServices services, const CancelToken& cancel) const override;

    /// How big a document on a drive where reading is downloading is worth
    /// fetching. Unlike a photograph there is nothing small inside a PDF to read
    /// instead -- rendering page 0 needs the file -- so this is the only guard,
    /// and a 300 MB scanned book on a bucket is not worth a tile.
    static constexpr qint64 kRemoteCeiling = 32LL * 1024 * 1024;
    /// And where reading is not downloading, generous but not unbounded.
    static constexpr qint64 kLocalCeiling = 512LL * 1024 * 1024;
};

#endif // MOLE_HAVE_QTPDF

} // namespace mole
