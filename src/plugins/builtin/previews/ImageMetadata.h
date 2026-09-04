#pragma once

#include "sdk/IMetadataReader.h"

namespace mole {

/// What a photograph says about itself: its size, and what the camera wrote.
///
/// **A header read, never a decode.** Dimensions come from `QImageReader` over a
/// buffer of the file's first bytes, and the EXIF block is walked where it lies.
/// A 60 MB raw file costs the same kilobytes as a thumbnail, which is the whole
/// point -- opening a folder of photographs must not decode a folder of
/// photographs.
///
/// **Every offset in the file is a claim, not a promise.** An IFD entry says
/// where its value is and how long it is, and a corrupt or hostile file says
/// whatever it likes. Each one is checked against the buffer before it is
/// followed, and a pointer that leads outside costs that tag and nothing else --
/// no read, no growing the buffer to reach it, no crash. Same lesson as
/// ADR-0010's archive entries.
///
/// **Nothing goes on the network.** GPS is shown as the numbers in the file. A
/// preview that looked a coordinate up would tell somebody else that a
/// photograph had been looked at, which is the rule ADR-0006 already states for
/// rendered HTML and is not restricted to it.
class ImageMetadataReader final : public IMetadataReader
{
public:
    QString id() const override { return QStringLiteral("mole.metadata.image"); }
    /// Above the generic reader, so the picture's own facts come first.
    int priority() const override { return 100; }
    bool canRead(const FileEntry& entry) const override;
    QList<FileFact> read(const FileEntry& entry, QByteArrayView head, PluginServices services,
        const CancelToken& cancel) const override;

    /// The most the reader will ever hold of an image. Every header worth
    /// reading, and every EXIF block a camera writes, is well inside it.
    static constexpr qint64 kHeaderBytes = 64 * 1024;

    /// Everything derivable from a prefix of an image file, with no I/O at all.
    /// Public because this is where the risk is and it deserves to be tested
    /// with bytes a test wrote rather than through a file, a drive and a tab.
    static QList<FileFact> factsFor(QByteArrayView bytes, const QString& fileName);

    /// Whether a prefix is too short to answer with -- the dimensions are not in
    /// it yet, or the EXIF block it points at runs past the end.
    static bool wantsMore(QByteArrayView bytes, const QString& fileName);

    /// The small picture a camera wrote into IFD1 of this file's EXIF block, as
    /// the JPEG bytes it is, or nothing when there is not one inside `bytes`.
    ///
    /// Public because the gallery needs exactly this and needs it to be the same
    /// walker: almost every camera JPEG carries a 160x120 thumbnail in its first
    /// 64 kB, and reading that instead of the whole file is the difference between
    /// a sixty-fourth of a megabyte and eight on a drive where reading is
    /// downloading. See MOLE-143.
    ///
    /// The offset and the length IFD1 gives are claims like every other offset
    /// here: both are checked against the buffer before anything follows them, so
    /// a file pointing outside itself costs this thumbnail and nothing else.
    static QByteArray embeddedThumbnail(QByteArrayView bytes);

    /// The EXIF orientation this file claims, 1 to 8, and 1 when it says
    /// nothing.
    ///
    /// **IFD0 tag 0x0112, which is not in the thumbnail.** The small picture
    /// lives in IFD1 and is stored the way the sensor read it -- the orientation
    /// that makes it upright is a tag in the *main* directory, and this reader
    /// was already reading it for the panel and not handing it to anybody. So a
    /// portrait phone photograph came back sideways from a remote drive and
    /// upright locally, where the whole file is decoded and QImageReader's own
    /// autotransform reads the same tag. See MOLE-385.
    static int orientationOf(QByteArrayView bytes);

    /// The transform that turns a picture stored with `orientation` upright.
    /// Identity for 1 and for anything outside 1..8.
    static QTransform transformFor(int orientation);
};

} // namespace mole
