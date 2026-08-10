#pragma once

#include "core/vfs/IFileSystem.h"
#include "core/vfs/VfsUri.h"

namespace mole {

/// The suffix a file wears while it is still being written.
///
/// A process killed outright cannot tidy up after itself. It does not get to
/// delete what it wrote, it does not get to finish, and whatever is on disk or
/// on the server at that instant stays there. So the protection cannot be an
/// action taken afterwards — it has to be the name the bytes were going under
/// all along. `report.pdf.mole-partial` is visibly not `report.pdf`: nothing
/// opens it by mistake, no sync treats it as the real thing, and finding what a
/// kill left behind is reading a listing rather than consulting a bookkeeping
/// file the same kill would have truncated.
///
/// One suffix for the whole product, local and remote alike, so that what a
/// user sees after a machine loses power means the same thing wherever they see
/// it. See ADR-0020 and ADR-0021.
inline constexpr QLatin1String kPartialWriteSuffix(".mole-partial");

/// Where a write to `target` goes before it is finished.
VfsUri partialWriteOf(const VfsUri& target);

/// Whether this is the wreckage of a write rather than a file somebody meant to
/// have.
bool isPartialWrite(const QString& name);

/// Puts a finished write under the name it was asked for.
///
/// Expressed against IFileSystem because every backend that needs it does the
/// same two things — check the destination is still free, then rename — and the
/// difference between a local disk, SFTP, FTP and WebDAV is entirely in how
/// those two calls are carried out.
///
/// A failure here is the write's failure, and leaves nothing behind: bytes under
/// a name nothing will ever open are litter, not a result.
///
/// `mayReplace` says whether the destination was **already there when the write
/// began**. It is the difference between the two things "the file exists" can
/// mean at this point: the caller is overwriting a file it knew about, which is
/// ordinary, or something arrived during the write, which is data this write was
/// never asked to touch and must not silently destroy. Only the caller knows
/// which, and only because it looked before it started.
VfsError commitPartialWrite(
    IFileSystem& fs, const VfsUri& staging, const VfsUri& target, bool mayReplace = false);

} // namespace mole
