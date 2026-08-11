#pragma once

#include "core/vfs/IFileSystem.h"

#include <QString>

#include <functional>
#include <memory>

namespace mole::test {

/// A drive that misbehaves on purpose, wrapped around one that does not.
///
/// Three fakes were written in a single day to reproduce three faults, each one
/// forty lines of delegation around a fourth line that did the misbehaving. This
/// is that fourth line, once, with the delegation written down only here -- the
/// same shape as LoggingFileSystem, which wraps every real mount, and for the
/// same reason: a wrapper written once works over local disk, memory, SFTP and
/// whatever a plugin brings.
///
/// **Every fault fires at a byte offset, never on a clock.** "The connection
/// drops after 30% of the file" is a fact about the transfer; "the connection
/// drops after 200 ms" is a fact about the machine the test happens to run on,
/// and it passes here and fails on the build server. So the wrapper counts bytes
/// through each stream and acts when the count arrives -- and it clamps each
/// read to land exactly on the next offset, so a fault at byte 1200 fires at
/// byte 1200 whatever chunk size the caller happens to use.
///
/// The one fault that cannot be a byte offset is a stall, because a stall is by
/// definition the absence of an event. It still is not a clock: the stream stops
/// at its offset and stays stopped until the test calls release(), and the test
/// waits for isStalled() rather than for a duration.
///
/// ```
/// auto source = std::make_shared<FaultyFileSystem>(disk);
/// source->readFailsAt(4);                   // hands over four bytes, then dies
/// source->fileVanishesAt(1200);             // deleted underneath the reader
/// source->readStallsAt(600);                // ... until release()
/// ```
///
/// Declarations with the same offset fire in the order they were made, which is
/// what lets an action and a failure be declared together.
class FaultyFileSystem final : public IFileSystem
{
public:
    explicit FaultyFileSystem(FileSystemPtr inner);
    ~FaultyFileSystem() override;

    /// The backend underneath, for a test that wants to look at what really
    /// landed without the faults getting in the way.
    const FileSystemPtr& inner() const { return m_inner; }

    // ---- what goes wrong while reading -----------------------------------
    // `path` limits a fault to one file; the default is every file opened
    // through this wrapper, which is what a test with one file wants.

    /// Hands over `offset` bytes and then fails. A dropped connection.
    FaultyFileSystem& readFailsAt(qint64 offset, const QString& path = {},
        VfsError::Code code = VfsError::NetworkError,
        const QString& message = QStringLiteral("the connection went away"));

    /// Returns `chunk` bytes instead of what was asked for, once, and then
    /// carries on. Ordinary streaming behaviour, and a caller that treats it as
    /// the end of the file loses the rest.
    FaultyFileSystem& readGoesShortAt(qint64 offset, qint64 chunk = 1, const QString& path = {});

    /// Stops at `offset` and stays stopped until release(). What a stall guard
    /// is for, and the only way to hold a transfer still long enough to do
    /// something to it.
    FaultyFileSystem& readStallsAt(qint64 offset, const QString& path = {});

    /// Runs `action` when the reader reaches `offset`, once, before any further
    /// byte moves. The general form the named faults below are built from.
    FaultyFileSystem& whenReadReaches(qint64 offset, std::function<void()> action, const QString& path = {});

    // ---- what goes wrong while writing -----------------------------------

    /// Takes `offset` bytes and refuses the rest. The bytes before the offset
    /// really are written, because that is what a failing write does.
    FaultyFileSystem& writeFailsAt(qint64 offset, const QString& path = {},
        VfsError::Code code = VfsError::IoError,
        const QString& message = QStringLiteral("the write was refused"));

    /// The destination runs out of room at `offset`.
    FaultyFileSystem& destinationFillsAt(qint64 offset, const QString& path = {});

    /// Reports every byte as written and passes on only every Nth one. A server
    /// that acknowledges and loses, which is the failure that leaves a backup
    /// looking like a backup.
    FaultyFileSystem& writeKeepsEveryNth(int keepEvery, const QString& path = {});

    /// Accepts everything and fails when closed, having stored nothing. Every
    /// remote backend's commit path: the payload is staged and sent in close(),
    /// so the failure arrives after the last successful write().
    FaultyFileSystem& writeFailsOnClose(const QString& message = QStringLiteral("the server hung up"));

