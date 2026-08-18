#include "core/vcs/ReadRepositoryTask.h"

namespace mole {

ReadRepositoryTask::ReadRepositoryTask(QString localPath, QObject* parent)
    : Task(QStringLiteral("Reading git state"), parent)
    , m_localPath(std::move(localPath))
{
    setBackground(true);
}

void ReadRepositoryTask::run()
{
    if (isCancelRequested())
        return;

    // Through the cache, so a second folder in the same checkout costs a
    // discovery rather than an open.
    const std::shared_ptr<Repository> repository = RepositoryCache::shared().forPath(m_localPath);
    if (!repository) {
        // Not a failure. A folder that is not in a work tree is the ordinary case
        // and the band has to hear about it, or it would keep showing the branch
        // of the checkout somebody has just navigated out of.
        setStatusText(QStringLiteral("not a checkout"));
        emit repositoryRead(m_localPath, QString(), RepositoryHead {});
        return;
    }

    m_root = repository->root();
    m_gitDir = repository->gitDir();
    m_head = repository->head();
    setStatusText(m_head.branch.isEmpty() ? m_head.shortId : m_head.branch);
    emit repositoryRead(m_localPath, m_root, m_head);
}

} // namespace mole
