#include "core/sync/SyncTask.h"

#include <QHash>
#include <QLocale>

namespace mole {
namespace {

    constexpr qint64 kChunkSize = 512 * 1024;

} // namespace

SyncTask::SyncTask(FileSystemPtr sourceFs, VfsUri source, FileSystemPtr targetFs, VfsUri target,
    SyncOptions options, QObject* parent)
    : Task(options.dryRun ? QStringLiteral("Compare %1").arg(source.fileName())
                          : QStringLiteral("Sync %1").arg(source.fileName()),
          parent)
    , m_sourceFs(std::move(sourceFs))
    , m_source(std::move(source))
    , m_targetFs(std::move(targetFs))
    , m_target(std::move(target))
    , m_options(std::move(options))
{
    // Both trees, like a copy: a sync between two drives is work on two drives.
    noteTouching(m_source);
    noteTouching(m_target);
}

SyncTask::SyncTask(FileSystemPtr sourceFs, VfsUri source, FileSystemPtr targetFs, VfsUri target,
    SyncOptions options, SyncPlan plan, QObject* parent)
    : SyncTask(std::move(sourceFs), std::move(source), std::move(targetFs), std::move(target),
          std::move(options), parent)
{
    m_plan = std::move(plan);
    m_planWasGiven = true;
}

bool SyncTask::stillWhatThePlanSaw(const SyncPlan::Step& step, QString* changed) const
{
    const Result<FileEntry> now = m_targetFs->stat(step.target);
    if (!now.ok()) {
        // Already gone is not a change to refuse over: the outcome the step
        // wanted is the outcome there is. Anything else -- a drive that has gone
        // away, a folder nobody may read -- is not an answer about the file, and
        // removing on the strength of it would be acting on a guess.
        if (now.error().code == VfsError::NotFound)
            return true;
        *changed = now.error().message;
        return false;
    }

    if (now.value().size != step.bytes) {
        *changed = QStringLiteral("it is %1 bytes now and was %2 when the plan was made")
                       .arg(now.value().size)
                       .arg(step.bytes);
        return false;
    }

    // Only when both sides have one. A drive that reports no time says nothing
    // either way, and treating "no answer" as "it changed" would stop every
    // mirror to such a drive from ever deleting anything.
    if (step.targetModified.isValid() && now.value().modified.isValid()
        && now.value().modified != step.targetModified) {
        *changed = QStringLiteral("it was written at %1, after the plan was made")
                       .arg(now.value().modified.toString(Qt::ISODate));
        return false;
    }
    return true;
}

bool SyncTask::linkOne(const SyncPlan::Step& step)
{
    // Asked of the destination, because the destination is what knows. Most
    // drives have no links at all, and a link that quietly arrived as something
    // else is the fault ADR-0092 exists for -- so this says which file and why.
    if (!m_targetFs->capabilities().testFlag(VfsCapability::Symlink)) {
        m_failures.append(
            QStringLiteral("%1: a symbolic link, and this drive cannot hold one").arg(step.relativePath));
        return false;
    }

    const Result<QString> points = m_sourceFs->readLink(step.source);
    if (!points.ok()) {
        m_failures.append(QStringLiteral("%1: %2").arg(step.relativePath, points.error().message));
        return false;
    }
    if (const Result<void> made = m_targetFs->makeLink(step.target, points.value()); !made.ok()) {
        m_failures.append(QStringLiteral("%1: %2").arg(step.relativePath, made.error().message));
        return false;
    }
    return true;
}

bool SyncTask::copyOne(const SyncPlan::Step& step)
{
    // The plan measured the file already; passing that on is what lets a remote
    // backend fetch a large one differently from a small one.
    Result<std::unique_ptr<QIODevice>> input = m_sourceFs->openRead(step.source, step.bytes);
    if (!input.ok()) {
        m_failures.append(QStringLiteral("%1: %2").arg(step.relativePath, input.error().message));
        return false;
    }
    Result<std::unique_ptr<QIODevice>> output = m_targetFs->openWrite(step.target, step.bytes);
    if (!output.ok()) {
        m_failures.append(QStringLiteral("%1: %2").arg(step.relativePath, output.error().message));
        return false;
    }

    QIODevice* from = input.value().get();
    QIODevice* to = output.value().get();

    // Read into a buffer rather than through the QByteArray overload, because
    // that one answers "the file ended" and "the read failed" with the same
    // empty result. A device being filled from a network as it is read has no
    // other way to say which happened -- and taking the second for the first is
    // worse here than anywhere else: sync would commit the destination, count
    // the file as copied, and the next run would see matching sizes on both
    // sides and copy nothing. The loss would be permanent and silent.
    QByteArray chunk(kChunkSize, Qt::Uninitialized);
    qint64 written = 0;
    for (;;) {
        if (isCancelRequested())
            return false;

        const qint64 got = from->read(chunk.data(), kChunkSize);
        if (got < 0) {
            m_failures.append(QStringLiteral("%1: the source stopped after %2 bytes: %3")
                                  .arg(step.relativePath)
                                  .arg(written)
                                  .arg(from->errorString()));
            return false;
        }
        if (got == 0)
            break;

        // The reason goes in the message. A destination that filled up, one
        // whose connection went away and one whose file was pulled out from
        // under it were all "short write", and which of them it was is the only
        // part anybody can act on.
        const qint64 put = to->write(chunk.constData(), got);
        if (put != got) {
            m_failures.append(QStringLiteral("%1: the destination took %2 of %3 bytes and stopped: %4")
                                  .arg(step.relativePath)
                                  .arg(written + qMax<qint64>(put, 0))
                                  .arg(written + got)
                                  .arg(to->errorString()));
            return false;
        }

        written += got;
        // Throughput and the moving bar come from here, so a single large file
        // is not a frozen interface. From this task's own running total, never
        // read back from what was last reported -- see m_bytesCopied.
        setBytesDone(m_bytesCopied + written);
    }

    // A read that ended early and a file that shrank look exactly alike from
    // here: both hand over fewer bytes than the plan said and then report the
    // end of the file. Only the source can tell them apart, so it is asked --
    // once, and only when there is a discrepancy to explain. A file that really
    // is smaller now is copied as it now is; a source that still claims the
    // larger size gave a short answer. See ADR-0027.
    //
    // Before the destination is closed, because closing is what puts it in
    // place: a copy about to be called a failure must not first be renamed into
    // the name somebody asked for.
    if (step.bytes > 0 && written < step.bytes) {
        const Result<FileEntry> now = m_sourceFs->stat(step.source);
        if (!now.ok() || now.value().size != written) {
            m_failures.append(QStringLiteral("%1: the source said %2 bytes and gave %3")
                                  .arg(step.relativePath)
                                  .arg(step.bytes)
                                  .arg(written));
            return false;
        }
    }

    // Closing is where a buffered backend actually commits, so it happens before
    // anything stats the result -- and it is where a remote write reports that it
    // failed, which is why the outcome is collected rather than assumed.
    const Result<void> committed = closeAndReport(*to);
    from->close();
    if (!committed.ok()) {
        m_failures.append(QStringLiteral("%1: %2").arg(step.relativePath, committed.error().message));
        return false;
    }
    // Only once the file is committed. A step that failed part way leaves the
    // destination without it, so counting its bytes would make the total say
    // more arrived than did.
    m_bytesCopied += written;
    setBytesDone(m_bytesCopied);
    m_arrivals.append(Arrival { step.target, written });
    return true;
}

void SyncTask::verifyArrivals()
{
    if (m_arrivals.isEmpty())
        return;

    // By directory rather than by file: asking about one file at a time is a
    // round trip each, and on SFTP a stat *is* a listing of the parent. One
    // listing per directory answers for everything that landed in it.
    QHash<QString, QList<const Arrival*>> byDirectory;
    for (const Arrival& arrival : std::as_const(m_arrivals))
        byDirectory[arrival.target.parent().toString()].append(&arrival);

    for (auto it = byDirectory.constBegin(); it != byDirectory.constEnd(); ++it) {
        if (isCancelRequested())
            return;

        const Result<FileEntryList> listing = m_targetFs->list(VfsUri::fromString(it.key()), cancelToken());
        if (!listing.ok()) {
            // Not being able to look is not the same as finding something wrong.
            // Calling a sync failed because the check could not run would be its
            // own kind of lie.
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
                m_failures.append(
                    QStringLiteral("%1: was copied but is not there afterwards").arg(arrival->target.path()));
                continue;
            }
            if (sizes.value(name) != arrival->bytes) {
                m_failures.append(QStringLiteral("%1: %2 bytes were sent but %3 arrived")
                                      .arg(arrival->target.path())
                                      .arg(arrival->bytes)
                                      .arg(sizes.value(name)));
            }
        }
    }
}

