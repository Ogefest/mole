#pragma once

#include "sdk/PluginServices.h"

#include "core/vfs/FileEntry.h"
#include "core/vfs/VfsTypes.h"

#include <QImage>
#include <QList>
#include <QString>

namespace mole {

/// Makes a small picture of a file, for a view that shows what things look like
/// rather than what they are called.
///
/// One of the six on `PluginRegistry`, and the second that is not about showing
/// a file. (See the note in IMetadataReader.h: both called themselves the
/// fifth.)
/// Unlike the metadata readers -- where every reader that claims a file
/// contributes, because a container and its contents are two sets of facts about
/// one file -- **a file has one picture**, so the highest-priority thumbnailer
/// that claims it wins and the rest are not asked.
///
/// Three rules, and they are the whole design:
///
/// - **Off the UI thread, always.** `thumbnail()` runs on a `TaskManager` worker
///   like every other call that touches storage. Decoding a 24-megapixel
///   photograph on the GUI thread would freeze the window for exactly as long as
///   it takes, which on a folder of two hundred is not a number worth having.
/// - **Bounded.** What comes back is at most `size` pixels on its longest edge,
///   and a thumbnailer is expected to reach that without materialising the full
///   image where the format allows it.
/// - **Optional by design.** A null image is an ordinary answer: an unreadable
///   drive, a format nothing here can decode, a corrupt file, a 400 MB TIFF. The
///   tile keeps the icon it already had and nothing is put on screen about it.
///
/// See docs/adr/0058-a-file-can-say-what-it-looks-like.md.
class IThumbnailer
{
public:
    virtual ~IThumbnailer() = default;

    /// Stable id, for the registry and for a test that wants to know which one
    /// answered.
    virtual QString id() const = 0;
    /// Higher wins. A thumbnailer that knows the format is expected to sit above
    /// one that guesses.
    virtual int priority() const { return 0; }

    /// Cheap test on the name and the sniffed type. **Must not do any I/O**: this
    /// is called from the UI thread, once per file, while a folder is being laid
    /// out.
    virtual bool canThumbnail(const FileEntry& entry) const = 0;

    /// On a worker thread, and bounded: what comes back is at most `size` pixels
    /// on its longest edge. A null image means "no thumbnail", which is an
    /// ordinary answer and not an error.
    ///
    /// `cancel` must be polled: a folder scrolled past leaves its decodes
    /// pointless, and a view that cannot stop them is a view that is always
    /// behind.
    /// `services` by reference rather than by value, for the ABI reason spelled
    /// out beside IMetadataReader::read() and in ADR-0098.
    virtual QImage thumbnail(
        const FileEntry& entry, int size, const PluginServices& services, const CancelToken& cancel) const
        = 0;
};

/// Finds the one thumbnailer that will answer for a file. Null when none claims
/// it, which is most files.
class IThumbnailLookup
{
public:
    virtual ~IThumbnailLookup() = default;
    virtual IThumbnailer* thumbnailerFor(const FileEntry& entry) const = 0;
};

} // namespace mole
