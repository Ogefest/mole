#include "plugins/builtin/PreviewFeature.h"

#include "plugins/builtin/previews/MetadataReaders.h"
#include "sdk/IMetadataReader.h"

#include "core/data/FileType.h"
#include "core/settings/Preferences.h"
#include "core/tasks/InvokeFileActionTask.h"
#include "core/tasks/ListDirectoryTask.h"
#include "core/tasks/QueryFileActionsTask.h"
#include "core/tasks/ReadRangeTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QClipboard>
#include <QGuiApplication>

#include <algorithm>

namespace mole {
namespace {

    /// One key for every preview, not one per file type. See setDetailsOpen().
    const QString kDetailsOpenKey = QStringLiteral("preview.details.open");
    const QString kDetailsWidthKey = QStringLiteral("preview.details.width");

} // namespace

PreviewTabController::PreviewTabController(PluginServices services, QObject* parent)
    : FeatureController(QStringLiteral("Preview"), parent)
    , m_services(services)
{
}

PreviewTabController::~PreviewTabController()
{
    if (m_listing)
        m_listing->requestCancel();
    if (m_sniff)
        m_sniff->requestCancel();
    if (m_details)
        m_details->requestCancel();
    releaseMemberMount();
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
    // A new file is a new question about what else there is of it, and nothing
    // is known until the drive is asked.
    m_versionsAction.clear();
    m_otherVersions.clear();
    if (m_versionsPending)
        m_versionsPending->requestCancel();
    if (m_versionsLookup)
        m_versionsLookup->requestCancel();
    emit versionsChanged();

    showContents(entry);
    lookForVersions();
}

void PreviewTabController::showContents(const FileEntry& entry)
{
    // The old viewer goes before the new one arrives, so a heavy one does not
    // sit in memory while the next file loads.
    if (m_viewer) {
        m_viewer->deleteLater();
        m_viewer.clear();
    }
    m_viewSource = QUrl();
    m_viewerName.clear();
    // A note about the last file would read as a note about this one.
    m_fallbackNote.clear();
    m_provider = nullptr;
    // A file being stepped past takes its unfinished sniff with it, and the
    // readers for it: what they were about to say is about the last file.
    if (m_sniff) {
        m_sniff->requestCancel();
        m_sniff.clear();
    }
    if (m_details) {
        m_details->requestCancel();
        m_details.clear();
    }
    m_head.clear();
    m_detailFacts.clear();
    emit detailsChanged();
    // The wrapper the file before this one was read through goes with it.
    releaseMemberMount();

    // A file compressed on its own is a wrapper, and what a reader wanted to see
    // is the member. Everything from here on asks about m_showing, which is the
    // member when there was one and the file itself otherwise.
    m_showing = entry;
    if (const FileEntry member = singleCompressedMember(entry); member.uri.isValid())
        m_showing = member;
    setTitle(m_showing.name.isEmpty() ? QStringLiteral("Preview") : m_showing.name);
    // Worth persisting -- the session remembers which file a preview was on --
    // and it is how the shell learns this tab has moved to another drive.
    emit stateChanged();

    IPreviewProvider* provider = m_services.previews ? m_services.previews->providerFor(m_showing) : nullptr;

    // Pass one asked the name. When it got no further than the fallback tier --
    // the text viewer, or the list of facts -- the file itself is asked, because
    // that is where the answer can still change: a Dockerfile is text, and a zip
    // called notes.txt is not. See ADR-0033.
    const bool nameWasEnough = !provider || provider->priority() >= 0;
    if (!nameWasEnough && m_showing.mimeType.isEmpty() && m_showing.size != 0) {
        identifyThenShow();
        return;
    }

    installViewer(provider);
}

void PreviewTabController::lookForVersions()
{
    if (!m_services.isValid() || !m_current.uri.isValid())
        return;

    FileSystemPtr fs = m_services.vfs->resolve(m_current.uri);
    if (!fs)
        return;

    // What the drive lists, not what it does: nothing is fetched here, and a
    // drive that offers nothing costs one virtual call that answers with an
    // empty list.
    auto* task = new QueryFileActionsTask(std::move(fs), m_current.uri);
    m_versionsLookup = task;
    connect(task, &Task::finished, this, [this, task] {
        if (m_versionsLookup != task)
            return;
        m_versionsLookup.clear();
        if (task->target() != m_current.uri)
            return;

        for (const FileAction& action : task->actions()) {
            // The one that answers with other uris for this file. A link would
            // be no use here, and asking it would sign one nobody wanted.
            if (action.enabled && action.answers == FileActionKind::Uris) {
                m_versionsAction = action.id;
                m_versionsTitle = action.title;
                break;
            }
        }
        emit versionsChanged();
    });
    m_services.tasks->submit(task);
}

void PreviewTabController::requestVersions()
{
    if (m_versionsAction.isEmpty() || m_versionsPending || !m_otherVersions.isEmpty())
        return;
    if (!m_services.isValid())
        return;

    FileSystemPtr fs = m_services.vfs->resolve(m_current.uri);
    if (!fs)
        return;

    auto* task = new InvokeFileActionTask(std::move(fs), m_versionsAction, m_versionsTitle, m_current.uri);
    m_versionsPending = task;
    m_versionsError.clear();
    emit versionsChanged();

    connect(task, &Task::finished, this, [this, task] {
        if (m_versionsPending != task)
            return;
        m_versionsPending.clear();

        if (task->state() == Task::State::Failed) {
            // Said where somebody is looking, and it says which action: an empty
            // chooser that explains nothing is worse than no chooser.
            m_versionsError = QStringLiteral("%1: %2").arg(task->actionTitle(), task->error().message);
            emit versionsChanged();
            return;
        }

        QVariantList found;
        for (const VfsUri& uri : task->outcome().uris) {
            found.append(QVariantMap { { QStringLiteral("uri"), uri.toString() },
                { QStringLiteral("label"), uri.hasVersion() ? uri.version() : uri.fileName() } });
        }
        m_otherVersions = found;
        emit versionsChanged();
    });
    m_services.tasks->submit(task);
}

void PreviewTabController::showVersion(const QString& uri)
{
    if (!m_current.uri.isValid())
        return;

    // Empty means the file as it is, which is where a preview starts and where
    // this is the way back to.
    const VfsUri target = uri.isEmpty() ? m_current.uri : VfsUri::fromString(uri);
    if (!target.isValid() || target.withoutVersion() != m_current.uri.withoutVersion())
        return;
    if (target == m_showing.uri)
        return;

    FileEntry entry;
    // stat() on this thread, for the reason open() gives about itself: it is one
    // call about one file that the drive has just listed for us.
    if (FileSystemPtr fs = m_services.vfs ? m_services.vfs->resolve(target) : nullptr) {
        if (const Result<FileEntry> stat = fs->stat(target); stat.ok())
            entry = stat.value();
    }
    if (!entry.uri.isValid()) {
        entry.name = m_current.name;
        entry.uri = target;
    }

    // Contents only. The tab goes on being about the file it is about, so the
    // arrows still step through the folder and the session still records the
    // file rather than a state of it that may not be there next time.
    showContents(entry);
    emit currentChanged();
}

void PreviewTabController::identifyThenShow()
{
    FileSystemPtr fs = m_services.vfs ? m_services.vfs->resolve(m_showing.uri) : nullptr;
    if (!fs || !m_services.tasks) {
        installViewer(m_services.previews ? m_services.previews->providerFor(m_showing) : nullptr);
        return;
    }

    // One page, off the UI thread, and not snapped to line boundaries: what is
    // wanted is the first bytes of the file whatever they turn out to be.
    auto* task = new ReadRangeTask(std::move(fs), m_showing.uri, 0, FileType::kSampleBytes);
    task->setAlignToLines(false);
    m_sniff = task;

    connect(task, &Task::finished, this, [this, task, uri = m_showing.uri] {
        if (m_sniff != task)
            return;
        m_sniff.clear();
        if (m_showing.uri != uri)
            return;

        // A file that cannot be read still gets a viewer: the name's answer
        // stands, which is what happened before there was a second pass at all.
        if (task->state() == Task::State::Succeeded) {
            m_head = task->contents();
            m_showing.mimeType = FileType::identify(m_showing.name, m_head);
        }

        installViewer(m_services.previews ? m_services.previews->providerFor(m_showing) : nullptr);
    });

    m_services.tasks->submit(task);
    // So the view says it is looking rather than saying nothing can show this.
    emit currentChanged();
}

FileEntry PreviewTabController::singleCompressedMember(const FileEntry& entry)
{
    // The cheap test first, and it is also the one that keeps a tarball out: a
    // container has many members and F3 on it goes on doing what it always did.
    // Confirming one member costs a header read, so it must not be run on
    // something the name has already ruled out.
    if (entry.isDir || !FileType::namesSingleCompressedStream(entry.name))
        return {};
    if (!m_services.vfs)
        return {};

    // Only a local file, which is the same limit opening one as a drive has.
    const QString localPath = entry.uri.toLocalPath();
    if (localPath.isEmpty())
        return {};

    // Whichever backend claims this kind of file, which is a plugin's business
    // and not this one's. A build without the archive plugin has no factory that
    // does, and then this whole feature is quietly absent rather than broken.
    const QString suffix = entry.uri.suffix();
    IFileSystemFactory* opener = nullptr;
    for (IFileSystemFactory* factory : m_services.vfs->factories()) {
        if (factory->mountableFileSuffixes().contains(suffix)) {
            opener = factory;
            break;
        }
    }
    if (!opener)
        return {};

    QString error;
    FileSystemPtr inside = opener->create(opener->configForFile(localPath), &error);
    if (!inside)
        return {}; // corrupt, encrypted, or nothing this build can open

    const VfsUri root = opener->rootUriForFile(localPath);
    const Result<FileEntryList> listed = inside->list(root, CancelToken {});
    // Exactly one member, and a file: anything else is a container, or a `.gz`
    // that is not gzip at all, and both keep today's behaviour.
    if (!listed.ok() || listed.value().size() != 1 || listed.value().first().isDir)
        return {};

    // A viewer reads by resolving a uri, so the wrapper has to be mounted -- and
    // it must not become a drive in the sidebar for the length of a preview.
    Mount mount;
    mount.id = QStringLiteral("preview-member:") + root.authority();
    mount.displayName = entry.name;
    mount.root = root;
    mount.fileSystem = std::move(inside);
    mount.internal = true;
    m_memberMountId = m_services.vfs->addMount(mount);
    if (m_memberMountId.isEmpty())
        return {};
    m_memberMountOwner = m_services.vfs;

    return listed.value().first();
}

void PreviewTabController::releaseMemberMount()
{
    if (m_memberMountId.isEmpty())
        return;
    // The manager may already be gone: an application shutting down destroys it
    // before the tabs it owns, and this runs from a destructor as well as from a
    // file change. When it has gone the mount table went with it.
    if (m_memberMountOwner)
        m_memberMountOwner->removeMount(m_memberMountId);
    m_memberMountId.clear();
    m_memberMountOwner.clear();
}

void PreviewTabController::installViewer(IPreviewProvider* provider)
{
    if (!provider) {
        setSubtitle(QStringLiteral("Nothing can show this file"));
        emit currentChanged();
        return;
    }

    m_viewerName = provider->displayName();
    m_viewSource = provider->viewSource();
    m_providerId = provider->id();
    m_provider = provider;

    auto* controller = provider->createController(this);
    m_viewer = controller;

    // A viewer that reads the file and finds it cannot show it hands it back,
    // and the tab decides what happens next rather than each viewer inventing an
    // answer. Queued, because the decline usually arrives from inside the
    // viewer's own read -- deleting it there would be deleting the object whose
    // stack frame is still running. See ADR-0078.
    if (controller) {
        connect(
            controller, &PreviewController::declined, this,
            [this, provider](const QString& reason) { stepDownFrom(provider, reason); },
            Qt::QueuedConnection);
    }

    // What this viewer offers for this file, and what was chosen last time. Applied
    // before load(), so the file is read once and shown the way it was asked for
    // rather than shown one way and then the other.
    m_viewerOptions.clear();
    const QList<ViewerOption> declared = provider->options(m_showing);
    for (const ViewerOption& option : declared) {
        const std::optional<QString> stored = storedChoice(option, m_showing);
        // Only an answer somebody gave is applied. The default is the viewer's
        // own and it is already holding it, so pushing it in says nothing -- and
        // saying nothing is what lets a viewer tell an unanswered question from
        // one answered the same way the default would have been. See
        // storedChoice() for the case that needs the difference.
        if (controller && stored)
            controller->setViewerOption(option.key, *stored);

        // The strip shows the default where nothing is stored, because that is
        // what the file is about to be shown as.
        m_viewerOptions.append(QVariantMap { { QStringLiteral("key"), option.key },
            { QStringLiteral("title"), option.title }, { QStringLiteral("choices"), option.choices },
            { QStringLiteral("chosen"), stored.value_or(option.defaultChoice) } });
    }

    if (controller)
        controller->load(m_showing);

    // One switch for every preview rather than one per file type: see
    // setDetailsOpen(). Read here rather than once in the constructor because a
    // tab restored from a session builds its viewer before anybody looks at it.
    if (m_services.preferences) {
        m_detailsOpen = m_services.preferences->value(kDetailsOpenKey, false).toBool();
        m_detailsWidth = m_services.preferences->value(kDetailsWidthKey, 320).toInt();
    }
    emit detailsChanged();
    if (m_detailsOpen)
        readDetails();

    setSubtitle(folderPath());
    emit currentChanged();
}

void PreviewTabController::stepDownFrom(IPreviewProvider* declining, const QString& reason)
{
    // The file has moved on, or a second decline arrived from a viewer that has
    // already been replaced. Either way this is about a file nobody is looking at.
    if (!declining || m_provider != declining)
        return;

    IPreviewProvider* next
        = m_services.previews ? m_services.previews->providerBelow(m_showing, declining) : nullptr;

    const QString gaveUp = declining->displayName();
    const QString said = reason.trimmed();

    if (!next) {
        // The bottom of the ladder, which in this build is the list of facts and
        // accepts everything -- so getting here means somebody has registered a
        // viewer below it, or removed it. The reason stays on screen either way:
        // it is what the reader has instead of the file.
        m_fallbackNote = said.isEmpty()
            ? QStringLiteral("%1 could not show this file, and nothing else can").arg(gaveUp)
            : QStringLiteral("%1 could not show this file: %2").arg(gaveUp, said);
        emit currentChanged();
        return;
    }

    // The old viewer goes before the new one arrives, as it does when the file
    // changes: a media player that has a pipeline open must not keep it while its
    // replacement loads.
    if (m_viewer) {
        m_viewer->deleteLater();
        m_viewer.clear();
    }

    installViewer(next);

    // Set after installViewer, which clears nothing but does emit -- and the note
    // is about the step, so it has to outlive the install that carried it out.
    m_fallbackNote = said.isEmpty() ? QStringLiteral("%1 could not show this file").arg(gaveUp)
                                    : QStringLiteral("%1 could not show this file: %2").arg(gaveUp, said);
    emit currentChanged();
}

void PreviewTabController::setDetailsOpen(bool open)
{
    if (m_services.preferences)
        m_services.preferences->setValue(kDetailsOpenKey, open);

    if (m_detailsOpen == open)
        return;
    m_detailsOpen = open;

    if (!open && m_details) {
        m_details->requestCancel();
        m_details.clear();
    }
    if (open)
        readDetails();
    emit detailsChanged();
}

void PreviewTabController::setDetailsWidth(int width)
{
    if (width <= 0 || width == m_detailsWidth)
        return;
    m_detailsWidth = width;
    if (m_services.preferences)
        m_services.preferences->setValue(kDetailsWidthKey, width);
}

void PreviewTabController::copyDetails()
{
    QStringList lines;
    lines.reserve(m_detailFacts.size());
    for (const QVariant& entry : std::as_const(m_detailFacts)) {
        const QVariantMap fact = entry.toMap();
        lines.append(QStringLiteral("%1: %2").arg(
            fact.value(QStringLiteral("label")).toString(), fact.value(QStringLiteral("value")).toString()));
    }
    if (!lines.isEmpty())
        QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

void PreviewTabController::requestDetails()
{
    if (m_details || !m_detailFacts.isEmpty())
        return;
    readDetails();
}

void PreviewTabController::readDetails()
{
    if (m_details) {
        m_details->requestCancel();
        m_details.clear();
    }
    if (!m_services.isValid() || !m_services.metadata || !m_showing.uri.isValid() || m_showing.isDir)
        return;

    const QList<IMetadataReader*> readers = m_services.metadata->readersFor(m_showing);
    if (readers.isEmpty()) {
        m_detailFacts.clear();
        emit detailsChanged();
        return;
    }

    auto* task = new ReadMetadataTask(
        m_services.vfs->resolve(m_showing.uri), m_showing, m_head, readers, m_services);
    m_details = task;

    connect(task, &Task::finished, this, [this, task, uri = m_showing.uri] {
        if (m_details != task)
            return;
        m_details.clear();
        if (m_showing.uri != uri)
            return;

        m_detailFacts.clear();
        if (task->state() == Task::State::Succeeded) {
            const QList<FileFact> facts = task->facts();
            for (const FileFact& fact : facts) {
                m_detailFacts.append(QVariantMap {
                    { QStringLiteral("label"), fact.label }, { QStringLiteral("value"), fact.value } });
            }
        }
        emit detailsChanged();
    });

    m_services.tasks->submit(task);
    emit detailsChanged();
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
                    // A listing says nothing about what is inside a file, so it
                    // must not undo what the content pass found out -- and that
                    // was recorded against whatever is on screen.
                    const bool wrapped = m_showing.uri != m_current.uri;
                    const QString identified = wrapped ? m_current.mimeType : m_showing.mimeType;
                    m_current = entry;
                    if (m_current.mimeType.isEmpty())
                        m_current.mimeType = identified;
                    // The member of a wrapper is not in this listing -- only the
                    // wrapper is -- so it keeps everything it already knows.
                    if (!wrapped)
                        m_showing = m_current;
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

QStringList PreviewTabController::openLocations() const
{
    // The file in the folder rather than the member of a wrapper: what holds a
    // drive open is where the file lives, and a substituted member lives inside
    // it. See singleCompressedMember().
    if (!m_current.uri.isValid())
        return {};
    return { m_current.uri.toString() };
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

std::optional<QString> PreviewTabController::storedChoice(
    const ViewerOption& option, const FileEntry& entry) const
{
    if (!m_services.preferences)
        return std::nullopt;

    const QVariant remembered = m_services.preferences->value(preferenceKey(option.key, entry));
    if (!remembered.isValid())
        return std::nullopt;

    // A choice that is no longer offered is not obeyed, and it is not treated as
    // an answer either: the viewer's options can change between versions, and a
    // value nothing offers any more says nothing about what the reader wants.
    const QString value = remembered.toString();
    return option.choices.contains(value) ? std::optional<QString>(value) : std::nullopt;
}

void PreviewTabController::chooseViewerOption(const QString& key, const QString& value)
{
    if (m_services.preferences)
        m_services.preferences->setValue(preferenceKey(key, m_showing), value);

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
