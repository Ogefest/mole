#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <QSet>
#include <QStringList>

#include <optional>

namespace mole {

/// Copies or moves entries between any two mounts.
///
/// It streams through IFileSystem, so local-to-local, local-to-archive-target
/// and NAS-to-S3 are all the same code path. A move within one backend is
/// short-circuited to a rename; anything else is copy-then-delete.
class TransferTask final : public Task
{
    Q_OBJECT

public:
    /// How much of a file the copy loop moves at a time.
    ///
    /// Named here rather than kept inside the loop because it is a boundary:
    /// a file one byte either side of it, or an exact multiple of it, takes a
    /// different path through the last read, and that is where a byte goes
    /// missing.
    static constexpr qint64 kCopyChunkBytes = 256 * 1024;

    enum class Mode { Copy, Move };

    /// What to do when the destination name is taken.
    enum class Conflict {
        Fail, ///< stop that entry and record a failure
        Skip, ///< leave the existing file alone
        Overwrite
    };

    struct Request
    {
        FileSystemPtr sourceFileSystem;
        FileSystemPtr targetFileSystem;
        QList<VfsUri> sources;
        VfsUri targetDirectory;
        Mode mode = Mode::Copy;
        Conflict onConflict = Conflict::Fail;
        /// Renames a single source on arrival. Empty keeps every original name;
        /// ignored when there is more than one source, since there would be
        /// nothing sensible to call the rest.
        QString targetName;
    };

    explicit TransferTask(Request request, QObject* parent = nullptr);

    int copiedCount() const { return m_copied; }
    int skippedCount() const { return m_skipped; }
    int failedCount() const { return m_failures.size(); }
    /// One human-readable line per entry that could not be transferred.
    const QStringList& failures() const { return m_failures; }

protected:
    void run() override;

private:
    struct Job
    {
        VfsUri source;
        VfsUri target;
        bool isDirectory = false;
        /// Known from the plan, so progress can be counted in bytes rather than
        /// in files -- one very large file otherwise sits at 0% then jumps.
        qint64 size = 0;
        /// Which of the requested sources this job came out of. A move deletes
        /// a source only when every job belonging to it arrived, so each job
        /// has to be able to say which one that is.
        int sourceIndex = 0;
    };

    /// Skipped is distinct from Transferred on purpose: a skipped conflict is
    /// a success, but it did not copy anything and must not be counted as if
    /// it had.
    ///
    /// Merged is distinct from both. A directory arriving where a directory of
    /// the same name stands is the expected thing rather than a clash, and it
    /// used to be reported as a skip -- which under ADR-0029's rule marked the
    /// whole source unfinished, so a move of a folder onto a folder of the same
    /// name copied the files and then deleted nothing. Nothing was copied for
    /// the directory itself either, so it is not Transferred.
    enum class Outcome { Transferred, Merged, Skipped, Failed };

    /// What the conflict policy says about one arrival.
    enum class Verdict {
        Proceed, ///< nothing in the way, or what is there is to be replaced
        Merge, ///< a directory that is already there; its children still arrive
        Skip, ///< leave what is there alone
        Failed, ///< recorded as a failure, and this entry does not happen
    };

    /// One file that was copied, and how many bytes went into it.
    struct Arrival
    {
        VfsUri target;
        qint64 bytes = 0;
    };

    /// True when `inner` is `outer` or sits underneath it, judged by uri rather
    /// than by which backend object was handed in.
    static bool isInsideOrEqual(
        const VfsUri& inner, const VfsUri& outer, Qt::CaseSensitivity sensitivity = Qt::CaseSensitive);

    /// Why `source` may not be put where this request wants it, or nothing.
    ///
    /// Held in one place because there are two paths to a move and both need it:
    /// the plan checks it per job, and the same-backend shortcut renames without
    /// building a plan at all. It was in planJobs() alone, which is how a move
    /// within one backend went straight past it -- reported as one item moved,
    /// no failures, and the tree relabelled underneath itself. See ADR-0029 and
    /// MOLE-275.
    std::optional<VfsError> refusalFor(const VfsUri& source, bool sourceIsDirectory) const;

