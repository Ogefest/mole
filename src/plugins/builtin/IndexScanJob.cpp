#include "plugins/builtin/IndexScanJob.h"

#include "core/events/EventBus.h"
#include "core/index/ScanTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

namespace mole {

IndexScanJob::IndexScanJob(PluginServices services, QObject* parent)
    : QObject(parent)
    , m_services(services)
{
}

bool IndexScanJob::start(const ScheduleRule& rule, std::function<void(bool, QString)> done)
{
    if (!m_services.isValid() || !m_services.index)
        return false;

    const QString rootUri = rule.parameters.value(rootUriParameter()).toString();
    if (rootUri.isEmpty())
        return false;

    const VfsUri root = VfsUri::fromString(rootUri);
    FileSystemPtr fs = m_services.vfs->resolve(root);
    if (!fs) {
        // Not a failure of the job so much as of the world: an unplugged drive
        // reads as "could not run", which the scheduler records rather than
        // counting as a failure of the rule.
        return false;
    }

    auto* task = new ScanTask(std::move(fs), root, rule.label, m_services.index);
    // Incremental unless the rule says otherwise, which is the whole point of
    // running this every night rather than once a quarter.
    task->setIncremental(rule.parameters.value(incrementalParameter(), true).toBool());

    connect(task, &Task::finished, this, [this, task, rootUri, done] {
        const bool ok = task->state() == Task::State::Succeeded;
        QString message;
        if (ok) {
            message = task->statusText();
            if (m_services.events)
                m_services.events->postIndexUpdated(-1, task->filesIndexed());
            emit volumeScanned(rootUri);
        } else if (task->state() == Task::State::Failed) {
            message = task->error().message;
        } else {
            message = QStringLiteral("Cancelled");
        }
        done(ok, message);
    });

    m_services.tasks->submit(task);
    return true;
}

} // namespace mole
