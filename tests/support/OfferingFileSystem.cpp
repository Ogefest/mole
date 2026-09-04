#include "support/OfferingFileSystem.h"

#include <QBuffer>
#include <QDateTime>
#include <QThread>

#include <utility>

namespace mole::test {

OfferingFileSystem::OfferingFileSystem(std::shared_ptr<MemoryFileSystem> inner)
    : m_inner(inner ? std::move(inner) : std::make_shared<MemoryFileSystem>())
{
}

void OfferingFileSystem::addVersion(const QString& path, const QString& token, const QByteArray& contents)
{
    QMutexLocker lock(&m_mutex);
    m_versions[path].append({ token, contents });
}

void OfferingFileSystem::setLinkable(const QString& path, bool linkable)
{
    QMutexLocker lock(&m_mutex);
    m_linkable[path] = linkable;
}

int OfferingFileSystem::actionsForCallCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_actionsForCalls;
}

int OfferingFileSystem::invokeCallCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_invokeCalls;
}

QStringList OfferingFileSystem::versionsOf(const QString& path) const
{
    QMutexLocker lock(&m_mutex);
    QStringList tokens;
    for (const auto& version : m_versions.value(path))
        tokens.append(version.first);
    return tokens;
}

bool OfferingFileSystem::waitOut(const CancelToken& cancel)
{
    const int delay = m_actionDelayMs;
    if (delay <= 0)
        return !cancel.isCancelled();

    m_working.store(true);
    // Chunked, so a cancelled call does not have to wait out the whole of it.
    for (int slept = 0; slept < delay && !cancel.isCancelled(); slept += 10)
        QThread::msleep(10);
    m_working.store(false);
    return !cancel.isCancelled();
}

// ---- the drive underneath -------------------------------------------------

QString OfferingFileSystem::scheme() const
{
    return m_inner->scheme();
}

VfsCapabilities OfferingFileSystem::capabilities() const
{
    return m_inner->capabilities();
}

Qt::CaseSensitivity OfferingFileSystem::pathCaseSensitivity() const
{
    return m_inner->pathCaseSensitivity();
}

NameRules OfferingFileSystem::nameRules() const
{
    return m_inner->nameRules();
}

Result<FileEntryList> OfferingFileSystem::list(const VfsUri& dir, const CancelToken& cancel)
{
    return m_inner->list(dir, cancel);
}

Result<FileEntry> OfferingFileSystem::stat(const VfsUri& target)
{
    if (!target.hasVersion())
        return m_inner->stat(target);

    QMutexLocker lock(&m_mutex);
    for (const auto& version : m_versions.value(target.path())) {
        if (version.first != target.version())
            continue;
        FileEntry entry;
        entry.name = target.fileName();
        entry.uri = target;
        entry.size = version.second.size();
        entry.modified = QDateTime::fromSecsSinceEpoch(1'700'000'000);
        return entry;
    }
    return VfsError::make(
        VfsError::NotFound, QStringLiteral("no such version of %1: %2").arg(target.path(), target.version()));
}

Result<void> OfferingFileSystem::makeDirectory(const VfsUri& target)
{
    return m_inner->makeDirectory(target);
}

Result<QString> OfferingFileSystem::readLink(const VfsUri& link)
{
    return m_inner->readLink(link);
}

Result<void> OfferingFileSystem::makeLink(const VfsUri& link, const QString& target)
{
    return m_inner->makeLink(link, target);
}

Result<void> OfferingFileSystem::remove(const VfsUri& target, bool recursive, const CancelToken& cancel)
{
    return m_inner->remove(target, recursive, cancel);
}

Result<void> OfferingFileSystem::rename(const VfsUri& from, const VfsUri& to, const CancelToken& cancel)
{
    return m_inner->rename(from, to, cancel);
}

Result<void> OfferingFileSystem::replace(const VfsUri& from, const VfsUri& to, const CancelToken& cancel)
{
    return m_inner->replace(from, to, cancel);
}

Result<std::unique_ptr<QIODevice>> OfferingFileSystem::openRead(
    const VfsUri& target, qint64 expectedSize, const CancelToken& cancel)
{
    if (!target.hasVersion())
        return m_inner->openRead(target, expectedSize, cancel);

    QMutexLocker lock(&m_mutex);
    for (const auto& version : m_versions.value(target.path())) {
        if (version.first != target.version())
            continue;
        auto buffer = std::make_unique<QBuffer>();
        buffer->setData(version.second);
        buffer->open(QIODevice::ReadOnly);
        return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(std::move(buffer)));
    }
    return VfsError::make(
        VfsError::NotFound, QStringLiteral("no such version of %1: %2").arg(target.path(), target.version()));
}

