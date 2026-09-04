#include "plugins/builtin/PreviewFeature.h"

#include "plugins/builtin/previews/MetadataReaders.h"
#include "sdk/IMetadataReader.h"

#include "core/data/FileType.h"
#include "core/events/EventBus.h"
#include "core/settings/Preferences.h"
#include "core/tasks/InvokeFileActionTask.h"
#include "core/tasks/ListDirectoryTask.h"
#include "core/tasks/QueryFileActionsTask.h"
#include "core/tasks/ReadRangeTask.h"
#include "core/tasks/StatTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QClipboard>
#include <QFileInfo>
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
    // **The neighbours follow the folder.** ARCHITECTURE.md says this tab owns
    // two things: which file is current, and the list of its neighbours so the
    // arrows can step through the folder. The list was built once, on open(),
    // and nothing subscribed to this -- so a file deleted, renamed or added
    // while the preview was open left the arrows stepping through a list that no
    // longer matched: a deleted neighbour gave a read error, a new one was
    // unreachable, and "3 of 17" was stale. See MOLE-384.
    if (m_services.events) {
        connect(m_services.events, &EventBus::directoryChanged, this, [this](const VfsUri& directory) {
            if (!m_current.uri.isValid() || directory != m_current.uri.parent())
                return;
            loadSiblings(directory, m_current.uri);
        });
    }
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

    // **What the tab was asked to show, recorded before anything can fail.**
    //
    // open() returned below on an unmounted drive with m_current still empty, so
    // saveState() wrote {"uri": ""} -- and a session saved after a restore with
    // an S3 or SFTP drive not yet connected lost the file for good, even though
    // the drive comes back later. It shows up on the *second* restart, by which
    // time nothing points at the cause. The browser pane's lost location was the
    // same shape.
    //
    // Kept beside m_current rather than in it, deliberately: currentUri() means
    // "the file this tab has settled on", and callers wait on it. Being asked
    // about a file is not the same as showing one. See MOLE-384.
    m_askedFor = target;

    FileSystemPtr fs = m_services.vfs->resolve(target);
    if (!fs) {
        setTitle(target.fileName());
        setSubtitle(QStringLiteral("No drive is mounted for this file"));
        return;
    }

    // On a task. The comment that used to be here said the entry was "usually
    // already known", which is true of a local disk and of nothing else: on a
    // share that has stopped answering, stat() blocks for as long as the mount
    // takes to give up, and this ran on the thread that draws for every F3. See
    // ARCHITECTURE.md's first rule and MOLE-360.
    //
    // The name and the uri are enough to open the tab with, so the tab opens at
    // once and fills in when the drive answers -- which is what every other slow
    // step here already does. A drive that cannot say leaves the fallback
    // standing, exactly as the direct call did.
    setTitle(target.fileName());
    askAbout(target, [this, target](const FileEntry& entry) {
        showEntry(entry);
        loadSiblings(target.parent(), target);
    });
}

void PreviewTabController::askAbout(const VfsUri& target, std::function<void(const FileEntry&)> then)
{
    FileEntry fallback;
    fallback.name = target.fileName();
    fallback.uri = target;

    FileSystemPtr fs = m_services.vfs ? m_services.vfs->resolve(target) : nullptr;
    if (!fs || !m_services.tasks) {
        then(fallback);
        return;
    }

    // Cancelled when another file is opened before this one has answered, so a
    // slow drive cannot deliver a stale entry over the file somebody has since
    // moved on to.
    if (m_statPending)
        m_statPending->requestCancel();

    auto* task = new StatTask(std::move(fs), target);
    m_statPending = task;
    connect(task, &StatTask::entryReady, this,
        [this, task, fallback, then = std::move(then)](const VfsUri&, bool found, const FileEntry& entry) {
            if (m_statPending == task)
                m_statPending.clear();
            then(found ? entry : fallback);
        });
    m_services.tasks->submit(task);
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
    //
    // Finding out costs opening the container and listing it -- libarchive, on a
    // file that may be a gzip whose length is only in its trailer, so listing it
    // reads the whole thing. That ran on the thread that draws. It happens on a
    // task now, and what follows is the rest of this function once the answer is
    // in. See MOLE-360.
    m_showing = entry;
    if (looksLikeASingleCompressedStream(entry)) {
        lookInsideThenShow(entry);
        return;
    }
    showWhatIsThere();
}

bool PreviewTabController::looksLikeASingleCompressedStream(const FileEntry& entry) const
{
    // The cheap tests, all of which are about the name and the mount rather than
    // about the bytes: everything that needs the container opened is in the task.
    return !entry.isDir && FileType::namesSingleCompressedStream(entry.name) && m_services.vfs
        && m_services.tasks && !entry.uri.toLocalPath().isEmpty();
}

