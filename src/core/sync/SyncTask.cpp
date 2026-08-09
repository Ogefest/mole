#include "core/sync/SyncTask.h"

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

    while (!from->atEnd()) {
        if (isCancelRequested())
            return false;
        const QByteArray chunk = from->read(kChunkSize);
        if (chunk.isEmpty())
            break;
        if (to->write(chunk) != chunk.size()) {
            m_failures.append(QStringLiteral("%1: short write").arg(step.relativePath));
            return false;
        }
        // Throughput and the moving bar come from here, so a single large file
        // is not a frozen interface.
        setBytesDone(bytesDone() < 0 ? chunk.size() : bytesDone() + chunk.size());
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
    return true;
}

void SyncTask::run()
{
    if (!m_sourceFs || !m_targetFs) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("One side has no drive mounted")));
        return;
    }

    setStatusText(QStringLiteral("comparing…"));
    m_plan
        = SyncPlan::build(m_sourceFs.get(), m_source, m_targetFs.get(), m_target, m_options, cancelToken());
    if (isCancelRequested())
        return;

    const QLocale locale;
    reportCount(
        QStringLiteral("copy"), QStringLiteral("To copy"), m_plan.countOf(SyncPlan::Action::Copy), 10);
    reportCount(QStringLiteral("replace"), QStringLiteral("To replace"),
        m_plan.countOf(SyncPlan::Action::Overwrite), 20);
    if (m_plan.countOf(SyncPlan::Action::Delete) > 0) {
        reportCount(QStringLiteral("delete"), QStringLiteral("To delete"),
            m_plan.countOf(SyncPlan::Action::Delete), 30);
    }

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
        case SyncPlan::Action::Delete:
            if (Result<void> removed = m_targetFs->remove(step.target, true); !removed.ok()) {
                m_failures.append(QStringLiteral("%1: %2").arg(step.relativePath, removed.error().message));
            } else {
                ++m_applied;
            }
            break;
        }

        setStatusText(step.relativePath);
        reportCount(QStringLiteral("done"), QStringLiteral("Done"), m_applied, 5);
        if (!m_failures.isEmpty()) {
            reportCount(QStringLiteral("failed"), QStringLiteral("Failed"),
                static_cast<double>(m_failures.size()), 40);
        }
    }

    setProgress(100);
    setStatusText(m_failures.isEmpty()
            ? QStringLiteral("%1 changes applied").arg(m_applied)
            : QStringLiteral("%1 applied, %2 failed").arg(m_applied).arg(m_failures.size()));
}

} // namespace mole
