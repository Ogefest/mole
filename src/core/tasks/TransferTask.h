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
    enum class Outcome { Transferred, Skipped, Failed };

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
    Outcome transferOne(const Job& job);
    /// `expectedSize` is what the plan was told the file is, and is used for the
    /// log rather than for a decision -- a listing can be out of date, and the
    /// backend is what proves a transfer was cut short.
    bool copyStream(const VfsUri& from, const VfsUri& to, qint64 expectedSize);

    /// Bytes already accounted for by completed jobs. The chunk loop adds its
    /// own progress on top, so a large file advances while it is copying.
    qint64 m_bytesCompleted = 0;
    /// Returns false when the entry should be skipped entirely.
    bool resolveConflict(const VfsUri& target, bool isDirectory, bool* skip);
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

protected:
    void run() override;

private:
    FileSystemPtr m_fileSystem;
    QList<VfsUri> m_targets;
    int m_deleted = 0;
    QStringList m_failures;
};

} // namespace mole
