#pragma once

#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

#include <QHash>
#include <QMutex>
#include <QSemaphore>
#include <QStringList>

#include <atomic>
#include <memory>

namespace mole {

/// A complete filesystem living in RAM.
///
/// It exists for two reasons. It is a real scratch drive the user can mount,
/// and it is the backend the test suite runs almost everything against --
/// deterministic, fast, and able to fake the failures that are impossible to
/// reproduce on demand with real hardware (see setFault()).
/// IFileSystem is already `enable_shared_from_this`, so this must not be it a
/// second time -- two bases would make `weak_from_this()` ambiguous. The write
/// device below needs a weak pointer to the *concrete* class, and gets one by
/// down-casting what IFileSystem::sharedSelf() hands back. See MOLE-364.
class MemoryFileSystem final : public IFileSystem
{
public:
    MemoryFileSystem();

    QString scheme() const override { return QStringLiteral("mem"); }
    VfsCapabilities capabilities() const override;
    Qt::CaseSensitivity pathCaseSensitivity() const override { return m_caseSensitivity; }
    NameRules nameRules() const override { return m_nameRules; }

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;
    Result<void> makeDirectory(const VfsUri& target) override;
    Result<QString> readLink(const VfsUri& link) override;
    Result<void> makeLink(const VfsUri& link, const QString& target) override;
    Result<void> remove(const VfsUri& target, bool recursive, const CancelToken& cancel = {}) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to, const CancelToken& cancel = {}) override;
    Result<std::unique_ptr<QIODevice>> openRead(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& cancel = {}) override;
    Result<std::unique_ptr<QIODevice>> openWrite(
        const VfsUri& target, qint64 expectedSize = -1, const CancelToken& cancel = {}) override;

    // ---- test / fixture helpers -----------------------------------------

    /// Creates a file and every missing parent directory.
    void addFile(const QString& path, const QByteArray& contents = {}, const QDateTime& modified = {});
    void addDirectory(const QString& path);
    /// Dates an entry, folders included. A tree built a moment ago is a tree
    /// nothing can tell apart from one changed a moment ago, and an incremental
    /// scan is entirely about that difference.
    void setModified(const QString& path, const QDateTime& when);

    /// Behaves like a volume that does not distinguish case -- NTFS, or a
    /// default APFS one. This is how a rule that only bites on such a volume is
    /// held on any machine, without needing one.
    ///
    /// Case-preserving as well as case-insensitive, because that is what those
    /// volumes are: a name is stored as it was written and found however it is
    /// spelled. A fixture that folded names on the way in would pass a rename
    /// this whole exercise is about and report the wrong name afterwards.
    void setCaseSensitivity(Qt::CaseSensitivity sensitivity) { m_caseSensitivity = sensitivity; }

    /// Marks an existing entry as a link, or as a Windows shortcut.
    ///
    /// The two are one thing to QFileInfo and must not be one thing here, or the
    /// rule that tells them apart is only checkable on a machine that has .lnk
    /// files. Neither changes what the entry contains: a shortcut really is an
    /// ordinary file, and what a link points at is a question nothing asks yet.
    void markAsSymlink(const QString& path);
    /// Puts a symbolic link at `path` pointing at `target`, which is stored as
    /// given: a link to nothing is a link, and the fixtures need one.
    void addSymlink(const QString& path, const QString& target);
    void markAsShortcut(const QString& path);

    /// Refuses the names a real volume of that kind would refuse, so what a
    /// transfer and a rename preview do about one is held on any machine.
    void setNameRules(const NameRules& rules) { m_nameRules = rules; }

    /// Makes every operation touching `path` fail with `error`. Pass
    /// VfsError::None to clear. This is how the tests cover "the NAS went away
    /// half way through a scan" without needing a NAS.
    void setFault(const QString& path, VfsError::Code error);
    void clearFaults();

    /// Sleeps this long inside list() -- used to test cancellation and to keep
    /// the UI honest about slow backends.
    void setListDelayMs(int ms) { m_listDelayMs = ms; }
    /// Holds every listing until the test releases the semaphore, and says how
    /// many are being held.
    ///
    /// A condition rather than a clock, which is the difference between a test
    /// that says what it means and one that passes on this machine: "a drive that
    /// has stopped answering" is a listing that has not come back, not a listing
    /// that takes 200 ms. A mount whose server has gone waits for the kernel's
    /// timeout -- about fifteen minutes for a hard NFS mount, or for ever -- and
    /// there is no duration a suite can stand in for that. See MOLE-362.
    ///
    /// Null clears it. A held listing is inside list() with the drive's own mutex
    /// *not* taken, so the rest of the drive still answers -- which is what a real
    /// blocking call does.
    void setListGate(std::shared_ptr<QSemaphore> gate);
    /// How many listings are being held by the gate right now. Wait for this,
    /// never for a duration.
    int listsInProgress() const;
    /// The same gate for openRead(), because a task that has to be caught *inside*
    /// a read -- cancelled while it is holding a file open -- cannot be caught by
    /// waiting a while and hoping. Null clears it.
    void setReadGate(std::shared_ptr<QSemaphore> gate);
    /// How many reads are being held right now, and how many have been reached
    /// at all. Both are conditions to wait on rather than durations.
    int readsInProgress() const;
    int readCount() const;
    /// The same for openRead(), because the honest way to test what a view does
    /// while a file is slow to arrive is to have one that is.
    void setReadDelayMs(int ms) { m_readDelayMs = ms; }
    /// Sleeps this long inside *every* operation that goes to storage: stat,
    /// makeDirectory, remove, rename, and the two openers as well as list.
    ///
    /// For the other kind of claim about a slow drive -- not "the view says so
    /// while it waits" but "the window did not wait at all". A gesture that takes
    /// a second on a share that takes a second is a gesture made on the thread
    /// that draws, and that is what tst_DrivesOffTheDrawingThread measures. See
    /// MOLE-360.
    void setOperationDelayMs(int ms) { m_operationDelayMs = ms; }
    /// Hands the bytes over at a pace: no more than `bytesPerRead` per read, and
    /// `delayMs` before each one.
    ///
    /// Unlike the two above this makes a read take time *while bytes are moving*,
    /// which is the only way to have a transfer that is genuinely in flight --
    /// with a speed to measure and a bar between the ends -- without moving enough
    /// real bytes to make the suite slow. A delay before the file arrives cannot
    /// do it: the copy is still instant once it starts.
    void setReadThrottle(qint64 bytesPerRead, int delayMs)
    {
        m_throttleBytes = bytesPerRead;
        m_throttleDelayMs = delayMs;
    }

