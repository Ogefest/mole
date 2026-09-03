#include "core/tasks/TransferTask.h"

#include "core/diagnostics/Diagnostics.h"
#include "core/vfs/DirectoryWalker.h"

#include <QElapsedTimer>
#include <QLocale>

#include <algorithm>

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

bool TransferTask::isInsideOrEqual(const VfsUri& inner, const VfsUri& outer, Qt::CaseSensitivity sensitivity)
{
    // By uri rather than by backend pointer, deliberately. Two mounts of the
    // same drive are two objects -- a bookmarked folder and the disk it lives on
    // are both mounted -- and nothing above here knows they are the same place.
    // The uri is what does know.
    //
    // The sensitivity comes from the destination backend, not from the uri's
    // scheme, because the volume is the only thing that really knows. On an NTFS
    // or a default APFS volume, comparing the two spellings exactly let a
    // different capitalisation walk straight through the guard below.
    return inner.isWithin(outer, sensitivity);
}

std::optional<VfsError> TransferTask::refusalFor(const VfsUri& source, bool sourceIsDirectory) const
{
    // A directory cannot be put inside itself. The copy is finite -- the plan is
    // built before a byte moves, so the walk never meets what the copy is
    // writing -- but a *move* then deletes the source, and the only copy of
    // everything that was in it is now underneath it. See ADR-0029.
    if (sourceIsDirectory
        && isInsideOrEqual(
            m_request.targetDirectory, source, m_request.targetFileSystem->pathCaseSensitivity())) {
        return VfsError::make(VfsError::NotSupported,
            QStringLiteral("%1 cannot be put inside itself: the destination is inside it")
                .arg(source.path()));
    }
    return std::nullopt;
}

QString TransferTask::arrivalNameFor(const VfsUri& source) const
{
    return (m_request.sources.size() == 1 && !m_request.targetName.isEmpty()) ? m_request.targetName
                                                                              : source.fileName();
}

bool TransferTask::everySourceCanBeRenamed(QList<bool>* sourceIsDirectory) const
{
    sourceIsDirectory->clear();
    sourceIsDirectory->reserve(m_request.sources.size());

    bool renameable = true;
    for (const VfsUri& source : m_request.sources) {
        const Result<FileEntry> stat = m_request.sourceFileSystem->stat(source);
        const bool isDirectory = stat.ok() && stat.value().isDir;
        sourceIsDirectory->append(isDirectory);
        if (!isDirectory)
            continue;

        // One stat of the destination, and only for a directory: a bulk move of
        // files pays a stat of the source apiece and nothing more.
        const Result<FileEntry> standing
            = m_request.targetFileSystem->stat(m_request.targetDirectory.child(arrivalNameFor(source)));
        if (standing.ok() && standing.value().isDir)
            renameable = false;
    }
    return renameable;
}

