#pragma once

#include "sdk/IThumbnailer.h"

namespace mole {

/// A picture of a picture, which is the case the gallery exists for.
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
    QImage thumbnail(
        const FileEntry& entry, int size, PluginServices services, const CancelToken& cancel) const override;

    /// What this Qt build can decode, asked of Qt rather than hard-coded: which
    /// formats exist depends on which image plugins are installed, and claiming
    /// one we cannot read would replace an icon with an empty tile.
    static QStringList imageSuffixes();
};

} // namespace mole