Result<std::unique_ptr<QIODevice>> OfferingFileSystem::openWrite(
    const VfsUri& target, qint64 expectedSize, const CancelToken& cancel)
{
    // An earlier state is read-only, which is what an earlier state is.
    if (target.hasVersion())
        return VfsError::make(VfsError::NotSupported, QStringLiteral("an earlier version cannot be written"));
    return m_inner->openWrite(target, expectedSize, cancel);
}

// ---- what this drive contributes -------------------------------------------

FileActionList OfferingFileSystem::actionsFor(const VfsUri& target, const FileEntry& entry)
{
    {
        QMutexLocker lock(&m_mutex);
        ++m_actionsForCalls;
    }

    // Nothing for a directory and nothing for an earlier state of a file: what
    // is on offer is about the file as it is.
    if (entry.isDir || target.hasVersion())
        return {};

    FileActionList actions;
    {
        QMutexLocker lock(&m_mutex);
        if (m_linkable.value(target.path(), true)) {
            actions.append(FileAction {
                linkAction(), QStringLiteral("Copy a temporary link"), true, FileActionKind::Text });
        }
    }
    if (!versionsOf(target.path()).isEmpty())
        actions.append(
            FileAction { versionsAction(), QStringLiteral("Earlier versions"), true, FileActionKind::Uris });
    return actions;
}

Result<FileActionOutcome> OfferingFileSystem::invoke(
    const QString& id, const VfsUri& target, const CancelToken& cancel)
{
    {
        QMutexLocker lock(&m_mutex);
        ++m_invokeCalls;
    }
    if (!waitOut(cancel))
        return VfsError::make(VfsError::Cancelled, QStringLiteral("the action was cancelled"));
    if (m_actionFault != VfsError::None)
        return VfsError::make(m_actionFault, QStringLiteral("the far end would not answer"));

    if (id == linkAction()) {
        return FileActionOutcome::fromText(QStringLiteral("https://example.invalid/") + target.fileName(),
            QDateTime::fromSecsSinceEpoch(1'700'003'600));
    }

    if (id == versionsAction()) {
        QList<VfsUri> uris;
        for (const QString& token : versionsOf(target.path()))
            uris.append(target.withVersion(token));
        if (uris.isEmpty()) {
            return VfsError::make(
                VfsError::NotFound, QStringLiteral("nothing earlier is kept for %1").arg(target.path()));
        }
        return FileActionOutcome::fromUris(uris);
    }

    return IFileSystem::invoke(id, target, cancel);
}

int OfferingFileSystem::folderQueryCallCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_folderQueries;
}

Result<QStringList> OfferingFileSystem::entriesWithActions(const VfsUri& dir, const CancelToken& cancel)
{
    {
        QMutexLocker lock(&m_mutex);
        ++m_folderQueries;
    }
    if (!waitOut(cancel))
        return VfsError::make(VfsError::Cancelled, QStringLiteral("the folder query was cancelled"));

    const Result<FileEntryList> listing = m_inner->list(dir, cancel);
    if (!listing.ok())
        return listing.error();

    // One pass over what is already in hand, which is the shape both real
    // sources have: a snapshot directory is listed once, and a container answers
    // one paginated call over the prefix.
    QMutexLocker lock(&m_mutex);
    QStringList named;
    for (const FileEntry& entry : listing.value()) {
        if (entry.isDir)
            continue;
        const QString path = entry.uri.path();
        if (!m_versions.value(path).isEmpty() || m_linkable.value(path, true))
            named.append(entry.name);
    }
    return named;
}

Result<QStringList> OfferingFileSystem::askWhatIsOffered(const VfsUri&, const CancelToken& cancel)
{
    if (!waitOut(cancel))
        return VfsError::make(VfsError::Cancelled, QStringLiteral("the probe was cancelled"));
    return QStringList { linkAction(), versionsAction() };
}

} // namespace mole::test