    /// The name one source arrives under. Written once because three places ask
    /// it and the fourth would have been the copy that drifted.
    QString arrivalNameFor(const VfsUri& source) const;

    /// Whether the whole request can be done by renaming, and what each source
    /// is. Both answers come out of the same stat, and the shortcut needs both.
    ///
    /// A directory landing on a directory of the same name is a merge, which a
    /// rename cannot do: it would have to treat it as a clash instead, and
    /// Overwrite then deletes everything the destination folder holds that the
    /// source does not. The *whole* request goes to the generic path rather than
    /// that one source, because the plan is built from the request -- a source
    /// already renamed would be planned a second time and reported missing.
    bool everySourceCanBeRenamed(QList<bool>* sourceIsDirectory) const;

    /// Whether the plan would accept every source as it stands.
    ///
    /// The same-backend move shortcut does not build a plan, and the plan is
    /// where a transfer is refused -- so the shortcut is taken only when there is
    /// nothing to refuse, and anything else falls to the ordinary path, which
    /// reports it. One copy of each refusal rather than two that can drift, which
    /// is exactly how a directory came to be renamed inside itself with no
    /// failure and one item reported moved.
    bool nothingToRefuse() const;

    /// Weighs every copied file on the destination and fails the ones that do
    /// not match what was sent. One listing per directory, not one stat per
    /// file.
    void verifyArrivals();

    /// Expands directories into the full list of entries to create and copy.
    bool planJobs(QList<Job>& jobsOut);

    /// Deletes what really did arrive, for a source that only partly did.
    ///
    /// The whole source is removed in one call when every job under it was
    /// transferred, which is the ordinary case. This is the other one: a merge
    /// where a child was skipped, where removing the tree would take the skipped
    /// file with it and leaving the tree would mean nothing moved at all.
    void removeWhatArrivedUnder(int sourceIndex, const QList<Job>& jobs, const QList<Outcome>& outcomes);
    Outcome transferOne(const Job& job);
    /// `expectedSize` is what the plan was told the file is, and is used for the
    /// log rather than for a decision -- a listing can be out of date, and the
    /// backend is what proves a transfer was cut short.
    bool copyStream(const VfsUri& from, const VfsUri& to, qint64 expectedSize);

    /// Bytes already accounted for by completed jobs. The chunk loop adds its
    /// own progress on top, so a large file advances while it is copying.
    qint64 m_bytesCompleted = 0;
    /// Applies the conflict policy to one arrival.
    ///
    /// `replacing`, when asked for, says the policy was Overwrite and the file
    /// in the way was deliberately **left standing**: whatever puts the arrival
    /// in place is the thing that replaces it, so a transfer that fails part way
    /// leaves the file it was replacing alone. A caller that renames rather than
    /// writes has to say replace() instead of rename() when it is set.
    Verdict resolveConflict(const VfsUri& target, bool isDirectory, bool* replacing = nullptr);
    void recordFailure(const VfsUri& uri, const VfsError& error);

    Request m_request;
    QList<Arrival> m_arrivals;
    /// Indices into Request::sources whose jobs did not all arrive. A move
    /// deletes what is not in here and nothing else.
    QSet<int> m_unfinishedSources;
    int m_copied = 0;
    int m_skipped = 0;
    QStringList m_failures;
};

/// Removes entries, recursing into directories.
class DeleteTask final : public Task
{
    Q_OBJECT

public:
    DeleteTask(FileSystemPtr fileSystem, QList<VfsUri> targets, QObject* parent = nullptr);

    int deletedCount() const { return m_deleted; }
    const QStringList& failures() const { return m_failures; }
    /// Which targets actually went.
    ///
    /// The count says how many and this says which, because a caller showing a
    /// list of files has to remove the rows that are gone and keep the rows that
    /// are not. The duplicates tab used to clear all of them whatever happened,
    /// so a delete refused by a read-only drive left an empty tab and a rescan to
    /// do. See MOLE-341.
    const QList<VfsUri>& deletedUris() const { return m_deletedUris; }

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    QList<VfsUri> m_targets;
    int m_deleted = 0;
    QList<VfsUri> m_deletedUris;
    QStringList m_failures;
};

} // namespace mole
