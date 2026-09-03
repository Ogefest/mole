#pragma once

#include "core/vfs/FileAction.h"
#include "core/vfs/FileEntry.h"
#include "core/vfs/NameRules.h"
#include "core/vfs/VfsTypes.h"
#include "core/vfs/VfsUri.h"

#include <QIODevice>
#include <QMutex>
#include <QString>
#include <QThread>

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

    /// What this drive will accept in a name.
    ///
    /// The destination is what knows, so the destination is what answers. A FAT
    /// stick on Linux is stricter than the disk it is plugged into; an NTFS
    /// volume refuses a colon, a trailing dot and the MS-DOS device names; a
    /// bucket refuses almost nothing.
    ///
    /// Permissive by default, which is what every protocol backend is, and what
    /// this layer assumed silently before it could be asked.
    virtual NameRules nameRules() const { return {}; }

    /// Directory listing. Must poll `cancel` on slow backends.
    virtual Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) = 0;

    /// Metadata for a single node.
    virtual Result<FileEntry> stat(const VfsUri& target) = 0;

    // ---- Optional operations -------------------------------------------
    // The defaults return NotSupported so a new backend can start read-only
    // and grow. Advertise the matching VfsCapability when you override one.

    virtual Result<void> makeDirectory(const VfsUri& target);

    /// What a symbolic link points at, exactly as the drive stores it.
    ///
    /// **Not resolved.** A relative target comes back relative, and a target
    /// that is not there comes back all the same -- copying a link is copying
    /// the text, and a drive that helpfully answered with an absolute path would
    /// turn a relocatable tree into one pinned to where it was copied from. The
    /// caller that wants the node behind the link asks the drive about the
    /// link's own path, which every backend already follows.
    ///
    /// Fails with NotSupported on a drive with no links, and with NotALink for
    /// a path that is not one. See ADR-0092.
    virtual Result<QString> readLink(const VfsUri& link);

    /// Makes `link` a symbolic link pointing at `target`, which is stored as
    /// given and never checked: a link may legitimately point at nothing, and on
    /// a copy the thing it points at may not have arrived yet.
    ///
    /// Fails with AlreadyExists when the name is taken -- the same rule
    /// makeDirectory() follows, and for the same reason. See ADR-0092.
    virtual Result<void> makeLink(const VfsUri& link, const QString& target);
    virtual Result<void> remove(const VfsUri& target, bool recursive);
    virtual Result<void> rename(const VfsUri& from, const VfsUri& to);

    /// Puts `from` at `to`, replacing whatever is already there.
    ///
    /// The difference from rename() is the whole reason there are two of them.
    /// rename() refuses an occupied destination, and has to: a rename that
    /// silently destroyed a file nobody mentioned is how the only copy of
    /// something goes. This is the call for the case where the caller has
    /// already established that replacing is exactly what was asked for -- a
    /// finished write going over the file it was written to replace -- and says
    /// so by calling a different method rather than by passing a flag nobody
    /// reads.
    ///
    /// The default is what every caller used to do by hand: remove the
    /// destination, then rename onto the free name. It is all a protocol that
    /// has no atomic replace can offer, and between those two calls there is an
    /// instant at which the name has nothing at it. A backend that can do
    /// better overrides this -- on a local disk rename(2) replaces in one step,
    /// so the instant does not exist. See ADR-0087.
    ///
    /// The destination is removed non-recursively, so a directory with things
    /// in it is refused rather than emptied: throwing away a tree is a decision
    /// for a caller that means it, not a side effect of putting a file down.
    virtual Result<void> replace(const VfsUri& from, const VfsUri& to);

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

    // ---- What only this drive can do -------------------------------------
    //
    // The second tier, alongside VfsCapability rather than inside it. The enum
    // above holds what the core has to understand by name; these two hold what
    // only the user acts on, so nothing between here and the menu ever learns
    // what a particular drive brought. See FileAction.h and ADR-0075.

    /// What this drive can do to `target` that another drive could not.
    ///
    /// Empty by default, which is every backend in the tree: a drive that
    /// contributes nothing is asked nothing further, and needs no edit to say
    /// so. `entry` is what the listing already knows about the node -- whether
    /// it is a directory, how big it is, what it is called -- so a drive can
    /// rule an action out without a second round trip.
    ///
    /// Called on a worker thread like everything else here, so it may talk to
    /// the drive. It is asked about one node the user is looking at; whether a
    /// whole listing can be marked up is MOLE-198's question, not this one's.
    virtual FileActionList actionsFor(const VfsUri& target, const FileEntry& entry);

    /// Does one of them, and answers with one of exactly two kinds.
    ///
    /// `id` is one this drive handed out. An id it has never seen means the
    /// shell and the drive disagree about what is on offer, and the answer to
    /// that is NotSupported rather than the nearest thing the drive can think
    /// of -- which is why the default here refuses everything.
    virtual Result<FileActionOutcome> invoke(
        const QString& id, const VfsUri& target, const CancelToken& cancel);

    /// Whether this drive can read a uri that names an earlier state of a file.
    ///
    /// False by default, which is what makes refusing the default: a backend
    /// says no by implementing nothing. The refusal itself is not each backend's
    /// job -- see withVersionGuard(), which VfsManager puts on every mount.
    ///
    /// This is the whole risk of carrying a version in a uri. A backend that
    /// ignored a token it did not recognise would answer with the *current* file
    /// while the window says it is showing an earlier one, and a silent wrong
    /// answer on a screen whose entire purpose is to say which version you are
    /// looking at is the worst outcome available here. See ADR-0077.
    virtual bool understandsVersions() const { return false; }

    /// Which entries in `dir` this drive has an action for, by name.
    ///
    /// **One query for the folder, never one per row.** A folder of five
    /// thousand files must not become five thousand lookups on the path that
    /// draws -- and it need not, because both sources are naturally per
    /// directory: a filesystem exposing snapshots as paths lists the same
    /// relative path inside each snapshot, bounded by the number of snapshots,
    /// and an object store answers with one paginated call over a prefix.
    ///
    /// Empty by default, like actionsFor(), so a drive with nothing to offer
    /// does nothing. Called on a worker thread and must poll `cancel`: leaving
    /// the folder abandons the question.
    virtual Result<QStringList> entriesWithActions(const VfsUri& dir, const CancelToken& cancel);

    /// What this drive turned out to be able to offer, and whether it has been
    /// asked yet.
    ///
    /// Cheap, non-blocking and safe from any thread, including the one that
    /// draws: it reports what has already been discovered and never asks the
    /// drive. Unasked until something needs the answer -- see probe().
    virtual DriveOffers offers() const;

    /// Finds out, once, and remembers it for the life of this drive.
    ///
    /// Called from a worker thread when somebody first opens a folder here. The
    /// second call and every one after it costs nothing, and a call made while
    /// another thread is asking returns rather than asking again.
    ///
    /// Not at mount and not when a drive is configured: mounting must not get
    /// slower or fail for a capability nobody has asked for yet, a local volume
    /// is never configured at all -- SystemVolumes::enumerate() discovers it --
    /// and an answer written down once goes stale silently, a recorded "no"
    /// being the worst kind. ADR-0076 works through both.
    ///
    /// A probe that fails leaves the drive working and the offers absent. It is
    /// never anybody's error: whoever opened the folder asked for a listing.
    virtual void probe(const VfsUri& target, const CancelToken& cancel);

    /// Names the thread that draws the window, so that a call arriving from it
    /// says so instead of being noticed by somebody with a slow drive.
    ///
    /// ARCHITECTURE.md's first rule is that the interface never touches storage,
    /// and the header of this class says every method is called from a worker
    /// thread. Seven places did it anyway (MOLE-360): F2, F7, F3, a drag, bulk
    /// rename's listing, an archive open and two image headers. Each one is a
    /// window that stops for as long as a stalled mount takes to give up, and
    /// none of them was visible on a local disk.
    ///
    /// The same mechanism as IndexDatabase::doNotQueryFrom(), for the same reason
    /// (ADR-0066): it **warns rather than refusing**, because the answer such a
    /// call gives is correct and turning a slow window into a broken one is not
    /// an improvement -- and a test can then hold every route the interface has
    /// to silence. Pass nullptr to stop guarding.
    ///
    /// Static, and one per process: a backend is built per mount, while the
    /// thread that draws is the same one for all of them.
    static void doNotCallFrom(QThread* thread);

protected:
    static Result<void> notSupported(const char* what);

    /// Warns when the caller is the thread doNotCallFrom() named.
    ///
    /// Called by a backend at the top of each method that goes to storage. Not
    /// every backend does it, and it does not have to: what it buys is a test
    /// that can walk the interface's routes, so the backend the tests are driven
    /// against is the one that has to ask. MemoryFileSystem does.
    static void checkNotOnTheDrawingThread(const char* what);

    /// What this drive can offer at `target`, asked of the drive itself.
    ///
    /// Empty by default, which is every backend that has nothing to discover, so
    /// none of them needs to say so. Override it to ask the far end -- and take
    /// as long as it takes: nothing waits on this, and the listing that
    /// triggered it has already been answered.
    virtual Result<QStringList> askWhatIsOffered(const VfsUri& target, const CancelToken& cancel);

private:
    /// Held on the instance, which is what "for the life of the mount" means:
    /// VfsManager builds one backend per mount and lets go of it when the drive
    /// goes away, so a drive that is unmounted and mounted again is asked again.
    mutable QMutex m_offersMutex;
    DriveOffers m_offers;
    bool m_probing = false;
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
