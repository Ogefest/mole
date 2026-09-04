#pragma once

#include "plugins/builtin/thumbnails/ThumbnailLimits.h"
#include "sdk/IThumbnailer.h"

namespace mole {

/// A picture of a picture, which is the case the gallery exists for.
///
/// Three answers, in order, stopping at the first that works:
///
/// 1. **The thumbnail the camera already put in the file**, from a bounded prefix
///    read -- but only on a drive where reading is downloading, because that is
///    where the saving is. Softer than the tile, and it identifies the picture,
///    which is the whole job.
/// 2. **A decode of the whole file, scaled**, when the file is under a ceiling
///    worth fetching. On a local drive that is the first answer, because reading
///    it costs nothing and the result is sharp.
/// 3. **Nothing**, which is the icon tile, and is a correct answer rather than a
///    failure: a folder of RAW files on a bucket costs nothing and says so.
///
/// Two calls on QImageReader are the whole of the quality of this:
///
/// - `setScaledSize()`, so a 24-megapixel photograph is decoded *at the size
///   actually wanted*. For JPEG this reaches libjpeg's DCT scaler and the full
///   image is never materialised. Not every format can scale while reading --
///   PNG cannot -- so for those the image is decoded and then scaled, which is
///   correct and costs what a full decode costs. Nobody should assume the cheap
///   path everywhere.
/// - `setAutoTransform(true)`, so a photograph taken in portrait appears in
///   portrait. Without it every picture from a phone is on its side, and this is
///   the line that is always missing.
class ImageThumbnailer final : public IThumbnailer
{
public:
    QString id() const override { return QStringLiteral("mole.thumb.image"); }

    bool canThumbnail(const FileEntry& entry) const override;
    QImage thumbnail(const FileEntry& entry, int size, const PluginServices& services,
        const CancelToken& cancel) const override;

    /// What this Qt build can decode, asked of Qt rather than hard-coded: which
    /// formats exist depends on which image plugins are installed, and claiming
    /// one we cannot read would replace an icon with an empty tile.
    static QStringList imageSuffixes();

    /// How big a file on a drive that is not local is worth fetching **whole**
    /// when it has no thumbnail of its own inside it.
    ///
    /// This is the one number that protects a metered bucket. A folder of five
    /// hundred photographs on a network drive is four gigabytes, and making tiles
    /// by fetching all of it would be the most expensive thing this application
    /// has ever done. Eight megabytes is a generous camera JPEG; a RAW file or a
    /// panorama is past it and gets an icon, which is a correct answer and not a
    /// failure.
    static constexpr qint64 kRemoteCeiling = 8 * 1024 * 1024;

    /// And the same question where reading is not downloading. Generous, because a
    /// local read is cheap -- but not unbounded: a 400 MB TIFF decoded to make a
    /// 200-pixel tile is a minute of somebody's afternoon.
    static constexpr qint64 kLocalCeiling = 192LL * 1024 * 1024;
};

} // namespace mole
