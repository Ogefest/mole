#include "core/tasks/TransferTask.h"

#include "core/vfs/DirectoryWalker.h"

#include <QLocale>

namespace mole {
namespace {

    constexpr qint64 kChunkSize = 256 * 1024;

    QString describe(TransferTask::Mode mode, int count)
    {
        return mode == TransferTask::Mode::Copy ? QStringLiteral("Copy %1 item(s)").arg(count)
                                                : QStringLiteral("Move %1 item(s)").arg(count);
    }

} // namespace

TransferTask::TransferTask(Request request, QObject* parent)
    : Task(describe(request.mode, static_cast<int>(request.sources.size())), parent)
    , m_request(std::move(request))
{
}

void TransferTask::recordFailure(const VfsUri& uri, const VfsError& error)
{
    m_failures.append(QStringLiteral("%1: %2").arg(uri.fileName(), error.message));
}

bool TransferTask::planJobs(QList<Job>& jobsOut)
{
    for (const VfsUri& source : std::as_const(m_request.sources)) {
        if (isCancelRequested())
            return false;

        Result<FileEntry> stat = m_request.sourceFileSystem->stat(source);
        if (!stat.ok()) {
            recordFailure(source, stat.error());
            continue;
        }

        const QString arrivalName = (m_request.sources.size() == 1 && !m_request.targetName.isEmpty())
            ? m_request.targetName
            : source.fileName();
        const VfsUri target = m_request.targetDirectory.child(arrivalName);

        if (!stat.value().isDir) {
            jobsOut.append(Job { source, target, false, stat.value().size });
            continue;
        }

        // The directory itself first, so its children have somewhere to land.
        jobsOut.append(Job { source, target, true, 0 });

        DirectoryWalker walker(m_request.sourceFileSystem);
        Result<void> walked = walker.walk(source, cancelToken(), [&](const FileEntry& entry, int) {
            // Rebuild each child's path relative to the source root.
            const QString relative = entry.uri.path().mid(source.path().size());
            jobsOut.append(
                Job { entry.uri, VfsUri(target.scheme(), target.authority(), target.path() + relative),
                    entry.isDir, entry.isDir ? 0 : entry.size });
            return DirectoryWalker::Action::Continue;
        });

        if (!walked.ok()) {
            if (walked.error().code == VfsError::Cancelled)
                return false;
            recordFailure(source, walked.error());
        }
        for (const VfsError& error : walker.errors())
            m_failures.append(error.message);
    }
    return true;
}

bool TransferTask::resolveConflict(const VfsUri& target, bool isDirectory, bool* skip)
{
    *skip = false;

    Result<FileEntry> existing = m_request.targetFileSystem->stat(target);
    if (!existing.ok())
        return true; // nothing there, carry on

    // Merging into an existing directory is the expected behaviour, not a clash.
    if (isDirectory && existing.value().isDir) {
        *skip = true;
        return true;
    }

    switch (m_request.onConflict) {
    case Conflict::Skip:
        *skip = true;
        ++m_skipped;
        return true;
    case Conflict::Overwrite: {
        Result<void> removed = m_request.targetFileSystem->remove(target, true);
        if (!removed.ok()) {
            recordFailure(target, removed.error());
            return false;
        }
        return true;
    }
    case Conflict::Fail:
        recordFailure(target, VfsError::make(VfsError::AlreadyExists, QStringLiteral("already exists")));
        return false;
    }
    return false;
}

bool TransferTask::copyStream(const VfsUri& from, const VfsUri& to)
{
    Result<std::unique_ptr<QIODevice>> input = m_request.sourceFileSystem->openRead(from);
    if (!input.ok()) {
        recordFailure(from, input.error());
        return false;
    }

    Result<std::unique_ptr<QIODevice>> output = m_request.targetFileSystem->openWrite(to);
    if (!output.ok()) {
        recordFailure(to, output.error());
        return false;
    }

    QIODevice* source = input.value().get();
    QIODevice* target = output.value().get();

    qint64 written = 0;
    while (!source->atEnd()) {
        if (isCancelRequested())
            return false;

        const QByteArray chunk = source->read(kChunkSize);
        if (chunk.isEmpty())
            break;
        if (target->write(chunk) != chunk.size()) {
            recordFailure(to, VfsError::make(VfsError::IoError, QStringLiteral("short write")));
            return false;
        }

        // Reported per chunk, not per file: this is what makes a single large
        // copy show a moving bar and a throughput figure at all.
        written += chunk.size();
        setBytesDone(m_bytesCompleted + written);
    }

    // Closing is where a buffered backend actually commits, so it must happen
    // before anyone stats the result -- and it is where a remote write reports
    // that it failed, which is why the outcome is collected rather than assumed.
    const Result<void> committed = closeAndReport(*target);
    source->close();
    if (!committed.ok()) {
        recordFailure(to, committed.error());
        return false;
    }
    return true;
}

TransferTask::Outcome TransferTask::transferOne(const Job& job)
{
    bool skip = false;
    if (!resolveConflict(job.target, job.isDirectory, &skip))
        return Outcome::Failed;
    if (skip)
        return Outcome::Skipped;

    if (job.isDirectory) {
        Result<void> created = m_request.targetFileSystem->makeDirectory(job.target);
        if (!created.ok() && created.error().code != VfsError::AlreadyExists) {
            recordFailure(job.target, created.error());
            return Outcome::Failed;
        }
        return Outcome::Transferred;
    }

    return copyStream(job.source, job.target) ? Outcome::Transferred : Outcome::Failed;
}

void TransferTask::run()
{
    if (!m_request.sourceFileSystem || !m_request.targetFileSystem) {
        fail(VfsError::make(VfsError::NotSupported, QStringLiteral("Transfer is missing a backend")));
        return;
    }
    if (m_request.sources.isEmpty()) {
        setProgress(100);
        setStatusText(QStringLiteral("Nothing to transfer"));
        return;
    }

    // Moving inside one backend is a rename, which is instant and atomic --
    // never stream bytes when the filesystem can just relabel them.
    const bool sameBackend = m_request.sourceFileSystem == m_request.targetFileSystem;
    if (m_request.mode == Mode::Move && sameBackend) {
        int index = 0;
        for (const VfsUri& source : std::as_const(m_request.sources)) {
            if (isCancelRequested())
                return;

            const QString arrivalName = (m_request.sources.size() == 1 && !m_request.targetName.isEmpty())
                ? m_request.targetName
                : source.fileName();
            const VfsUri target = m_request.targetDirectory.child(arrivalName);
            bool skip = false;
            if (!resolveConflict(target, false, &skip) || skip) {
                ++index;
                continue;
            }

            Result<void> renamed = m_request.sourceFileSystem->rename(source, target);
            if (renamed.ok())
                ++m_copied;
            else if (renamed.error().code == VfsError::NotSupported)
                break; // fall through to the generic path below
            else
                recordFailure(source, renamed.error());

            setProgress(static_cast<int>(100.0 * ++index / m_request.sources.size()));
        }

        if (m_copied + m_skipped + m_failures.size() >= m_request.sources.size()) {
            setStatusText(QStringLiteral("Moved %1 item(s)").arg(m_copied));
            setProgress(100);
            return;
        }
    }

    QList<Job> jobs;
    if (!planJobs(jobs))
        return; // cancelled

    qint64 payload = 0;
    for (const Job& job : std::as_const(jobs))
        payload += job.size;
    setByteTotal(payload);

    setStatusText(
        QStringLiteral("%1 item(s), %2").arg(jobs.size()).arg(QLocale().formattedDataSize(payload)));

    int done = 0;
    int files = 0;
    for (const Job& job : std::as_const(jobs)) {
        if (isCancelRequested())
            return;

        // Directories are structure, not payload: only files count as copied.
        if (transferOne(job) == Outcome::Transferred && !job.isDirectory)
            ++m_copied;

        // Whatever the outcome, this job is no longer in flight, so its bytes
        // are settled -- a skipped file must not leave the total short for ever.
        m_bytesCompleted += job.size;
        setBytesDone(m_bytesCompleted);

        ++done;
        if (!job.isDirectory)
            ++files;
        setStatusText(QStringLiteral("%1 / %2 files").arg(files).arg(jobs.size()));

        // Published through the general mechanism rather than baked into the
        // status line, so the strip can lay them out and a later task can
        // publish its own without the interface changing.
        reportCount(TaskMetrics::kFiles, QStringLiteral("Files"), files, 40);
        if (m_skipped > 0)
            reportCount(QStringLiteral("skipped"), QStringLiteral("Skipped"), m_skipped, 50);
        if (!m_failures.isEmpty()) {
            reportCount(QStringLiteral("failed"), QStringLiteral("Failed"),
                static_cast<double>(m_failures.size()), 60);
        }
    }

    // A move only deletes the source once every byte arrived. Losing data
    // because a copy half-failed is not an acceptable trade for tidiness.
    if (m_request.mode == Mode::Move && m_failures.isEmpty()) {
        for (const VfsUri& source : std::as_const(m_request.sources)) {
            if (isCancelRequested())
                return;
            Result<void> removed = m_request.sourceFileSystem->remove(source, true);
            if (!removed.ok())
                recordFailure(source, removed.error());
        }
    }

    setProgress(100);
    if (m_failures.isEmpty()) {
        setStatusText(QStringLiteral("%1 file(s) transferred%2")
                          .arg(m_copied)
                          .arg(m_skipped > 0 ? QStringLiteral(", %1 skipped").arg(m_skipped) : QString()));
    } else {
        setStatusText(QStringLiteral("%1 transferred, %2 failed").arg(m_copied).arg(m_failures.size()));
    }
}

DeleteTask::DeleteTask(FileSystemPtr fileSystem, QList<VfsUri> targets, QObject* parent)
    : Task(QStringLiteral("Delete %1 item(s)").arg(targets.size()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_targets(std::move(targets))
{
}

void DeleteTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(VfsError::NotSupported, QStringLiteral("Delete is missing a backend")));
        return;
    }

    int done = 0;
    for (const VfsUri& target : std::as_const(m_targets)) {
        if (isCancelRequested())
            return;

        Result<void> removed = m_fileSystem->remove(target, true);
        if (removed.ok())
            ++m_deleted;
        else
            m_failures.append(QStringLiteral("%1: %2").arg(target.fileName(), removed.error().message));

        ++done;
        setProgress(static_cast<int>(100.0 * done / m_targets.size()));
    }

    setProgress(100);
    setStatusText(m_failures.isEmpty()
            ? QStringLiteral("%1 item(s) deleted").arg(m_deleted)
            : QStringLiteral("%1 deleted, %2 failed").arg(m_deleted).arg(m_failures.size()));
}

} // namespace mole
