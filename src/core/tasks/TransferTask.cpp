#include "core/tasks/TransferTask.h"

#include "core/diagnostics/Diagnostics.h"
#include "core/vfs/DirectoryWalker.h"

#include <QElapsedTimer>
#include <QLocale>

namespace mole {
namespace {

    constexpr qint64 kChunkSize = TransferTask::kCopyChunkBytes;

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
    // Both ends. A copy from a disk to a bucket is work on two drives, and the
    // sidebar has to say so about both of them.
    noteTouching(m_request.sources);
    noteTouching(m_request.targetDirectory);
}

void TransferTask::recordFailure(const VfsUri& uri, const VfsError& error)
{
    m_failures.append(QStringLiteral("%1: %2").arg(uri.fileName(), error.message));
}

bool TransferTask::isInsideOrEqual(const VfsUri& inner, const VfsUri& outer)
{
    // By uri rather than by backend pointer, deliberately. Two mounts of the
    // same drive are two objects -- a bookmarked folder and the disk it lives on
    // are both mounted -- and nothing above here knows they are the same place.
    // The uri is what does know.
    if (inner.scheme() != outer.scheme() || inner.authority() != outer.authority())
        return false;
    if (inner.path() == outer.path())
        return true;
    const QString prefix
        = outer.path().endsWith(QLatin1Char('/')) ? outer.path() : outer.path() + QLatin1Char('/');
    return inner.path().startsWith(prefix);
}

bool TransferTask::planJobs(QList<Job>& jobsOut)
{
    int sourceIndex = -1;
    for (const VfsUri& source : std::as_const(m_request.sources)) {
        ++sourceIndex;
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

        // A directory cannot be put inside itself. The copy is finite -- the
        // plan is built before a byte moves, so the walk never meets what the
        // copy is writing -- but a *move* then deletes the source, and the only
        // copy of everything that was in it is now underneath it. See ADR-0029.
        if (stat.value().isDir && isInsideOrEqual(m_request.targetDirectory, source)) {
            recordFailure(source,
                VfsError::make(VfsError::NotSupported,
                    QStringLiteral("%1 cannot be put inside itself: the destination is inside it")
                        .arg(source.path())));
            continue;
        }

        if (!stat.value().isDir) {
            jobsOut.append(Job { source, target, false, stat.value().size, sourceIndex });
            continue;
        }

        // The directory itself first, so its children have somewhere to land.
        jobsOut.append(Job { source, target, true, 0, sourceIndex });

        DirectoryWalker walker(m_request.sourceFileSystem);
        Result<void> walked = walker.walk(source, cancelToken(), [&](const FileEntry& entry, int) {
            // Rebuild each child's path relative to the source root.
            const QString relative = entry.uri.path().mid(source.path().size());
            jobsOut.append(
                Job { entry.uri, VfsUri(target.scheme(), target.authority(), target.path() + relative),
                    entry.isDir, entry.isDir ? 0 : entry.size, sourceIndex });
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

bool TransferTask::copyStream(const VfsUri& from, const VfsUri& to, qint64 expectedSize)
{
    QElapsedTimer clock;
    clock.start();

    // The size from the plan goes with the request: a remote backend cannot set
    // up a transfer sensibly without knowing whether it is fetching kilobytes or
    // gigabytes, and this is the one caller that always knows.
    Result<std::unique_ptr<QIODevice>> input = m_request.sourceFileSystem->openRead(from, expectedSize);
    if (!input.ok()) {
        recordFailure(from, input.error());
        return false;
    }

    Result<std::unique_ptr<QIODevice>> output = m_request.targetFileSystem->openWrite(to, expectedSize);
    if (!output.ok()) {
        recordFailure(to, output.error());
        return false;
    }

    QIODevice* source = input.value().get();
    QIODevice* target = output.value().get();

    // Read into a buffer rather than through the QByteArray overload, because
    // that one answers "the file ended" and "the read failed" with the same
    // empty result. A device that is being filled from a network as it is read
    // has no other way to say which happened, and taking the second for the
    // first is how half a file gets written and then reported as copied.
    QByteArray chunk(kChunkSize, Qt::Uninitialized);
    qint64 written = 0;
    for (;;) {
        if (isCancelRequested())
            return false;

        const qint64 got = source->read(chunk.data(), kChunkSize);
        if (got < 0) {
            recordFailure(from,
                VfsError::make(VfsError::IoError,
                    QStringLiteral("the source stopped after %1 bytes: %2")
                        .arg(written)
                        .arg(source->errorString())));
            return false;
        }
        if (got == 0)
            break;

        // The reason goes in the message. A destination that filled up, one
        // whose connection went away and one whose file was pulled out from
        // under it are all "short write", and which of them it was is the only
        // part anybody can act on.
        const qint64 put = target->write(chunk.constData(), got);
        if (put != got) {
            recordFailure(to,
                VfsError::make(VfsError::IoError,
                    QStringLiteral("the destination took %1 of %2 bytes and stopped: %3")
                        .arg(written + qMax<qint64>(put, 0))
                        .arg(written + got)
                        .arg(target->errorString())));
            return false;
        }

        // Reported per chunk, not per file: this is what makes a single large
        // copy show a moving bar and a throughput figure at all.
        written += got;
        setBytesDone(m_bytesCompleted + written);
    }

    // A read that ended early and a file that shrank look exactly alike from
    // here: both hand over fewer bytes than the plan said and then report the
    // end of the file. Only the source can tell them apart, so it is asked --
    // once, and only when there is a discrepancy to explain. A file that really
    // is smaller now is copied as it now is; a source that still claims the
    // larger size gave a short answer, and taking that for a finished copy is
    // how a move deletes the only whole one. See ADR-0027.
    //
    // Before the destination is closed, because closing is what puts it in
    // place: a copy that is about to be called a failure must not first be
    // renamed into the name somebody asked for.
    if (expectedSize > 0 && written < expectedSize) {
        const Result<FileEntry> now = m_request.sourceFileSystem->stat(from);
        if (!now.ok() || now.value().size != written) {
            recordFailure(from,
                VfsError::make(VfsError::IoError,
                    QStringLiteral("the source said %1 bytes and gave %2").arg(expectedSize).arg(written)));
            return false;
        }
        qCDebug(taskLog, "%s: shrank to %lld bytes while it was being copied", qPrintable(from.toString()),
            static_cast<long long>(written));
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

    qCDebug(taskLog, "%s -> %s: %lld of %lld bytes in %lld ms", qPrintable(from.toString()),
        qPrintable(to.toString()), static_cast<long long>(written), static_cast<long long>(expectedSize),
        clock.elapsed());

    // A file that grew between the listing and the copy is said rather than
    // enforced: everything it had when it was opened arrived, which is all a
    // copy can promise. What the destination now holds is checked separately,
    // once everything has been written -- see verifyArrivals().
    if (expectedSize > 0 && written > expectedSize) {
        qCWarning(taskLog, "%s: copied %lld bytes where the listing said %lld", qPrintable(from.toString()),
            static_cast<long long>(written), static_cast<long long>(expectedSize));
    }

    m_arrivals.append(Arrival { to, written });
    return true;
}

void TransferTask::verifyArrivals()
{
    if (m_arrivals.isEmpty())
        return;

    // By directory rather than by file: asking about one file at a time is a
    // round trip each, and on SFTP a stat *is* a listing of the parent -- ten
    // thousand files would mean ten thousand listings of the same directory.
    // One listing per directory answers for everything that landed in it.
    QHash<QString, QList<const Arrival*>> byDirectory;
    for (const Arrival& arrival : std::as_const(m_arrivals))
        byDirectory[arrival.target.parent().toString()].append(&arrival);

    int verified = 0;
    for (auto it = byDirectory.constBegin(); it != byDirectory.constEnd(); ++it) {
        if (isCancelRequested())
            return;

        const VfsUri directory = VfsUri::fromString(it.key());
        const Result<FileEntryList> listing = m_request.targetFileSystem->list(directory, cancelToken());
        if (!listing.ok()) {
            // Not being able to look is not the same as finding something wrong,
            // and calling a copy failed because the check could not run would be
            // its own kind of lie.
            qCWarning(taskLog, "could not check what arrived in %s: %s", qPrintable(it.key()),
                qPrintable(listing.error().message));
            continue;
        }

        QHash<QString, qint64> sizes;
        for (const FileEntry& entry : listing.value()) {
            if (!entry.isDir)
                sizes.insert(entry.name, entry.size);
        }

        for (const Arrival* arrival : it.value()) {
            const QString name = arrival->target.fileName();
            if (!sizes.contains(name)) {
                recordFailure(arrival->target,
                    VfsError::make(
                        VfsError::IoError, QStringLiteral("was copied but is not there afterwards")));
                continue;
            }
            const qint64 landed = sizes.value(name);
            if (landed != arrival->bytes) {
                recordFailure(arrival->target,
                    VfsError::make(VfsError::IoError,
                        QStringLiteral("%1 bytes were sent but %2 arrived").arg(arrival->bytes).arg(landed)));
                continue;
            }
            ++verified;
        }
    }

    qCDebug(taskLog, "checked %d of %lld copied files against the destination", verified,
        static_cast<long long>(m_arrivals.size()));
    reportCount(QStringLiteral("verified"), QStringLiteral("Verified"), verified, 45);
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

    return copyStream(job.source, job.target, job.size) ? Outcome::Transferred : Outcome::Failed;
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
        const Outcome outcome = transferOne(job);
        if (outcome == Outcome::Transferred && !job.isDirectory)
            ++m_copied;
        // Anything that did not arrive means this source has not been moved,
        // whatever the reason. A skipped file records no failure -- skipping is
        // a success -- and deleting the source of one is a file thrown away for
        // a file that was already there under the same name.
        if (outcome != Outcome::Transferred)
            m_unfinishedSources.insert(job.sourceIndex);

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

    // Every file that was copied is now weighed on the destination itself. Up to
    // here the only word for it is our own -- bytes were handed to a backend and
    // it did not complain -- and the one thing worth knowing about a copy is
    // whether what arrived is what was sent. It is deliberately the last thing
    // before a move deletes anything.
    setStatusText(QStringLiteral("checking what arrived…"));
    verifyArrivals();

    // A move only deletes the source once every byte arrived. Losing data
    // because a copy half-failed is not an acceptable trade for tidiness -- and
    // a source none of whose files arrived is not deleted either, however
    // deliberate the reason they did not.
    if (m_request.mode == Mode::Move && m_failures.isEmpty()) {
        for (int index = 0; index < m_request.sources.size(); ++index) {
            if (isCancelRequested())
                return;
            if (m_unfinishedSources.contains(index))
                continue;
            Result<void> removed = m_request.sourceFileSystem->remove(m_request.sources.at(index), true);
            if (!removed.ok())
                recordFailure(m_request.sources.at(index), removed.error());
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
    noteTouching(m_targets);
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

        // Nobody asks for this on purpose: it is what a selection of everything
        // collapses to, or what an empty path resolves to. The drive root is
        // somebody's whole disk, and there is nothing to undo it with.
        if (target.isRoot()) {
            m_failures.append(QStringLiteral("%1: the drive root cannot be deleted").arg(target.toString()));
            ++done;
            setProgress(static_cast<int>(100.0 * done / m_targets.size()));
            continue;
        }

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
