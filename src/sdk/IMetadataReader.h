#pragma once

#include "sdk/PluginServices.h"

#include "core/vfs/FileEntry.h"
#include "core/vfs/VfsTypes.h"

#include <QByteArrayView>
#include <QList>
#include <QString>

namespace mole {

/// One thing a file says about itself: "Camera", "Canon EOS 5D".
///
/// A label and a value, both already in the form a reader wants shown -- a
/// duration is "4:32" here rather than 272, because only the reader knows
/// whether a number is seconds, samples or frames.
struct FileFact
{
    QString label;
    QString value;
};

/// Reads what a file says about itself: a photograph's camera and exposure, a
/// document's author, a video's codecs, an audio file's tags.
///
/// The fifth extension point, and the first that is not winner-takes-all.
/// `PreviewRegistry` stops at the highest-priority provider that claims a file,
/// because a file can only be shown one way; **every reader that claims a file
/// contributes**, because a container and its contents are two different sets
/// of facts about one file and nothing is gained by making them compete. A zip
/// reader saying "37 entries" and an archive-of-a-document reader naming the
/// author are both right at once.
///
/// Nothing here runs on the GUI thread, and nothing here reads a whole file.
/// The head the type sniff already read is handed over, and a reader wanting
/// more asks for a bounded range of its own through the services -- an EXIF
/// block is a few kilobytes at a known offset, and a 40 GB video's duration is
/// in its first pages or its last.
class IMetadataReader
{
public:
    virtual ~IMetadataReader() = default;

    /// Stable, namespaced identifier.
    virtual QString id() const = 0;

    /// Readers run highest first, and their facts are shown in that order. A
    /// reader that knows the format exactly should outrank one that knows the
    /// container it happens to be in.
    virtual int priority() const { return 0; }

    /// Cheap test on the entry -- its name, its size, and the type the content
    /// pass found in `FileEntry::mimeType`. Must not do any I/O, for the same
    /// reason `IPreviewProvider::canPreview()` must not: it is asked of every
    /// reader for every file. See ADR-0033 for where the type comes from.
    virtual bool canRead(const FileEntry& entry) const = 0;

    /// The facts, on a worker thread.
    ///
    /// `head` is the first few kilobytes of the file, already read. `cancel` is
    /// the running task's token: a reader doing more than a moment's work is
    /// expected to check it, because the reader for the file somebody just
    /// stepped away from is worth nothing.
    ///
    /// Returning nothing is normal -- a reader that claimed a file and then
    /// found no tags in it costs its own rows and nobody else's.
    virtual QList<FileFact> read(
        const FileEntry& entry, QByteArrayView head, PluginServices services, const CancelToken& cancel) const
        = 0;
};

/// Finds the readers for a file. Implemented by the host and handed to plugins
/// through PluginServices, the way IPreviewLookup is.
class IMetadataLookup
{
public:
    virtual ~IMetadataLookup() = default;

    /// Every reader that claims `entry`, highest priority first.
    virtual QList<IMetadataReader*> readersFor(const FileEntry& entry) const = 0;
};

} // namespace mole
