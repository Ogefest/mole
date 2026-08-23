#include "core/platform/Staging.h"

#include <QDir>
#include <QFileInfo>

namespace mole::staging {

namespace {

    /// The name every staged file and directory starts with, so anything left
    /// behind by a crash says whose it was.
    constexpr auto kPrefix = "mole-staging-";

    /// Is the directory one a payload can be put in? The message is the useful
    /// half: "there is no temporary directory" and "the temporary directory is a
    /// file" are different faults on somebody's machine.
    bool usable(const QString& path, QString* why)
    {
        if (path.isEmpty()) {
            if (why) {
                *why = QStringLiteral("no temporary directory is set, so there is nowhere to stage this");
            }
            return false;
        }
        const QFileInfo info(path);
        if (!info.exists()) {
            if (why)
                *why = QStringLiteral("the temporary directory %1 is not there").arg(path);
            return false;
        }
        if (!info.isDir()) {
            if (why)
                *why = QStringLiteral("%1 is not a directory, so nothing can be staged in it").arg(path);
            return false;
        }
        if (!info.isWritable()) {
            if (why)
                *why = QStringLiteral("the temporary directory %1 cannot be written to").arg(path);
            return false;
        }
        return true;
    }

} // namespace

QString directory()
{
    // Read on every call rather than cached: a test moves it, and a long-running
    // window is exactly where a cached answer would be wrong.
    const QString configured = qEnvironmentVariable("MOLE_STAGING_DIR");
    return configured.isEmpty() ? QDir::tempPath() : configured;
}

bool openFile(QTemporaryFile& file, QString* why)
{
    const QString where = directory();
    if (!usable(where, why))
        return false;

    // The template, spelled out. Left to Qt, a temporary file goes wherever
    // QDir::tempPath() points *at the moment it opens* -- which is the whole
    // fault this exists for.
    file.setFileTemplate(QDir(where).filePath(QLatin1String(kPrefix) + QStringLiteral("XXXXXX")));
    if (file.open())
        return true;
    if (why) {
        *why = QStringLiteral("could not open a file to stage this in %1: %2").arg(where, file.errorString());
    }
    return false;
}

std::unique_ptr<QTemporaryDir> makeDirectory(QString* why)
{
    const QString where = directory();
    if (!usable(where, why))
        return nullptr;

    auto directory = std::make_unique<QTemporaryDir>(
        QDir(where).filePath(QLatin1String(kPrefix) + QStringLiteral("XXXXXX")));
    if (directory->isValid())
        return directory;
    if (why) {
        *why = QStringLiteral("could not make a scratch directory in %1: %2")
                   .arg(where, directory->errorString());
    }
    return nullptr;
}

} // namespace mole::staging