void SyncTask::run()
{
    if (!m_sourceFs || !m_targetFs) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("One side has no drive mounted")));
        return;
    }

    // A plan handed in is the plan somebody agreed to, and comparing again here
    // is the fault this constructor exists for -- the second walk sees a source
    // that has moved on and produces deletions the confirmation never listed.
    if (!m_planWasGiven) {
        setStatusText(QStringLiteral("comparing…"));
        m_plan = SyncPlan::build(
            m_sourceFs.get(), m_source, m_targetFs.get(), m_target, m_options, cancelToken());
    }
    if (isCancelRequested())
        return;

    const QLocale locale;
    reportCount(
        QStringLiteral("copy"), QStringLiteral("To copy"), m_plan.countOf(SyncPlan::Action::Copy), 10);
    reportCount(QStringLiteral("replace"), QStringLiteral("To replace"),
        m_plan.countOf(SyncPlan::Action::Overwrite), 20);
    if (m_plan.countOf(SyncPlan::Action::Link) > 0) {
        reportCount(
            QStringLiteral("link"), QStringLiteral("To link"), m_plan.countOf(SyncPlan::Action::Link), 25);
    }
    if (m_plan.countOf(SyncPlan::Action::Delete) > 0) {
        reportCount(QStringLiteral("delete"), QStringLiteral("To delete"),
            m_plan.countOf(SyncPlan::Action::Delete), 30);
    }

    // A directory the comparison could not read produced no steps at all, which
    // keeps the plan safe -- but it also means this sync is not the mirror it
    // was asked for, and saying nothing about that would leave somebody
    // believing the two trees now match.
    for (const QString& path : m_plan.unreadable())
        m_failures.append(QStringLiteral("%1: could not be read, so nothing was planned for it").arg(path));

    emit planReady(m_plan);

    if (m_options.dryRun) {
        setProgress(100);
        setStatusText(m_plan.isEmpty()
                ? QStringLiteral("already in step")
                : QStringLiteral("%1 changes · %2 (nothing written)")
                      .arg(m_plan.steps().size() - m_plan.countOf(SyncPlan::Action::Skip))
                      .arg(locale.formattedDataSize(m_plan.bytesToTransfer())));
        return;
    }

    setByteTotal(m_plan.bytesToTransfer());

    for (const SyncPlan::Step& step : m_plan.steps()) {
        if (isCancelRequested())
            return;

        switch (step.action) {
        case SyncPlan::Action::Skip:
            continue;
        case SyncPlan::Action::CreateDirectory:
            if (Result<void> made = m_targetFs->makeDirectory(step.target); !made.ok()) {
                m_failures.append(QStringLiteral("%1: %2").arg(step.relativePath, made.error().message));
            } else {
                ++m_applied;
            }
            break;
        case SyncPlan::Action::Copy:
        case SyncPlan::Action::Overwrite:
            if (copyOne(step))
                ++m_applied;
            break;
        case SyncPlan::Action::Link:
            if (linkOne(step))
                ++m_applied;
            break;
        case SyncPlan::Action::Delete: {
            // Asked again, because this is the step that cannot be undone and
            // the plan may be minutes old. A destination that has changed under
            // it keeps what it has, and the run says which one and why.
            QString changed;
            if (!stillWhatThePlanSaw(step, &changed)) {
                m_failures.append(QStringLiteral("%1: not deleted -- %2").arg(step.relativePath, changed));
                break;
            }
            if (Result<void> removed = m_targetFs->remove(step.target, true); !removed.ok()) {
                m_failures.append(QStringLiteral("%1: %2").arg(step.relativePath, removed.error().message));
            } else {
                ++m_applied;
            }
            break;
        }
        }

        setStatusText(step.relativePath);
        reportCount(QStringLiteral("done"), QStringLiteral("Done"), m_applied, 5);
        if (!m_failures.isEmpty()) {
            reportCount(QStringLiteral("failed"), QStringLiteral("Failed"),
                static_cast<double>(m_failures.size()), 40);
        }
    }

    // What the destination says it now holds, against what was sent to it. A
    // server that acknowledges bytes and stores fewer is caught by a transfer
    // and used to pass a sync -- and a sync is the one that runs unattended and
    // then reports the two trees as matching. See ADR-0016.
    verifyArrivals();

    setProgress(100);
    setStatusText(m_failures.isEmpty()
            ? QStringLiteral("%1 changes applied").arg(m_applied)
            : QStringLiteral("%1 applied, %2 failed").arg(m_applied).arg(m_failures.size()));
}

} // namespace mole
