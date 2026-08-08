#include "ui/FileLauncher.h"

#include "core/tasks/ReadFileTask.h"
#include "core/tasks/TaskManager.h"
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

QString FileLauncher::scratchPathFor(const VfsUri& uri)
{
    if (!m_scratch)
        m_scratch = std::make_unique<QTemporaryDir>();
    if (!m_scratch->isValid())
        return {};

    // Keep the original name so the desktop picks the handler by extension,
    // and keep the parent path as a subdirectory so two files called
    // "readme.txt" from different folders do not collide.
    QString relative = uri.path();
    relative.remove(0, 1);
    const QString target = QDir(m_scratch->path()).filePath(relative);
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
    // 200 MB file out of a zip does not stall the window.
    auto* task = new ReadFileTask(std::move(fs), uri);
    connect(task, &Task::finished, this, [this, task, uri, scratch] {
        if (task->state() != Task::State::Succeeded) {
            emit failed(uri.toString(), task->error().message);
            return;
        }

        QFile file(scratch);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit failed(uri.toString(), QStringLiteral("Cannot write the extracted copy"));
            return;
        }
        file.write(task->contents());
        file.close();

        launch(scratch, uri);
    });

    m_services.tasks->submit(task);
}

} // namespace mole
