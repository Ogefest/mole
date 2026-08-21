#include "core/tasks/InvokeFileActionTask.h"

namespace mole {

InvokeFileActionTask::InvokeFileActionTask(
    FileSystemPtr fileSystem, QString id, QString title, VfsUri target, QObject* parent)
    : Task(title, parent)
    , m_fileSystem(std::move(fileSystem))
    , m_id(std::move(id))
    , m_title(std::move(title))
    , m_target(std::move(target))
{
}

void InvokeFileActionTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("There is no drive here any more")));
        return;
    }

    setProgress(kIndeterminateProgress);
    Result<FileActionOutcome> outcome = m_fileSystem->invoke(m_id, m_target, cancelToken());
    if (!outcome.ok()) {
        fail(outcome.error());
        return;
    }

    // A drive that answers with neither text nor uris has said it did something
    // it did not do, and there is nothing to show for it. Caught here rather
    // than left to whatever draws the answer, which would show an empty box.
    if (!outcome.value().isValid()) {
        fail(VfsError::make(VfsError::Unknown, QStringLiteral("The drive answered with nothing")));
        return;
    }

    m_outcome = outcome.value();
    setProgress(100);
}

} // namespace mole
