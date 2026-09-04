#include "core/tasks/DriveCheckTask.h"

#include <QLocale>

namespace mole {

DriveCheckTask::DriveCheckTask(
    const QString& driveName, FileSystemPtr fileSystem, VfsUri root, QObject* parent)
    : Task(QStringLiteral("Checking %1").arg(driveName), parent)
    , m_driveName(driveName)
    , m_fileSystem(std::move(fileSystem))
    , m_root(std::move(root))
{
    noteRunsOn(m_fileSystem);
}

void DriveCheckTask::run()
{
    setStatusText(QStringLiteral("Connecting to %1").arg(m_driveName));
    setProgress(-1);

    if (!m_fileSystem) {
        const VfsError error
            = VfsError::make(VfsError::Unknown, QStringLiteral("There is nothing configured to connect to"));
        fail(error);
        emit checked(false, error.message);
        return;
    }

    // Listing the root is the cheapest request that proves the whole chain: name
    // resolution, TLS, credentials and the path all have to be right before it
    // can answer.
    const Result<FileEntryList> listing = m_fileSystem->list(m_root, cancelToken());

    if (isCancelRequested()) {
        setStatusText(QStringLiteral("Cancelled"));
        emit checked(false, QStringLiteral("The check was cancelled"));
        return;
    }

    if (!listing.ok()) {
        fail(listing.error());
        setStatusText(listing.error().message);
        emit checked(false, listing.error().message);
        return;
    }

    const int count = static_cast<int>(listing.value().size());
    reportCount(QStringLiteral("entries"), QStringLiteral("Found"), count);

    // Said as a count rather than as "connected", because an empty answer from
    // the wrong place looks the same as an empty answer from the right one, and
    // the number is what lets someone tell them apart.
    const QLocale locale;
    const QString message = count == 0
        ? QStringLiteral("Connected. The root is empty.")
        : QStringLiteral("Connected. %1 %2 in the root.")
              .arg(locale.toString(count), count == 1 ? QStringLiteral("entry") : QStringLiteral("entries"));

    setProgress(100);
    setStatusText(message);
    emit checked(true, message);
}

} // namespace mole