bool TransferTask::nothingToRefuse() const
{
    for (const VfsUri& source : m_request.sources) {
        // The uri predicate first, because it is free and almost always false --
        // it is what decides whether a stat is worth a round trip at all. Without
        // this the fast path would pay a stat per source to answer a question
        // about paths.
        if (isInsideOrEqual(
                m_request.targetDirectory, source, m_request.targetFileSystem->pathCaseSensitivity())) {
            const Result<FileEntry> stat = m_request.sourceFileSystem->stat(source);
            if (!stat.ok() || refusalFor(source, stat.value().isDir).has_value())
                return false;
        }

        // And the name the destination would have to accept, which the shortcut
        // never asked about either. See MOLE-243.
        if (checkName(arrivalNameFor(source), m_request.targetFileSystem->nameRules()).isRejected())
            return false;
    }
    return true;
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

        // A symbolic link is copied as a link, whatever it points at and whether
        // or not that is there -- see ADR-0092. Asked before the question below,
        // because a link to nothing is *both* a link and a special, and it is
        // the link that decides what to do with it.
        if (stat.value().isSymlink) {
            const QString arrival = arrivalNameFor(source);
            if (const NameVerdict verdict = checkName(arrival, m_request.targetFileSystem->nameRules());
                verdict.isRejected()) {
                recordFailure(source, VfsError::make(VfsError::IoError, verdict.reason));
                continue;
            }
            jobsOut.append(
                Job { source, m_request.targetDirectory.child(arrival), Kind::Link, 0, sourceIndex });
            continue;
        }

        // Neither a file nor a folder: a named pipe, a socket, a device node, a
        // link to something that is not there. There is nothing to stream and
        // nothing sensible to make at the far end, so it is refused by name --
        // and a move, which deletes nothing while anything failed, then leaves
        // it where it is. It used not to be listed at all, so it was passed over
        // in silence and then removed with the rest of the tree. See MOLE-333.
        //
        // The same-backend shortcut is deliberately not held to this: a rename
        // really does move a pipe or a link, and refusing what the drive can do
        // would be a worse answer than the one the two paths give apart.
        if (stat.value().special != SpecialKind::None) {
            recordFailure(source,
                VfsError::make(VfsError::NotSupported,
                    QStringLiteral("%1, so it was not copied").arg(describe(stat.value().special))));
            continue;
        }

        const QString arrivalName = arrivalNameFor(source);

        // Asked before a byte moves, and asked of the destination, because the
        // destination is what knows. This is the code that names the output of
        // extracting an archive and of downloading from a remote drive -- where
        // "really?.txt" and "a:b.txt" are perfectly legal names -- so on Windows
        // a folder copied off a NAS used to fail partway through with an IoError
        // carrying a path and no reason, having already written everything
        // before the offending file.
        //
        // One file's failure, not the run's, and not a silently sanitised name:
        // a file that arrives under a name nobody chose is harder to find later
        // than one that did not arrive.
        if (const NameVerdict verdict = checkName(arrivalName, m_request.targetFileSystem->nameRules());
            verdict.isRejected()) {
            recordFailure(source,
                VfsError::make(VfsError::IoError,
                    verdict.suggestion.isEmpty() ? verdict.reason
                                                 : QStringLiteral("%1 -- \"%2\" would be accepted")
                                                       .arg(verdict.reason, verdict.suggestion)));
            continue;
        }

        const VfsUri target = m_request.targetDirectory.child(arrivalName);

        if (const std::optional<VfsError> refusal = refusalFor(source, stat.value().isDir)) {
            recordFailure(source, *refusal);
            continue;
        }

        if (!stat.value().isDir) {
            jobsOut.append(Job { source, target, Kind::File, stat.value().size, sourceIndex });
            continue;
        }

        // The directory itself first, so its children have somewhere to land.
        jobsOut.append(Job { source, target, Kind::Directory, 0, sourceIndex });

        const NameRules arrivalRules = m_request.targetFileSystem->nameRules();

        DirectoryWalker walker(m_request.sourceFileSystem);
        Result<void> walked = walker.walk(source, cancelToken(), [&](const FileEntry& entry, int) {
            // The same rule as above, one level in, and in the same order: the
            // walker does not descend into a link, so this is where a linked
            // folder inside a copied tree becomes one job of its own.
            if (entry.isSymlink) {
                const QString relative = entry.uri.path().mid(source.path().size());
                jobsOut.append(
                    Job { entry.uri, VfsUri(target.scheme(), target.authority(), target.path() + relative),
                        Kind::Link, 0, sourceIndex });
                return DirectoryWalker::Action::Continue;
            }

            // The same refusal as above, one level in. This is where it is met in
            // practice: nobody selects a socket and presses copy, and every tree
            // under /run and /tmp has one in it.
            if (entry.special != SpecialKind::None) {
                recordFailure(entry.uri,
                    VfsError::make(VfsError::NotSupported,
                        QStringLiteral("%1, so it was not copied").arg(describe(entry.special))));
                return DirectoryWalker::Action::Continue;
            }

            // Every child gets the same question as the top-level name, and for
            // the same reason: the offending file is somewhere inside a tree
            // copied off a drive with looser rules, and it is one file's failure
            // rather than the run's.
            if (const NameVerdict verdict = checkName(entry.name, arrivalRules); verdict.isRejected()) {
                recordFailure(entry.uri,
                    VfsError::make(VfsError::IoError,
                        verdict.suggestion.isEmpty() ? verdict.reason
                                                     : QStringLiteral("%1 -- \"%2\" would be accepted")
                                                           .arg(verdict.reason, verdict.suggestion)));
                // A directory nothing can be written into is a subtree that
                // cannot arrive, and saying so once beats saying it per file.
                return entry.isDir ? DirectoryWalker::Action::SkipSubtree : DirectoryWalker::Action::Continue;
            }

            // Rebuild each child's path relative to the source root.
            const QString relative = entry.uri.path().mid(source.path().size());
            jobsOut.append(
                Job { entry.uri, VfsUri(target.scheme(), target.authority(), target.path() + relative),
                    entry.isDir ? Kind::Directory : Kind::File, entry.isDir ? 0 : entry.size, sourceIndex });
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

TransferTask::Verdict TransferTask::resolveConflict(const VfsUri& target, bool isDirectory, bool* replacing)
{
    if (replacing)
        *replacing = false;

    Result<FileEntry> existing = m_request.targetFileSystem->stat(target);
    if (!existing.ok())
        return Verdict::Proceed; // nothing there, carry on

    // Merging into an existing directory is the expected behaviour, not a clash.
    // Its own answer rather than a skip: nothing is copied for the directory
    // itself, but everything inside it still arrives, and a source whose only
    // "skip" was this was never deleted -- so a move of a folder onto a folder
    // of the same name behaved as a copy. See MOLE-332.
    if (isDirectory && existing.value().isDir)
        return Verdict::Merge;

    switch (m_request.onConflict) {
    case Conflict::Skip:
        ++m_skipped;
        return Verdict::Skip;
    case Conflict::Overwrite: {
        // A file going over a file is left standing until the replacement is
        // whole. This used to remove it here, before the write was even opened,
        // so a copy that then failed half way -- a dropped connection, a full
        // disk, a cancel -- left neither the old file nor the new one. The
        // protection against exactly that has been in the backends since
        // ADR-0021: a write goes under a working name and is put in place at the
        // very end. Handing it a destination that had already been deleted was
        // what stopped it engaging. See ADR-0087 and MOLE-331.
        if (!isDirectory && !existing.value().isDir) {
            if (replacing)
                *replacing = true;
            return Verdict::Proceed;
        }

        // Different kinds, so there is nothing the arrival can be put over: a
        // directory standing where a file is going, or a file where a directory
        // is. That one has to go first, and there is no moment at which both
        // could exist anyway.
        Result<void> removed = m_request.targetFileSystem->remove(target, true);
        if (!removed.ok()) {
            recordFailure(target, removed.error());
            return Verdict::Failed;
        }
        return Verdict::Proceed;
    }
    case Conflict::Fail:
        recordFailure(target, VfsError::make(VfsError::AlreadyExists, QStringLiteral("already exists")));
        return Verdict::Failed;
    }
    return Verdict::Failed;
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
    // A link is a name, so a clash over one follows the rules for a file rather
    // than the ones for a directory -- there is nothing to merge into.
    bool replacing = false;
    switch (resolveConflict(job.target, job.kind == Kind::Directory, &replacing)) {
    case Verdict::Failed:
        return Outcome::Failed;
    case Verdict::Skip:
        return Outcome::Skipped;
    case Verdict::Merge:
        return Outcome::Merged;
    case Verdict::Proceed:
        break;
    }

    if (job.kind == Kind::Link)
        return linkOne(job, replacing) ? Outcome::Transferred : Outcome::Failed;

    if (job.kind == Kind::Directory) {
        Result<void> created = m_request.targetFileSystem->makeDirectory(job.target);
        if (!created.ok() && created.error().code != VfsError::AlreadyExists) {
            recordFailure(job.target, created.error());
            return Outcome::Failed;
        }
        return Outcome::Transferred;
    }

    return copyStream(job.source, job.target, job.size) ? Outcome::Transferred : Outcome::Failed;
}

bool TransferTask::linkOne(const Job& job, bool replacing)
{
    // Asked of the destination, because the destination is what knows -- the same
    // sentence the name rules follow. Most drives have no links at all, and the
    // refusal has to name the file and say why: a link that quietly arrived as
    // something else is the fault this whole rule exists for. See ADR-0092.
    if (!m_request.targetFileSystem->capabilities().testFlag(VfsCapability::Symlink)) {
        recordFailure(job.source,
            VfsError::make(
                VfsError::NotSupported, QStringLiteral("a symbolic link, and this drive cannot hold one")));
        return false;
    }

    const Result<QString> points = m_request.sourceFileSystem->readLink(job.source);
    if (!points.ok()) {
        recordFailure(job.source, points.error());
        return false;
    }

    // Nothing is staged, because there is nothing to stream: a link is made in
    // one call or not at all. So what is standing there is removed here rather
    // than being left until the arrival is whole -- the moment ADR-0021 protects
    // for a file does not exist for a link.
    if (replacing) {
        if (const Result<void> removed = m_request.targetFileSystem->remove(job.target, false);
            !removed.ok()) {
            recordFailure(job.target, removed.error());
            return false;
        }
    }

    const Result<void> made = m_request.targetFileSystem->makeLink(job.target, points.value());
    if (!made.ok()) {
        recordFailure(job.target, made.error());
        return false;
    }
    return true;
}

void TransferTask::removeWhatArrivedUnder(
    int sourceIndex, const QList<Job>& jobs, const QList<Outcome>& outcomes)
{
    // Only a skip can bring us here: any failure at all stops the whole delete
    // step before this is reached, and a cancel returns earlier still. So what
    // is left under this source is a file somebody asked to leave alone, and
    // everything else did arrive.
    //
    // Deepest first, so a directory is only tried once whatever was under it has
    // gone -- and non-recursively, so one that still holds a skipped file keeps
    // it. That is the same sentence ADR-0029 already applies to a selection of
    // files, applied to the selection a merge really is.
    QList<int> arrived;
    for (int at = 0; at < jobs.size(); ++at) {
        if (jobs.at(at).sourceIndex != sourceIndex)
            continue;
        if (outcomes.at(at) == Outcome::Transferred || outcomes.at(at) == Outcome::Merged)
            arrived.append(at);
    }
    std::sort(arrived.begin(), arrived.end(), [&jobs](int first, int second) {
        return jobs.at(first).source.path().size() > jobs.at(second).source.path().size();
    });

    for (const int at : std::as_const(arrived)) {
        if (isCancelRequested())
            return;
        const Job& job = jobs.at(at);
        const Result<void> removed = m_request.sourceFileSystem->remove(job.source, false);
        // A directory that is not empty is one holding what was skipped, which
        // is the outcome asked for rather than a failure to report.
        if (!removed.ok() && !(job.kind == Kind::Directory && removed.error().code == VfsError::NotEmpty))
            recordFailure(job.source, removed.error());
    }
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
    //
    // Only when there is nothing to refuse, though: this path does not build a
    // plan and the plan is where a transfer is refused, so a directory being put
    // inside itself used to be renamed underneath itself, reported as one item
    // moved with no failures. Local disk was saved only by the kernel, which
    // answers EINVAL; MemoryFileSystem, which is a scratch drive somebody can
    // mount, relabelled the whole subtree. See MOLE-275 and ADR-0029.
    //
    // And only when a rename can express what was asked. A folder landing on a
    // folder of the same name is a merge, which a rename cannot do: the
    // shortcut used to pass isDirectory=false for every source, so the merge
    // branch was unreachable there and Overwrite recursively deleted the whole
    // destination folder before renaming the source onto its name. Which of the
    // two happened depended on whether both ends shared a FileSystemPtr, which
    // the user cannot see. See MOLE-332.
    const bool sameBackend = m_request.sourceFileSystem == m_request.targetFileSystem;
    QList<bool> sourceIsDirectory;
    if (m_request.mode == Mode::Move && sameBackend && nothingToRefuse()
        && everySourceCanBeRenamed(&sourceIsDirectory)) {
        int index = 0;
        for (const VfsUri& source : std::as_const(m_request.sources)) {
            if (isCancelRequested())
                return;

            const VfsUri target = m_request.targetDirectory.child(arrivalNameFor(source));
            bool replacing = false;
            const Verdict verdict = resolveConflict(target, sourceIsDirectory.at(index), &replacing);
            if (verdict != Verdict::Proceed) {
                ++index;
                continue;
            }

            // Nothing was cleared out of the way, so the rename is the one that
            // is allowed to replace what is standing there. rename() refuses an
            // occupied name, and has to.
            Result<void> renamed = replacing ? m_request.sourceFileSystem->replace(source, target)
                                             : m_request.sourceFileSystem->rename(source, target);
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
    // Kept beside the jobs rather than inside them, because the delete step is
    // the only thing that reads it and it is one byte per job.
    QList<Outcome> outcomes;
    outcomes.reserve(jobs.size());
    for (const Job& job : std::as_const(jobs)) {
        if (isCancelRequested())
            return;

        // Directories are structure, not payload: only files count as copied.
        const Outcome outcome = transferOne(job);
        outcomes.append(outcome);
        if (outcome == Outcome::Transferred && job.kind != Kind::Directory)
            ++m_copied;
        // Anything that did not arrive means this source has not been moved,
        // whatever the reason. A skipped file records no failure -- skipping is
        // a success -- and deleting the source of one is a file thrown away for
        // a file that was already there under the same name.
        //
        // A merge is not one of those. The directory was already there, which is
        // what was expected, and everything inside it still had its own answer.
        if (outcome != Outcome::Transferred && outcome != Outcome::Merged)
            m_unfinishedSources.insert(job.sourceIndex);

        // Whatever the outcome, this job is no longer in flight, so its bytes
        // are settled -- a skipped file must not leave the total short for ever.
        m_bytesCompleted += job.size;
        setBytesDone(m_bytesCompleted);

        ++done;
        if (job.kind != Kind::Directory)
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
            if (!m_unfinishedSources.contains(index)) {
                Result<void> removed = m_request.sourceFileSystem->remove(m_request.sources.at(index), true);
                if (!removed.ok())
                    recordFailure(m_request.sources.at(index), removed.error());
                continue;
            }
            removeWhatArrivedUnder(index, jobs, outcomes);
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
        if (removed.ok()) {
            ++m_deleted;
            m_deletedUris.append(target);
        } else {
            m_failures.append(QStringLiteral("%1: %2").arg(target.fileName(), removed.error().message));
        }

        ++done;
        setProgress(static_cast<int>(100.0 * done / m_targets.size()));
    }

    setProgress(100);
    setStatusText(m_failures.isEmpty()
            ? QStringLiteral("%1 item(s) deleted").arg(m_deleted)
            : QStringLiteral("%1 deleted, %2 failed").arg(m_deleted).arg(m_failures.size()));
}

} // namespace mole
