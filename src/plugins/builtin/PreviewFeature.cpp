#include "plugins/builtin/PreviewFeature.h"

#include "core/settings/Preferences.h"
#include "core/tasks/ListDirectoryTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <algorithm>

namespace mole {

PreviewTabController::PreviewTabController(PluginServices services, QObject* parent)
    : FeatureController(QStringLiteral("Preview"), parent)
    , m_services(services)
{
}

PreviewTabController::~PreviewTabController()
{
    if (m_listing)
        m_listing->requestCancel();
}

QString PreviewTabController::folderPath() const
{
    if (!m_current.uri.isValid())
        return {};
    const VfsUri parent = m_current.uri.parent();
    return parent.scheme() == QLatin1String("file") ? parent.path() : parent.toString();
}

QObject* PreviewTabController::viewer() const
{
    return m_viewer.data();
}

int PreviewTabController::position() const
{
    for (int i = 0; i < m_siblings.size(); ++i) {
        if (m_siblings.at(i).uri == m_current.uri)
            return i + 1;
    }
    return 0;
}

void PreviewTabController::open(const QString& uri)
{
    const VfsUri target = VfsUri::fromString(uri);
    if (!target.isValid())
        return;

    if (!m_services.isValid()) {
        setSubtitle(QStringLiteral("Application services are not available"));
        return;
    }

    FileSystemPtr fs = m_services.vfs->resolve(target);
    if (!fs) {
        setTitle(target.fileName());
        setSubtitle(QStringLiteral("No drive is mounted for this file"));
        return;
    }

    // stat() is on a worker thread everywhere else, but here the entry is
    // usually already known and the neighbours load asynchronously anyway.
    Result<FileEntry> stat = fs->stat(target);
    FileEntry entry;
    if (stat.ok()) {
        entry = stat.value();
    } else {
        entry.name = target.fileName();
        entry.uri = target;
    }

    showEntry(entry);
    loadSiblings(target.parent(), target);
}

void PreviewTabController::showEntry(const FileEntry& entry)
{
    m_current = entry;
    setTitle(entry.name.isEmpty() ? QStringLiteral("Preview") : entry.name);

    // The old viewer goes before the new one arrives, so a heavy one does not
    // sit in memory while the next file loads.
    if (m_viewer) {
        m_viewer->deleteLater();
        m_viewer.clear();
    }
    m_viewSource = QUrl();
    m_viewerName.clear();

    IPreviewProvider* provider = m_services.previews ? m_services.previews->providerFor(entry) : nullptr;
    if (!provider) {
        setSubtitle(QStringLiteral("Nothing can show this file"));
        emit currentChanged();
        return;
    }

    m_viewerName = provider->displayName();
    m_viewSource = provider->viewSource();
    m_providerId = provider->id();

    auto* controller = provider->createController(this);
    m_viewer = controller;

    // What this viewer offers for this file, and what was chosen last time. Applied
    // before load(), so the file is read once and shown the way it was asked for
    // rather than shown one way and then the other.
    m_viewerOptions.clear();
    const QList<ViewerOption> declared = provider->options(entry);
    for (const ViewerOption& option : declared) {
        const QString chosen = rememberedChoice(option, entry);
        if (controller)
            controller->setViewerOption(option.key, chosen);

        m_viewerOptions.append(
            QVariantMap { { QStringLiteral("key"), option.key }, { QStringLiteral("title"), option.title },
                { QStringLiteral("choices"), option.choices }, { QStringLiteral("chosen"), chosen } });
    }

    if (controller)
        controller->load(entry);

    setSubtitle(folderPath());
    emit currentChanged();
}

void PreviewTabController::loadSiblings(const VfsUri& directory, const VfsUri& select)
{
    if (m_listing)
        m_listing->requestCancel();

    FileSystemPtr fs = m_services.vfs->resolve(directory);
    if (!fs)
        return;

    auto* task = new ListDirectoryTask(std::move(fs), directory);
    m_listing = task;

    connect(task, &ListDirectoryTask::listed, this,
        [this, task, select](const VfsUri&, const FileEntryList& entries) {
            if (m_listing != task)
                return;

            // Files only, in the same order the browser shows them, so
            // stepping matches what the user just saw.
            m_siblings.clear();
            for (const FileEntry& entry : entries) {
                if (!entry.isDir)
                    m_siblings.append(entry);
            }
            std::sort(m_siblings.begin(), m_siblings.end(),
                [](const FileEntry& a, const FileEntry& b) { return a.name.localeAwareCompare(b.name) < 0; });

            // Keep whatever the user is looking at as the current entry even
            // if it is somehow missing from the listing.
            for (const FileEntry& entry : std::as_const(m_siblings)) {
                if (entry.uri == select) {
                    m_current = entry;
                    break;
                }
            }
            emit currentChanged();
        });

    connect(task, &Task::finished, this, [this, task] {
        if (m_listing == task)
            m_listing.clear();
    });

    m_services.tasks->submit(task);
}

void PreviewTabController::step(int delta)
{
    const int here = position();
    if (here == 0)
        return;

    const int target = here - 1 + delta;
    if (target < 0 || target >= m_siblings.size())
        return;

    showEntry(m_siblings.at(target));
}

void PreviewTabController::next()
{
    step(1);
}

void PreviewTabController::previous()
{
    step(-1);
}

QVariantMap PreviewTabController::saveState() const
{
    return { { QStringLiteral("uri"), currentUri() } };
}

void PreviewTabController::restoreState(const QVariantMap& state)
{
    const QString uri = state.value(QStringLiteral("uri")).toString();
    if (!uri.isEmpty())
        open(uri);
}

PreviewFeature::PreviewFeature(PluginServices services)
    : m_services(services)
{
}

QUrl PreviewFeature::viewSource() const
{
    return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/PreviewView.qml"));
}

QString PreviewTabController::preferenceKey(const QString& optionKey, const FileEntry& entry) const
{
    // Provider and suffix both: per suffix because "the next .html" is what was
    // asked for, and one text viewer serves .html, .xml and .svg with different
    // sensible answers; the provider id so two viewers claiming a suffix cannot
    // overwrite each other. See ADR-0006.
    return QStringLiteral("preview.%1.%2.%3").arg(m_providerId, entry.uri.suffix().toLower(), optionKey);
}

QString PreviewTabController::rememberedChoice(const ViewerOption& option, const FileEntry& entry) const
{
    if (!m_services.preferences)
        return option.defaultChoice;

    const QString remembered
        = m_services.preferences->value(preferenceKey(option.key, entry), option.defaultChoice).toString();
    // A choice that is no longer offered falls back rather than being obeyed: the
    // viewer's options can change between versions.
    return option.choices.contains(remembered) ? remembered : option.defaultChoice;
}

void PreviewTabController::chooseViewerOption(const QString& key, const QString& value)
{
    if (m_services.preferences)
        m_services.preferences->setValue(preferenceKey(key, m_current), value);

    // Applied to what is on screen now as well as remembered for next time, so
    // nothing has to be reopened by hand.
    if (auto* controller = qobject_cast<PreviewController*>(m_viewer.data()))
        controller->setViewerOption(key, value);

    for (QVariant& entry : m_viewerOptions) {
        QVariantMap option = entry.toMap();
        if (option.value(QStringLiteral("key")).toString() == key) {
            option[QStringLiteral("chosen")] = value;
            entry = option;
        }
    }
    emit currentChanged();
}

FeatureController* PreviewFeature::createController(QObject* parent)
{
    return new PreviewTabController(m_services, parent);
}

} // namespace mole
