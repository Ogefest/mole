#include "core/vcs/ReadStatusTask.h"

namespace mole {

ReadStatusTask::ReadStatusTask(QString localPath, QObject* parent)
    : Task(QStringLiteral("Reading git status"), parent)
    , m_localPath(std::move(localPath))
{
    setBackground(true);
}

void ReadStatusTask::run()
{
    if (isCancelRequested())
        return;

    const std::shared_ptr<Repository> repository = RepositoryCache::shared().forPath(m_localPath);
    if (!repository) {
        setStatusText(QStringLiteral("not a checkout"));
        return;
    }
    m_root = repository->root();

    // Checked here as well as by the caller, because a task can sit in the queue
    // behind a copy: by the time it runs, the pane that asked may already have its
    // answer from a walk somebody else started.
    const RepositoryStatus known = RepositoryStatusCache::shared().forRoot(m_root);
    if (known.complete) {
        m_cached = true;
        m_status = known;
        setStatusText(QStringLiteral("already known"));
        emit statusRead(m_root, m_status);
        return;
    }

    m_status = repository->readStatus(cancelToken());
    if (!m_status.complete) {
        // Cancelled, or git gave up part way. Nothing is emitted and nothing is
        // cached: the pane goes on showing the count it had, which is out of date,
        // where showing a partial walk would be wrong.
        setStatusText(QStringLiteral("abandoned"));
        return;
    }

    RepositoryStatusCache::shared().store(m_root, m_status);
    setStatusText(m_status.changedCount == 0 ? QStringLiteral("clean")
                                             : QStringLiteral("%1 changed").arg(m_status.changedCount));
    emit statusRead(m_root, m_status);
}

} // namespace mole
