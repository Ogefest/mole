#pragma once

#include "core/vfs/VfsUri.h"

namespace mole::thumbnails {

/// Whether a drive's reads are downloads.
///
/// The local filesystem is the only one that is exempt, and the scheme is what
/// decides. Every thumbnailer needs this, because what a picture costs to make is
/// entirely different on a bucket: on a metered one it is billed.
inline bool isRemote(const VfsUri& uri)
{
    return uri.scheme() != QLatin1String("file");
}

/// How much of a file is read while looking for something a camera wrote near the
/// front of it. The same bound the EXIF walker uses, because it is the same block.
inline constexpr qint64 kPrefixBytes = 64 * 1024;

} // namespace mole::thumbnails