void PreviewTabController::showWhatIsThere()
{
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

    // On a task, for the reason open() gives: one call about one file is still a
    // call to storage. See MOLE-360.
    askAbout(target, [this, target](const FileEntry& answered) {
        FileEntry entry = answered;
        if (!entry.uri.isValid() || entry.name.isEmpty()) {
            entry.name = m_current.name;
            entry.uri = target;
        }
        // Contents only. The tab goes on being about the file it is about, so the
        // arrows still step through the folder and the session still records the
        // file rather than a state of it that may not be there next time.
        showContents(entry);
        emit currentChanged();
    });
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

namespace {

    /// Opens a container and lists it, off the thread that draws.
    ///
    /// The work is libarchive's: a `.gz` keeps its uncompressed length in its
    /// trailer, so listing one reads the whole file. That is a task's business
    /// and not the window's -- and the mount that follows is added on the UI
    /// thread by whoever asked, because a mount table is the shell's. See
    /// MOLE-360.
    class LookInsideTask final : public Task
    {
        Q_OBJECT

    public:
        LookInsideTask(IFileSystemFactory* opener, QString localPath, QObject* parent = nullptr)
            : Task(QStringLiteral("Look inside %1").arg(QFileInfo(localPath).fileName()), parent)
            , m_opener(opener)
            , m_localPath(std::move(localPath))
        {
            setBackground(true);
        }

    signals:
        /// Emitted on the UI thread, once. `inside` is null when there is nothing
        /// to show: a container of many members, something this build cannot
        /// open, or a `.gz` that is not gzip at all. All of those keep the
        /// behaviour the file had before anybody looked.
        void looked(mole::FileSystemPtr inside, mole::VfsUri root, mole::FileEntry member);

    protected:
        void run() override
        {
            if (!m_opener) {
                emit looked(nullptr, VfsUri {}, FileEntry {});
                return;
            }

            QString error;
            FileSystemPtr inside = m_opener->create(m_opener->configForFile(m_localPath), &error);
            if (!inside) {
                emit looked(nullptr, VfsUri {}, FileEntry {});
                return;
            }

            const VfsUri root = m_opener->rootUriForFile(m_localPath);
            const Result<FileEntryList> listed = inside->list(root, cancelToken());
            // Exactly one member, and a file: anything else is a container, or a
            // `.gz` that is not gzip at all, and both keep today's behaviour.
            if (!listed.ok() || listed.value().size() != 1 || listed.value().first().isDir) {
                emit looked(nullptr, VfsUri {}, FileEntry {});
                return;
            }
            emit looked(std::move(inside), root, listed.value().first());
        }

    private:
        IFileSystemFactory* m_opener = nullptr;
        QString m_localPath;
    };

} // namespace

void PreviewTabController::lookInsideThenShow(const FileEntry& entry)
{
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
    if (!opener) {
        showWhatIsThere();
        return;
    }

    if (m_lookInside)
        m_lookInside->requestCancel();

    auto* task = new LookInsideTask(opener, entry.uri.toLocalPath());
    m_lookInside = task;
    connect(task, &LookInsideTask::looked, this,
        [this, task, entry](FileSystemPtr inside, const VfsUri& root, const FileEntry& member) {
            if (m_lookInside == task)
                m_lookInside.clear();
            // The file may have changed under this while the container was being
            // read; then the answer is about a file nobody is looking at.
            if (m_showing.uri != entry.uri)
                return;

            if (inside) {
                // A viewer reads by resolving a uri, so the wrapper has to be
                // mounted -- and it must not become a drive in the sidebar for
                // the length of a preview. Added here, on the thread that owns
                // the mount table.
                Mount mount;
                mount.id = QStringLiteral("preview-member:") + root.authority();
                mount.displayName = entry.name;
                mount.root = root;
                mount.fileSystem = std::move(inside);
                mount.internal = true;
                m_memberMountId = m_services.vfs->addMount(mount);
                if (!m_memberMountId.isEmpty()) {
                    m_memberMountOwner = m_services.vfs;
                    m_showing = member;
                }
            }
            showWhatIsThere();
        });
    m_services.tasks->submit(task);
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

            // Files only, by name.
            //
            // The comment here said "in the same order the browser shows them",
            // and that was not true: a browser sorted by date or by size steps
            // in a different order from the one the reader had just been looking
            // at. Taking the browser's order would mean the browser handing it
            // over -- there is no ordering to read off a uri -- and the tab is
            // reachable from a set, from a search result and from the command
            // palette, none of which has one. So the claim goes rather than the
            // sort: by name, always, which is at least an order a person can
            // predict. See MOLE-384.
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
    // What it settled on, or what it was asked for when it never got that far:
    // a drive that was not connected must not cost the file. See open().
    const QString uri = m_current.uri.isValid() ? currentUri() : m_askedFor.toString();
    return { { QStringLiteral("uri"), uri } };
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

// The task above declares Q_OBJECT in this file, the way IndexSummary's helper
// does: a class nothing outside this feature has any use for stays in it.
#include "PreviewFeature.moc"