    /// Runs `action` when the writer reaches `offset`, once. The write then
    /// carries on, so this is how the destination is interfered with rather than
    /// how it is failed.
    FaultyFileSystem& whenWriteReaches(qint64 offset, std::function<void()> action, const QString& path = {});

    // ---- what the file does underneath the reader -------------------------

    /// The file becomes `newSize` bytes when the reader reaches `offset`, and
    /// the reader sees the end of the file there. Nothing about a read that
    /// ends early says whether the file shrank or the connection lied.
    FaultyFileSystem& fileChangesSizeAt(qint64 offset, qint64 newSize, const QString& path = {});

    /// The file is deleted at `offset` and the read fails from there on, which
    /// is what a backend does when the object it was fetching stops existing.
    FaultyFileSystem& fileVanishesAt(qint64 offset, const QString& path = {});

    /// The file is renamed within its own directory at `offset`; as above, the
    /// read cannot continue.
    FaultyFileSystem& fileIsRenamedAt(qint64 offset, const QString& newName, const QString& path = {});

    /// Permission is withdrawn at `offset`: the read fails, and every later
    /// operation on this drive -- listing, stat, open, remove -- is denied.
    FaultyFileSystem& accessRevokedAt(qint64 offset, const QString& path = {});

    // ---- what goes wrong while listing ------------------------------------

    /// Stops before listing `path` and stays stopped until release().
    ///
    /// A walk opens no files, so a read stall never fires on one -- and a scan
    /// held still is the only honest way to look at what a half-finished scan
    /// has done to the index. Waiting is on isStalled(), as it is for a read.
    FaultyFileSystem& listStalls(const QString& path);

    // ---- what goes wrong while deleting -----------------------------------

    /// Refuses to delete `path`, or everything when it is empty.
    ///
    /// The dangerous case rather than an inconvenient one: the copy that came
    /// before it worked, so a move whose delete fails has to leave both copies
    /// and say so. Deleting the source anyway, or reporting the move as clean,
    /// are the two ways this loses a file.
    FaultyFileSystem& removeFails(const QString& path = {}, VfsError::Code code = VfsError::AccessDenied,
        const QString& message = QStringLiteral("permission denied"));

    // ---- what the drive says about itself ---------------------------------

    /// Every file claims to be this many bytes bigger than it is, in a listing
    /// and in a stat alike -- a plan is built from both, and a plan built from a
    /// listing is built from a claim.
    FaultyFileSystem& listingOverstatesSizeBy(qint64 bytes);

    /// The drive stops advertising RandomAccessRead: it can be read from the
    /// beginning and nowhere else. Not an exotic fault -- it is what a backend
    /// streaming over a socket really is, and every caller that asks for a window
    /// in the middle of a file has to answer for what it does when refused.
    FaultyFileSystem& cannotSeek();

    // ---- what went through it ---------------------------------------------
    //
    // The wrapper already counts every byte through every stream to fire its
    // faults, so saying how many there were costs nothing -- and it is the only
    // way to hold "the viewer read one page and stopped" to account. A bound on
    // what was read is as much a fact about a preview as what it showed.

    /// How many times a file was opened for reading through this wrapper.
    int openReadCount() const;
    /// Every byte handed to a reader through this wrapper, over all streams.
    qint64 bytesRead() const;
    /// What each of those streams delivered, in the order they were opened.
    /// One read of one page followed by one of a window is a different fact
    /// from one read of the two together, and only this can tell them apart.
    QList<qint64> readSizes() const;

    // ---- the stall, from the test's side ----------------------------------

    /// Whether a stream is sitting at a readStallsAt() offset right now. Wait
    /// for this, never for a duration.
    bool isStalled() const;
    /// Lets every stalled stream go, and any that stalls afterwards.
    void release();

    // ---- IFileSystem -------------------------------------------------------

    QString scheme() const override;
    VfsCapabilities capabilities() const override;
    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;
    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;
    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<SpaceInfo> space(const VfsUri& target) override;
    Result<AccessInfo> access(const VfsUri& target) override;
    Result<FileEntryList> search(
        const VfsUri& root, const QString& pattern, const CancelToken& cancel) override;

    /// The declared faults and the state shared with the open streams. Public
    /// because the stream devices need it and they are an implementation
    /// detail of this file; there is nothing here for a test to touch.
    struct Policy;

private:
    FileSystemPtr m_inner;
    std::shared_ptr<Policy> m_policy;
};

} // namespace mole::test
