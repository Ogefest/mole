#include "core/diagnostics/LoggingFileSystem.h"

#include "core/diagnostics/Diagnostics.h"

#include <QElapsedTimer>

#include <utility>

namespace mole {
namespace {

    /// Runs one backend call, and writes what it did when anyone is listening.
    ///
    /// A template rather than ten near-identical methods, and it returns the
    /// backend's own result untouched: a wrapper that alters an answer while
    /// claiming to observe it would be worse than no wrapper at all.
    template<typename Call>
    auto watch(const QString& drive, const char* operation, const QString& subject, Call&& call)
    {
        if (!driveLog().isDebugEnabled())
            return call();

        QElapsedTimer clock;
        clock.start();
        auto result = call();
        const qint64 elapsed = clock.elapsed();

        if (result.ok()) {
            qCDebug(driveLog, "[%s] %s %s: ok in %lld ms", qPrintable(drive), operation, qPrintable(subject),
                elapsed);
        } else {
            qCDebug(driveLog, "[%s] %s %s: %s in %lld ms", qPrintable(drive), operation, qPrintable(subject),
                qPrintable(result.error().message), elapsed);
        }
        return result;
    }

} // namespace

LoggingFileSystem::LoggingFileSystem(FileSystemPtr inner, QString name)
    : m_inner(std::move(inner))
    , m_name(std::move(name))
{
}

// Neither of these does any work or can fail, and both are called constantly by
// the interface. A line for each would drown everything worth reading.
QString LoggingFileSystem::scheme() const
{
    return m_inner->scheme();
}
VfsCapabilities LoggingFileSystem::capabilities() const
{
    return m_inner->capabilities();
}

// Nor do these three, and each one is an answer only the volume underneath can
// give. Forwarded silently for the same reason as the two above.
Qt::CaseSensitivity LoggingFileSystem::pathCaseSensitivity() const
{
    return m_inner->pathCaseSensitivity();
}
NameRules LoggingFileSystem::nameRules() const
{
    return m_inner->nameRules();
}
bool LoggingFileSystem::understandsVersions() const
{
    return m_inner->understandsVersions();
}

Result<FileEntryList> LoggingFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    if (!driveLog().isDebugEnabled())
        return m_inner->list(dir, cancel);

    QElapsedTimer clock;
    clock.start();
    Result<FileEntryList> result = m_inner->list(dir, cancel);
    const qint64 elapsed = clock.elapsed();

    // Listings say how many, not just whether: "ok in 4 s" and "ok in 4 s, 90000
    // entries" are different diagnoses.
    if (result.ok()) {
        qCDebug(driveLog, "[%s] list %s: %lld entries in %lld ms", qPrintable(m_name),
            qPrintable(dir.toString()), static_cast<long long>(result.value().size()), elapsed);
    } else {
        qCDebug(driveLog, "[%s] list %s: %s in %lld ms", qPrintable(m_name), qPrintable(dir.toString()),
            qPrintable(result.error().message), elapsed);
    }
    return result;
}

Result<FileEntry> LoggingFileSystem::stat(const VfsUri& target)
{
    return watch(m_name, "stat", target.toString(), [&] { return m_inner->stat(target); });
}

Result<void> LoggingFileSystem::makeDirectory(const VfsUri& target)
{
    return watch(m_name, "mkdir", target.toString(), [&] { return m_inner->makeDirectory(target); });
}

Result<QString> LoggingFileSystem::readLink(const VfsUri& link)
{
    return watch(m_name, "readlink", link.toString(), [&] { return m_inner->readLink(link); });
}

Result<void> LoggingFileSystem::makeLink(const VfsUri& link, const QString& target)
{
    const QString subject = link.toString() + QStringLiteral(" -> ") + target;
    return watch(m_name, "link", subject, [&] { return m_inner->makeLink(link, target); });
}

Result<void> LoggingFileSystem::remove(const VfsUri& target, bool recursive)
{
    return watch(m_name, "remove", target.toString(), [&] { return m_inner->remove(target, recursive); });
}

Result<void> LoggingFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    const QString subject = from.toString() + QStringLiteral(" -> ") + to.toString();
    return watch(m_name, "rename", subject, [&] { return m_inner->rename(from, to); });
}

Result<void> LoggingFileSystem::replace(const VfsUri& from, const VfsUri& to)
{
    const QString subject = from.toString() + QStringLiteral(" -> ") + to.toString();
    return watch(m_name, "replace", subject, [&] { return m_inner->replace(from, to); });
}

Result<std::unique_ptr<QIODevice>> LoggingFileSystem::openRead(const VfsUri& target, qint64 expectedSize)
{
    return watch(m_name, "read", target.toString(), [&] { return m_inner->openRead(target, expectedSize); });
}

Result<std::unique_ptr<QIODevice>> LoggingFileSystem::openWrite(const VfsUri& target, qint64 expectedSize)
{
    return watch(
        m_name, "write", target.toString(), [&] { return m_inner->openWrite(target, expectedSize); });
}

