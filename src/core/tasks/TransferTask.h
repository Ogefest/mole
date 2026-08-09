#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <QStringList>

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
    };

    /// Skipped is distinct from Transferred on purpose: a skipped conflict is
    /// a success, but it did not copy anything and must not be counted as if
    /// it had.
    enum class Outcome { Transferred, Skipped, Failed };

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
