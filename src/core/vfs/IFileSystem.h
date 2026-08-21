#pragma once

#include "core/vfs/FileEntry.h"
#include "core/vfs/VfsTypes.h"
#include "core/vfs/VfsUri.h"

#include <QIODevice>
#include <QString>

#include <chrono>
#include <memory>

namespace mole {

/// The one abstraction every "drive" implements -- local disk, SFTP, S3,
/// WebDAV, an archive, a database dump, whatever comes next. Add a backend by
/// implementing this plus IFileSystemFactory; nothing above this layer changes.
///
/// THREADING CONTRACT
/// ------------------
/// Every method here is synchronous and is only ever called from a worker
/// thread owned by TaskManager. The UI must never call a backend directly --
/// it submits a Task instead. That single rule is what keeps a stalled NFS
/// mount from freezing the window, and it means backends can be written in
/// plain blocking style with no async plumbing.
///
/// Implementations must be safe to call concurrently from several worker
/// threads, or serialise internally with their own mutex.
class IFileSystem
{
public:
    virtual ~IFileSystem() = default;

    /// Uri scheme this instance serves, e.g. "file", "sftp", "s3".
    virtual QString scheme() const = 0;

    /// What this backend actually supports. Callers must check before acting.
    virtual VfsCapabilities capabilities() const = 0;

    /// Whether two spellings that differ only in case are two nodes here.
    ///
    /// The volume is the only thing that really knows. S3 is case-sensitive and
    /// so is an ext4 disk; an NTFS volume is not, and an APFS one can be either.
    /// VfsUri can guess from the scheme and the platform, and a caller holding a
    /// backend should ask the backend instead -- which is what the guard against
    /// moving a directory into itself does, since getting that wrong deletes the
    /// only copy of everything underneath it.
    ///
    /// Case-sensitive by default: it is what every protocol backend is, and it
    /// is the answer that refuses rather than assumes.
    virtual Qt::CaseSensitivity pathCaseSensitivity() const { return Qt::CaseSensitive; }

    /// Directory listing. Must poll `cancel` on slow backends.
    virtual Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) = 0;

    /// Metadata for a single node.
    virtual Result<FileEntry> stat(const VfsUri& target) = 0;

    // ---- Optional operations -------------------------------------------
    // The defaults return NotSupported so a new backend can start read-only
    // and grow. Advertise the matching VfsCapability when you override one.

    virtual Result<void> makeDirectory(const VfsUri& target);
    virtual Result<void> remove(const VfsUri& target, bool recursive);
    virtual Result<void> rename(const VfsUri& from, const VfsUri& to);

    /// Opens a stream for reading. Caller owns the device and must close it.
    ///
    /// `expectedSize` is how many bytes the caller believes the file has, or -1
    /// when it does not know. It is a hint about how to fetch, never a limit on
    /// what is returned, and a backend is free to ignore it -- most do. A caller
    /// that has just listed the directory should pass what the listing said,
    /// because for a remote drive the difference between "a few kilobytes" and
    /// "twenty gigabytes" decides how the transfer has to be set up. Passing -1
    /// is always correct and always safe; it only costs speed.
    ///
    /// Every override repeats the default. A default argument binds to the
    /// static type, so an override that leaves it out compiles everywhere except
    /// at a call through the concrete class -- which is most of the test suite.
    virtual Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1);
    /// Opens a stream for writing. `expectedSize` is the same kind of hint as
    /// above and carries the same caveat about defaults in overrides: a backend
    /// that has to choose between one request and many needs to know roughly how
    /// much is coming, and -1 means the caller cannot say.
    virtual Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1);

    /// How much room this drive has. Only meaningful when ReportsSpace is
    /// advertised; everything else returns NotSupported and the interface
    /// simply says nothing about capacity.
    virtual Result<SpaceInfo> space(const VfsUri& target);

    /// Who may do what at `target`. Only meaningful when ReportsAccess is
    /// advertised; otherwise NotSupported, and the interface says nothing.
    virtual Result<AccessInfo> access(const VfsUri& target);

    /// Work this drive is still holding that no listing will ever show, older
    /// than `olderThan`. NotSupported unless ReportsLeftovers is advertised.
    ///
    /// **The age is not politeness, it is correctness.** Another copy of Mole --
    /// or another window of this one -- may have an upload in flight right now,
    /// and its parts look exactly like the ones a killed process left behind.
    /// Nothing distinguishes them but how long they have been there, which is
    /// why this cannot simply run on connect and why the threshold is the
    /// caller's to choose rather than a constant in here.
    virtual Result<QList<DriveLeftover>> leftovers(std::chrono::seconds olderThan, const CancelToken& cancel);
    /// Throws one away. The handle must be one this drive reported.
    virtual Result<void> discardLeftover(const DriveLeftover& leftover);

    /// Backend-side search. Only called when NativeSearch is advertised;
    /// otherwise the search feature walks the tree with list() instead.
    virtual Result<FileEntryList> search(
        const VfsUri& root, const QString& pattern, const CancelToken& cancel);

protected:
    static Result<void> notSupported(const char* what);
};

using FileSystemPtr = std::shared_ptr<IFileSystem>;

/// Implemented by write streams that only commit when they are closed.
///
/// Every remote backend is one of these: it stages the payload and sends it in
/// close(), because a signature needs the length up front and a half-sent object
/// is worse than none. QIODevice::close() returns void and cannot report that
/// the send failed, so the outcome is left here to be collected.
class ICommitsOnClose
{
public:
    virtual ~ICommitsOnClose() = default;
    /// Meaningful only after close(). ok() when the payload really landed.
    virtual VfsError commitError() const = 0;
};

/// Closes a write stream and says whether committing it actually worked.
///
/// Anything that writes through IFileSystem must finish with this rather than a
/// bare close(). On a buffered backend the bare call cannot fail, which means a
/// failed upload and a successful one look exactly alike -- and a copy that
/// silently did not happen is the worst outcome available.
Result<void> closeAndReport(QIODevice& device);

} // namespace mole
