#include "plugins/builtin/AnalysisJob.h"

#include "core/analysis/AnalyseDirectoryTask.h"
#include "core/analysis/AnalysisStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <memory>

namespace mole {

AnalysisJob::AnalysisJob(PluginServices services, AnalysisStore* store, QObject* parent)
    : QObject(parent)
    , m_services(services)
    , m_store(store)
{
}

StartOutcome AnalysisJob::start(const ScheduleRule& rule, std::function<void(bool, QString)> done)
{
    if (!m_services.isValid() || !m_store)
        return StartOutcome::failed(QStringLiteral("Reports are not available in this run"));

    const QString rootUri = rule.parameters.value(rootUriParameter()).toString();
    if (rootUri.isEmpty())
        return StartOutcome::failed(QStringLiteral("This rule names no folder to report on"));

    const VfsUri root = VfsUri::fromString(rootUri);
    FileSystemPtr fs = m_services.vfs->resolve(root);
    if (!fs) {
        // Not a failure of the job so much as of the world: an unplugged drive
        // reads as "could not run", and now says so in its own words. It used to
        // return false, which the scheduler recorded as a failure of the rule --
        // so a backup disk left out for a week read as "Failed x7". See MOLE-379.
        return StartOutcome::skipped(QStringLiteral("No drive is mounted for %1").arg(rootUri));
    }

    auto* task = new AnalyseDirectoryTask(std::move(fs), root, rule.label);

    // Shared rather than owned by the finish handler: when the application is
    // torn down mid-scan, `finished` never arrives and a raw pointer here
    // would simply be lost. The flag dies with the last connection instead.
    auto stored = std::make_shared<bool>(false);

    connect(task, &AnalyseDirectoryTask::reportReady, this,
        [this, stored, rootUri](const AnalysisReport& report) {
            // A report that did not reach the disk is not a run that finished:
            // the whole point of a scheduled report is that it is there in the
            // morning. AnalysisStore has already said why in the log.
            if (!m_store->save(report))
                return;
            m_store->prune(rootUri, m_historyKept);
            *stored = true;
            emit reportStored(rootUri);
        });

    connect(task, &Task::finished, this, [task, stored, done] {
        const bool ok = *stored && task->state() == Task::State::Succeeded;
        QString message;
        if (ok) {
            message = task->statusText();
        } else if (task->state() == Task::State::Failed) {
            message = task->error().message;
        } else if (task->state() == Task::State::Cancelled) {
            message = QStringLiteral("Cancelled");
        } else {
            message = QStringLiteral("The report produced nothing");
        }
        done(ok, message);
    });

    m_services.tasks->submit(task);
    return StartOutcome::started();
}

} // namespace mole
