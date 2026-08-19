#include "core/tasks/SweepLeftoversTask.h"

#include <QLocale>

namespace mole {

SweepLeftoversTask::SweepLeftoversTask(const QString& driveName, FileSystemPtr fileSystem,
    std::chrono::seconds olderThan, bool discard, QObject* parent)
    : Task(discard ? QStringLiteral("Clearing up %1").arg(driveName)
                   : QStringLiteral("Looking over %1").arg(driveName),
          parent)
    , m_driveName(driveName)
    , m_fileSystem(std::move(fileSystem))
    , m_olderThan(olderThan)
    , m_discard(discard)
{
}

void SweepLeftoversTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("That drive is not there")));
        return;
    }
    if (!m_fileSystem->capabilities().testFlag(VfsCapability::ReportsLeftovers)) {
        // Not a failure. Most drives have nothing of this kind to leave behind,
        // and saying so plainly beats an error somebody has to interpret.
        m_summary = QStringLiteral("%1 keeps nothing back, so there is nothing to clear up").arg(m_driveName);
        setStatusText(m_summary);
        return;
    }

    setStatusText(QStringLiteral("Asking %1 what it is holding").arg(m_driveName));
    const Result<QList<DriveLeftover>> found = m_fileSystem->leftovers(m_olderThan, cancelToken());
    if (!found.ok()) {
        fail(found.error());
        return;
    }

    m_found = found.value();
    reportCount(QStringLiteral("found"), QStringLiteral("Found"), m_found.size(), 10);

    if (m_found.isEmpty()) {
        m_summary = QStringLiteral("%1 is holding nothing that was left behind").arg(m_driveName);
        setStatusText(m_summary);
        setProgress(100);
        return;
    }

    if (!m_discard) {
        // Reported and left alone. What to do about it is the reader's call:
        // these are theirs, and one of them may be a copy they are running now
        // on another machine.
        m_summary = QStringLiteral("%1 is holding %2 that %3 never finished")
                        .arg(m_driveName)
                        .arg(m_found.size())
                        .arg(m_found.size() == 1 ? QStringLiteral("an upload") : QStringLiteral("uploads"));
        setStatusText(m_summary);
        setProgress(100);
        return;
    }

    QStringList refused;
    for (const DriveLeftover& leftover : std::as_const(m_found)) {
        if (isCancelRequested())
            return;
        setStatusText(leftover.path);
        const Result<void> discarded = m_fileSystem->discardLeftover(leftover);
        if (discarded.ok())
            ++m_discarded;
        else
            refused.append(QStringLiteral("%1: %2").arg(leftover.path, discarded.error().message));
        setProgress(static_cast<int>(100.0 * (m_discarded + refused.size()) / m_found.size()));
    }

    reportCount(QStringLiteral("cleared"), QStringLiteral("Cleared"), m_discarded, 20);
    m_summary = refused.isEmpty()
        ? QStringLiteral("Cleared %1 unfinished %2 from %3")
              .arg(m_discarded)
              .arg(m_discarded == 1 ? QStringLiteral("upload") : QStringLiteral("uploads"), m_driveName)
        : QStringLiteral("Cleared %1 of %2; %3 could not be: %4")
              .arg(m_discarded)
              .arg(m_found.size())
              .arg(refused.size())
              .arg(refused.join(QStringLiteral("; ")));
    setStatusText(m_summary);
}

} // namespace mole
