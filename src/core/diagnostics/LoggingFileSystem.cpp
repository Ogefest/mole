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

Result<void> LoggingFileSystem::remove(const VfsUri& target, bool recursive)
{
    return watch(m_name, "remove", target.toString(), [&] { return m_inner->remove(target, recursive); });
}

Result<void> LoggingFileSystem::rename(const VfsUri& from, const VfsUri& to)
{
    const QString subject = from.toString() + QStringLiteral(" -> ") + to.toString();
    return watch(m_name, "rename", subject, [&] { return m_inner->rename(from, to); });
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

Result<FileEntryList> LoggingFileSystem::search(
    const VfsUri& root, const QString& pattern, const CancelToken& cancel)
{
    const QString subject = root.toString() + QStringLiteral(" for \"") + pattern + QLatin1Char('"');
    return watch(m_name, "search", subject, [&] { return m_inner->search(root, pattern, cancel); });
}

FileSystemPtr withLogging(FileSystemPtr fileSystem, const QString& name)
{
    if (!fileSystem || std::dynamic_pointer_cast<LoggingFileSystem>(fileSystem))
        return fileSystem;
    return std::make_shared<LoggingFileSystem>(std::move(fileSystem), name);
}

} // namespace mole
