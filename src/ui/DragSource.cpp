#include "ui/DragSource.h"

#include "core/platform/Staging.h"
#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QMimeData>
#include <QUrl>

namespace mole {

DragSource::DragSource(PluginServices services, QObject* parent)
    : QObject(parent)
    , m_services(services)
{
}

DragSource::~DragSource() = default;

void DragSource::setStartHook(StartHook hook)
{
    m_startHook = hook ? std::move(hook) : StartHook();
}

QString DragSource::stagedPathFor(const VfsUri& uri)
{
    if (!m_scratch)
        m_scratch = staging::makeDirectory();
    if (!m_scratch)
        return {};

    QString relative = uri.path();
    relative.remove(0, 1);
    if (relative.isEmpty())
        return {};
    return QDir(m_scratch->path()).filePath(relative);
}

bool DragSource::stagedCopyIsFresh(const VfsUri& source) const
{
    const Staged known = m_staged.value(source.toString());
    if (known.path.isEmpty())
        return false;

    const QFileInfo staged(known.path);
    if (!staged.exists())
        return false;
    if (known.size < 0)
        return staged.isDir();

    // The source, as it is now. One stat, and only on a drag that already has a
    // copy to reuse -- the first drag of a row never pays for it.
    FileSystemPtr fs = m_services.isValid() ? m_services.vfs->resolve(source) : nullptr;
    if (!fs)
        return false;
    const Result<FileEntry> now = fs->stat(source);
    if (!now.ok())
        return false;
    return now.value().size == known.size && now.value().modified == known.modified;
}

void DragSource::remember(const VfsUri& source, const QString& stagedPath)
{
    Staged staged;
    staged.path = stagedPath;

    FileSystemPtr fs = m_services.isValid() ? m_services.vfs->resolve(source) : nullptr;
    if (fs) {
        const Result<FileEntry> entry = fs->stat(source);
        // A directory keeps the -1: what it looked like is a question about a tree.
        if (entry.ok() && !entry.value().isDir) {
            staged.size = entry.value().size;
            staged.modified = entry.value().modified;
        }
    }
    m_staged.insert(source.toString(), staged);
}

void DragSource::stage(const QList<VfsUri>& rows)
{
    if (!m_services.isValid()) {
        emit refused(QStringLiteral("Application services are not available"));
        return;
    }

    // One task per folder the rows came from, because staging keeps the parent
    // path and rows from two folders have two destinations. A selection comes out
    // of one listing, so this is nearly always one task -- but "nearly always" is
    // not something to assume in a loop that writes files. Ordered, so the tasks
    // queue in the same order twice.
    QMap<QString, QList<VfsUri>> byParent;
    for (const VfsUri& row : rows)
        byParent[row.parent().toString()].append(row);

    int fetching = 0;
    QStringList problems;

    for (auto group = byParent.constBegin(); group != byParent.constEnd(); ++group) {
        const QList<VfsUri>& sources = group.value();

        const QString stagedPath = stagedPathFor(sources.first());
        if (stagedPath.isEmpty()) {
            problems.append(QStringLiteral("cannot create a scratch directory"));
            continue;
        }
        const QString targetDirectory = QFileInfo(stagedPath).absolutePath();
        if (!QDir().mkpath(targetDirectory)) {
            problems.append(QStringLiteral("cannot write to %1").arg(targetDirectory));
            continue;
        }

        FileSystemPtr fs = m_services.vfs->resolve(sources.first());
        if (!fs) {
            problems.append(QStringLiteral("no drive is mounted for %1").arg(group.key()));
            continue;
        }

        // TransferTask rather than ReadFileTask: it streams instead of holding the
        // file in memory, it expands a directory into everything underneath it, and
        // it is what weighs the arrival at the destination -- see ADR-0016. A file
        // that leaves Mole half-copied is worse than one that could not be dragged
        // at all, because nothing downstream will ever question it.
        TransferTask::Request request;
        request.sourceFileSystem = std::move(fs);
        request.targetFileSystem = std::make_shared<LocalFileSystem>();
        request.sources = sources;
        request.targetDirectory = VfsUri::fromLocalPath(targetDirectory);
        request.mode = TransferTask::Mode::Copy;
        // A copy that is here already is a stale one being replaced on purpose:
        // freshness was checked before anything was queued.
        request.onConflict = TransferTask::Conflict::Overwrite;

        auto* task = new TransferTask(request);
        connect(task, &Task::finished, this, [this, task, sources] {
            if (!task->failures().isEmpty()) {
                emit refused(task->failures().join(QLatin1String("; ")));
                return;
            }
            for (const VfsUri& source : sources)
                remember(source, stagedPathFor(source));
        });

        m_services.tasks->submit(task);
        fetching += static_cast<int>(sources.size());
    }

    if (fetching > 0)
        emit staging(fetching);
    if (!problems.isEmpty())
        emit refused(problems.join(QLatin1String("; ")));
}

void DragSource::start(const QList<VfsUri>& rows)
{
    if (rows.isEmpty()) {
        emit refused(QStringLiteral("Nothing is selected"));
        return;
    }

    QList<QUrl> urls;
    QList<VfsUri> toFetch;
    int unreachable = 0;

    for (const VfsUri& row : rows) {
        // A directory goes out as its own url. Expanding it here would hand the
        // receiver a flat list of leaves and lose the folder it was dragged as.
        const QString localPath = row.toLocalPath();
        if (!localPath.isEmpty()) {
            urls.append(QUrl::fromLocalFile(localPath));
            continue;
        }

        // Fetched by an earlier drag, and the source has not moved on since.
        if (stagedCopyIsFresh(row)) {
            urls.append(QUrl::fromLocalFile(m_staged.value(row.toString()).path));
            continue;
        }

        if (m_services.isValid() && m_services.vfs->resolve(row)) {
            toFetch.append(row);
            continue;
        }

        // No drive mounted, so there is nothing to fetch from either.
        ++unreachable;
    }

    // The fetch, and no drag at all this time. Blocking until the bytes arrive is
    // not available -- this is the UI thread, and a 2 GB read over SFTP would
    // freeze the window with no progress and no cancel -- and a drag cannot be
    // started once the button is up. So the gesture is answered with a task and a
    // sentence, and the next one carries the files.
    if (!toFetch.isEmpty()) {
        stage(toFetch);
        return;
    }

    if (urls.isEmpty()) {
        emit refused(QStringLiteral("No drive is mounted for these files, so there is nothing to "
                                    "fetch them from"));
        return;
    }

    if (!m_startHook) {
        emit refused(QStringLiteral("No handler is configured"));
        return;
    }

    auto mime = std::make_unique<QMimeData>();
    mime->setUrls(urls);

    // Copy and nothing else. A receiver that asked for a move would otherwise
    // delete the source on the strength of a gesture that looks exactly like the
    // one that copies -- and nothing ever leaves Mole by being moved.
    if (!m_startHook(std::move(mime), Qt::CopyAction)) {
        emit refused(QStringLiteral("The desktop did not take the files"));
        return;
    }

    emit started(static_cast<int>(urls.size()));
    if (unreachable > 0)
        emit leftBehind(static_cast<int>(urls.size()), unreachable);
}

} // namespace mole
