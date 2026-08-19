#include "plugins/builtin/IndexScanJob.h"

#include "sdk/ScanReaders.h"

#include "core/events/EventBus.h"
#include "core/index/ScanTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QUuid>

namespace mole {

IndexScanJob::IndexScanJob(PluginServices services, QObject* parent)
    : QObject(parent)
    , m_services(services)
{
}

ScanOptions IndexScanJob::optionsFor(const ScheduleRule& rule)
{
    ScanOptions options;
    // Incremental unless the rule says otherwise, which is the whole point of
    // running this every night rather than once a quarter.
    options.incremental = rule.parameters.value(incrementalParameter(), true).toBool();
    options.metadata = rule.parameters.value(metadataParameter(), false).toBool();
    options.archives = rule.parameters.value(archivesParameter(), false).toBool();
    return options;
}

ScheduleRule IndexScanJob::ruleFor(const ScheduleStore& store, const QString& rootUri)
{
    const QList<ScheduleRule> rules = store.rules();
    for (const ScheduleRule& rule : rules) {
        if (rule.jobKind == kind() && rule.parameters.value(rootUriParameter()).toString() == rootUri)
            return rule;
    }
    return {};
}

QString IndexScanJob::schedule(ScheduleStore& store, const QString& rootUri, qint64 seconds,
    const ScanOptions& options, const QString& label)
{
    ScheduleRule rule = ruleFor(store, rootUri);

    if (seconds <= 0) {
        if (rule.isValid()) {
            store.remove(rule.id);
            store.save();
        }
        return {};
    }

    if (!rule.isValid()) {
        rule.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        rule.jobKind = kind();
    }
    rule.label = label;
    // Every option the scan was asked for, not just the incremental flag: a rule
    // that carried less rebuilt the volume as a poorer scan every night.
    rule.parameters = { { rootUriParameter(), rootUri }, { incrementalParameter(), options.incremental },
        { metadataParameter(), options.metadata }, { archivesParameter(), options.archives } };
    // Changed rather than left alone: choosing a different interval for a folder
    // that already had one used to do nothing at all.
    rule.intervalSeconds = seconds;
    rule.enabled = true;
    store.put(rule);
    store.save();
    return rule.id;
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

    auto* task = new ScanTask(fs, root, rule.label, m_services.index);
    // Everything the rule asks for, readers and all. Setting only the
    // incremental flag here is what quietly stripped the metadata and the
    // archive rows out of every subtree a nightly run re-walked.
    applyScanOptions(*task, optionsFor(rule), m_services, fs, root);

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