Result<SpaceInfo> LoggingFileSystem::space(const VfsUri& target)
{
    return watch(m_name, "space", target.toString(), [&] { return m_inner->space(target); });
}

Result<AccessInfo> LoggingFileSystem::access(const VfsUri& target)
{
    return watch(m_name, "access", target.toString(), [&] { return m_inner->access(target); });
}

Result<QList<DriveLeftover>> LoggingFileSystem::leftovers(
    std::chrono::seconds olderThan, const CancelToken& cancel)
{
    if (!driveLog().isDebugEnabled())
        return m_inner->leftovers(olderThan, cancel);

    QElapsedTimer clock;
    clock.start();
    Result<QList<DriveLeftover>> result = m_inner->leftovers(olderThan, cancel);
    const qint64 elapsed = clock.elapsed();

    // How many, like a listing: the whole question a sweep answers is whether
    // there is anything there, and "ok" alone does not say.
    if (result.ok()) {
        qCDebug(driveLog, "[%s] leftovers older than %lld s: %lld found in %lld ms", qPrintable(m_name),
            static_cast<long long>(olderThan.count()), static_cast<long long>(result.value().size()),
            elapsed);
    } else {
        qCDebug(driveLog, "[%s] leftovers older than %lld s: %s in %lld ms", qPrintable(m_name),
            static_cast<long long>(olderThan.count()), qPrintable(result.error().message), elapsed);
    }
    return result;
}

Result<void> LoggingFileSystem::discardLeftover(const DriveLeftover& leftover)
{
    // The handle, not the path: it is what went to the drive, and a sweep that
    // discarded the wrong thing is diagnosed from what was actually sent.
    return watch(m_name, "discard", leftover.handle, [&] { return m_inner->discardLeftover(leftover); });
}

Result<FileEntryList> LoggingFileSystem::search(
    const VfsUri& root, const QString& pattern, const CancelToken& cancel)
{
    const QString subject = root.toString() + QStringLiteral(" for \"") + pattern + QLatin1Char('"');
    return watch(m_name, "search", subject, [&] { return m_inner->search(root, pattern, cancel); });
}

FileActionList LoggingFileSystem::actionsFor(const VfsUri& target, const FileEntry& entry)
{
    if (!driveLog().isDebugEnabled())
        return m_inner->actionsFor(target, entry);

    QElapsedTimer clock;
    clock.start();
    FileActionList actions = m_inner->actionsFor(target, entry);
    const qint64 elapsed = clock.elapsed();

    // How many, like a listing, and for the same reason: a drive that asks the
    // far end what it can offer is a drive that can be slow to answer nothing.
    qCDebug(driveLog, "[%s] actions %s: %lld offered in %lld ms", qPrintable(m_name),
        qPrintable(target.toString()), static_cast<long long>(actions.size()), elapsed);
    return actions;
}

Result<FileActionOutcome> LoggingFileSystem::invoke(
    const QString& id, const VfsUri& target, const CancelToken& cancel)
{
    const QString subject = id + QStringLiteral(" on ") + target.toString();
    return watch(m_name, "action", subject, [&] { return m_inner->invoke(id, target, cancel); });
}

Result<QStringList> LoggingFileSystem::entriesWithActions(const VfsUri& dir, const CancelToken& cancel)
{
    return watch(
        m_name, "offered in", dir.toString(), [&] { return m_inner->entriesWithActions(dir, cancel); });
}

DriveOffers LoggingFileSystem::offers() const
{
    return m_inner->offers();
}

void LoggingFileSystem::probe(const VfsUri& target, const CancelToken& cancel)
{
    if (!driveLog().isDebugEnabled()) {
        m_inner->probe(target, cancel);
        return;
    }

    QElapsedTimer clock;
    clock.start();
    m_inner->probe(target, cancel);
    const qint64 elapsed = clock.elapsed();

    // What the drive says it can offer, and how long it took to find out. A
    // probe that answers nothing and one that was never made look identical
    // afterwards, and this is the only place that can tell them apart.
    const DriveOffers found = m_inner->offers();
    qCDebug(driveLog, "[%s] probe %s: %s in %lld ms", qPrintable(m_name), qPrintable(target.toString()),
        qPrintable(found.isKnown() ? (found.ids.isEmpty() ? QStringLiteral("nothing offered")
                                                          : found.ids.join(QStringLiteral(", ")))
                                   : QStringLiteral("no answer")),
        elapsed);
}

FileSystemPtr withLogging(FileSystemPtr fileSystem, const QString& name)
{
    if (!fileSystem || std::dynamic_pointer_cast<LoggingFileSystem>(fileSystem))
        return fileSystem;
    return std::make_shared<LoggingFileSystem>(std::move(fileSystem), name);
}

} // namespace mole