    int listCallCount() const;

    // ---- what a probe of this drive finds --------------------------------
    //
    // A drive's extra capabilities are a property of what it was pointed at, so
    // a fixture has to be able to be pointed at either kind of thing: a volume
    // that keeps earlier states of a file and one that does not, a far end that
    // answers and one that does not.

    /// What a probe will answer. Ids, in the namespace FileAction::id uses.
    void setOffers(const QStringList& ids) { m_offers = ids; }
    /// Makes the probe fail the way an unreachable far end does. The drive must
    /// go on working, and the listing that triggered the probe must not notice.
    void setProbeFault(VfsError::Code error) { m_probeFault = error; }
    /// Makes the probe slow, so "a probe that hangs leaves the listing alone" is
    /// held by waiting for a condition rather than for a clock.
    void setProbeDelayMs(int ms) { m_probeDelayMs = ms; }

    /// How many times this drive was really asked. "At most once per drive per
    /// session" is a claim, and counting is what checks it.
    int probeCallCount() const;
    /// How many times stat() has been reached. For the claim that a gesture costs
    /// no request at all -- on a real remote drive a stat is a round trip, and a
    /// count is the only way to say "none" rather than "few".
    int statCallCount() const;
    /// Whether a probe is inside the drive right now. What a test waits for.
    bool isProbing() const;

protected:
    Result<QStringList> askWhatIsOffered(const VfsUri& target, const CancelToken& cancel) override;

private:
    struct Node
    {
        bool isDir = false;
        QByteArray contents;
        QDateTime modified;
        // After the three above, because the fixture helpers build a Node with
        // a braced list and these are set afterwards rather than at creation.
        bool isSymlink = false;
        bool isShortcut = false;
        /// What a link points at, stored as it was given. Only ever read for a
        /// node that is one.
        QString linkTarget;
    };

    static QString normalise(const QString& path);
    /// The stored spelling of whatever is at `path`, or `path` when nothing is.
    /// A no-op on a case-sensitive volume, which is the default. Callers must
    /// hold m_mutex.
    QString resolve(const QString& path) const;
    /// Moves a directory's own modification time, the way adding to or removing
    /// from a real one does. Callers must hold m_mutex.
    void touchParent(const QString& path);
    VfsUri uriFor(const QString& path) const;
    Result<void> faultFor(const QString& path) const;
    /// Sleeps setOperationDelayMs(), in steps.
    void waitAsASlowDriveWould() const;

    mutable QMutex m_mutex;
    Qt::CaseSensitivity m_caseSensitivity = Qt::CaseSensitive;
    NameRules m_nameRules;
    QHash<QString, Node> m_nodes;
    QHash<QString, VfsError::Code> m_faults;
    int m_listDelayMs = 0;
    /// The gate every listing waits at, and how many are waiting. Guarded by
    /// m_gateMutex rather than m_mutex: a held listing must not hold the drive's
    /// own lock, or the fixture would stop the rest of the drive answering and
    /// stop being a model of a blocking call.
    mutable QMutex m_gateMutex;
    std::shared_ptr<QSemaphore> m_listGate;
    std::atomic_int m_listsHeld { 0 };
    std::shared_ptr<QSemaphore> m_readGate;
    std::atomic_int m_readsHeld { 0 };
    std::atomic_int m_readCalls { 0 };
    int m_readDelayMs = 0;
    int m_operationDelayMs = 0;
    qint64 m_throttleBytes = 0;
    int m_throttleDelayMs = 0;
    mutable int m_listCalls = 0;
    mutable int m_statCalls = 0;
    QStringList m_offers;
    VfsError::Code m_probeFault = VfsError::None;
    int m_probeDelayMs = 0;
    mutable int m_probeCalls = 0;
    std::atomic_bool m_probing { false };
};

class MemoryFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("mem"); }
    QString displayName() const override { return QStringLiteral("In-memory scratch"); }
    QString iconName() const override { return QStringLiteral("drive-removable-media"); }

    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;
};

} // namespace mole
