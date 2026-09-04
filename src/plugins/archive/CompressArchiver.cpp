#include "plugins/archive/CompressArchiver.h"

#include "plugins/archive/CompressTask.h"

#include "core/events/EventBus.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

namespace mole {

CompressArchiver::CompressArchiver(PluginServices services)
    : m_services(services)
{
}

QList<IArchiver::Format> CompressArchiver::formats() const
{
    // Read off CompressTask's own table rather than typed again here: which kinds
    // exist, what each is called and what it can carry are facts about the writer,
    // and a second list of them is the fault this whole change is about.
    QList<Format> offered;
    for (const QString& name : CompressTask::formatNames()) {
        const CompressTask::Format kind = CompressTask::formatFromName(name);
        Format format;
        format.id = name;
        format.suffix = CompressTask::suffixFor(kind);
        format.takesPassword = CompressTask::formatSupportsPassword(kind);
        format.holdsOneFileOnly = CompressTask::takesOneFileOnly(kind);
        offered.append(format);
    }
    return offered;
}

bool CompressArchiver::compress(const Request& request)
{
    if (request.sources.isEmpty() || request.target.path().isEmpty())
        return false;
    // A format this plugin does not write is refused rather than written as a zip
    // under a name that says otherwise -- the same rule formatIfKnown() is for.
    const std::optional<CompressTask::Format> kind = CompressTask::formatIfKnown(request.formatId);
    if (!kind)
        return false;
    if (!m_services.vfs || !m_services.tasks)
        return false;

    CompressTask::Request work;
    work.sources = request.sources;
    work.target = request.target;
    work.format = *kind;
    work.passphrase = request.passphrase;
    work.removeSourcesWhenDone = request.removeSourcesWhenDone;
    work.sourceFileSystem = m_services.vfs->resolve(work.sources.first());
    work.targetFileSystem = m_services.vfs->resolve(work.target);
    if (!work.sourceFileSystem || !work.targetFileSystem) {
        if (m_services.events) {
            m_services.events->postNotification(EventBus::Severity::Warning,
                QStringLiteral("Cannot compress"), QStringLiteral("No drive is mounted for this"));
        }
        return false;
    }

    auto* task = new CompressTask(work);
    EventBus* events = m_services.events;
    const VfsUri target = work.target;
    QObject::connect(task, &Task::finished, task, [task, events, target] {
        if (!events)
            return;
        if (task->state() == Task::State::Failed) {
            events->postNotification(
                EventBus::Severity::Warning, QStringLiteral("Compression failed"), task->error().message);
            return;
        }
        if (task->state() != Task::State::Succeeded)
            return;
        // Announced entry by entry, so a second pane on the same folder stops
        // showing files that are no longer there. The plugin says this rather
        // than the shell: what was removed is something only the task knows.
        for (const VfsUri& removed : task->removedSources())
            events->postEntryRemoved(removed);
        // And the listing has a new file in it, which whoever asked wants to see.
        events->postDirectoryChanged(target.parent());
    });
    m_services.tasks->submit(task);
    return true;
}

} // namespace mole
