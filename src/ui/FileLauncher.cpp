#include "ui/FileLauncher.h"

#include "core/platform/Staging.h"
#include "core/tasks/ReadFileTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/NameRules.h"
#include "core/vfs/VfsManager.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

namespace mole {

FileLauncher::FileLauncher(PluginServices services, QObject* parent)
    : QObject(parent)
    , m_services(services)
    , m_openHook(
          [](const QString& localPath) { return QDesktopServices::openUrl(QUrl::fromLocalFile(localPath)); })
{
}

FileLauncher::~FileLauncher() = default;

void FileLauncher::setOpenHook(OpenHook hook)
{
    m_openHook = hook ? std::move(hook) : OpenHook();
}

QString FileLauncher::scratchDirectory() const
{
    return m_scratch && m_scratch->isValid() ? m_scratch->path() : QString();
}

QString FileLauncher::scratchPathFor(const VfsUri& uri)
{
    if (!m_scratch)
        m_scratch = staging::makeDirectory();
    if (!m_scratch)
        return {};

    // Built from components rather than from a path string, and this is the
    // whole of the fix. Taking the leading slash off uri.path() left
    // "C:/Users/ann/notes.txt", which QFileInfo calls absolute on Windows -- so
    // QDir::filePath() handed it straight back and the scratch directory was
    // never involved. At best the staging copy went somewhere nobody expects; at
    // worst a download from a remote drive was written over a local file.
    //
    // Keep the original name so the desktop picks the handler by extension, and
    // keep the parent path as a subdirectory so two files called "readme.txt"
    // from different folders do not collide. The authority joins them, because
    // the same path on two servers is two files.
    const NameRules rules = NameRules::forPlatform();

    QStringList parts;
    const auto append = [&](const QString& segment) {
        const NameVerdict verdict = checkName(segment, rules);
        if (!verdict.isRejected()) {
            parts.append(segment);
            return;
        }
        // A remote path may hold a character the local disk will not store, and
        // this function used to paste remote segments into a local path with no
        // check. Here a name nobody chose is the right answer rather than the
        // wrong one: nobody reads the staging path, and the alternative is not
        // opening the file at all.
        if (!verdict.suggestion.isEmpty())
            parts.append(verdict.suggestion);
    };

    if (!uri.authority().isEmpty())
        append(uri.authority());
    for (const QString& segment : uri.path().split(QLatin1Char('/'), Qt::SkipEmptyParts))
        append(segment);
    if (parts.isEmpty())
        return {};

    const QDir root(m_scratch->path());
    const QString target = QDir::cleanPath(root.filePath(parts.join(QLatin1Char('/'))));

    // Asserted rather than assumed, because the cost of being wrong here is
    // writing over somebody's file. Nothing above should be able to produce a
    // path outside the scratch directory; if it ever does, nothing is opened.
    const QString inside = QDir::cleanPath(root.absolutePath()) + QLatin1Char('/');
    if (!target.startsWith(inside))
        return {};

    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return {};
    return target;
}

void FileLauncher::launch(const QString& localPath, const VfsUri& origin)
{
    if (!m_openHook) {
        emit failed(origin.toString(), QStringLiteral("No handler is configured"));
        return;
    }
    if (!m_openHook(localPath)) {
        emit failed(
            origin.toString(), QStringLiteral("The desktop has no application registered for this file"));
        return;
    }
    emit opened(localPath);
}

void FileLauncher::open(const VfsUri& uri)
{
    if (!uri.isValid()) {
        emit failed(uri.toString(), QStringLiteral("Not a valid location"));
        return;
    }

    const QString localPath = uri.toLocalPath();
    if (!localPath.isEmpty()) {
        launch(localPath, uri);
        return;
    }

    if (!m_services.isValid()) {
        emit failed(uri.toString(), QStringLiteral("Application services are not available"));
        return;
    }

    FileSystemPtr fs = m_services.vfs->resolve(uri);
    if (!fs) {
        emit failed(uri.toString(), QStringLiteral("No drive is mounted for this file"));
        return;
    }

    const QString scratch = scratchPathFor(uri);
    if (scratch.isEmpty()) {
        emit failed(uri.toString(), QStringLiteral("Cannot create a scratch directory"));
        return;
    }

    // Reading happens on a worker thread like everything else, so opening a
    // 200 MB file out of a zip does not stall the window -- and so does writing
    // the copy, which used to happen here in the finished handler, on the thread
    // that draws, with the result unchecked. A scratch directory on a full /tmp
    // handed the program a truncated file under the right name. See MOLE-406.
    auto* task = new ReadFileTask(std::move(fs), uri);
    task->landAt(scratch);
    connect(task, &Task::finished, this, [this, task, uri, scratch] {
        if (task->state() != Task::State::Succeeded) {
            emit failed(uri.toString(), task->error().message);
            return;
        }
        launch(scratch, uri);
    });

    m_services.tasks->submit(task);
}

} // namespace mole
