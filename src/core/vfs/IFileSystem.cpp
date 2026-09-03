#include "core/vfs/IFileSystem.h"

#include "core/diagnostics/Diagnostics.h"

#include <atomic>

namespace mole {

namespace {

    /// The thread the interface draws on, or null when nothing is guarding.
    ///
    /// A plain atomic pointer, and never dereferenced: it is compared with the
    /// current thread and nothing else, so a thread that has ended cannot be
    /// followed.
    std::atomic<QThread*> theDrawingThread { nullptr };

} // namespace

void IFileSystem::doNotCallFrom(QThread* thread)
{
    theDrawingThread.store(thread);
}

void IFileSystem::checkNotOnTheDrawingThread(const char* what)
{
    if (theDrawingThread.load() != QThread::currentThread())
        return;
    // Not qCWarning: this is a programming fault rather than an operational
    // fact, and it should be visible without anybody turning a category on --
    // the same choice IndexDatabase makes about the same kind of mistake.
    qWarning("Drive call on the thread that draws the window: %s. Ask through a task -- "
             "see ARCHITECTURE.md and MOLE-360.",
        what);
}

Result<void> IFileSystem::notSupported(const char* what)
{
    return Result<void>::failure(VfsError::NotSupported,
        QStringLiteral("Operation not supported by this backend: %1").arg(QLatin1String(what)));
}

Result<void> IFileSystem::makeDirectory(const VfsUri&)
{
    return notSupported("makeDirectory");
}

Result<void> IFileSystem::remove(const VfsUri&, bool)
{
    return notSupported("remove");
}

Result<void> IFileSystem::rename(const VfsUri&, const VfsUri&)
{
    return notSupported("rename");
}

Result<void> IFileSystem::replace(const VfsUri& from, const VfsUri& to)
{
    // Asked before anything is removed. A backend that cannot rename would
    // remove the destination and then discover it has nothing to put there,
    // which is the two-step's worst outcome reached for no gain at all.
    if (!capabilities().testFlag(VfsCapability::Rename))
        return notSupported("replace");

    const Result<void> cleared = remove(to, false);
    if (!cleared.ok() && cleared.error().code != VfsError::NotFound)
        return cleared;
    return rename(from, to);
}

Result<std::unique_ptr<QIODevice>> IFileSystem::openRead(const VfsUri&, qint64)
{
    return VfsError::make(VfsError::NotSupported, QStringLiteral("openRead not supported"));
}

Result<std::unique_ptr<QIODevice>> IFileSystem::openWrite(const VfsUri&, qint64)
{
    return VfsError::make(VfsError::NotSupported, QStringLiteral("openWrite not supported"));
}

Result<SpaceInfo> IFileSystem::space(const VfsUri&)
{
    return VfsError::make(VfsError::NotSupported, QStringLiteral("capacity is unknown here"));
}

Result<AccessInfo> IFileSystem::access(const VfsUri&)
{
    return VfsError::make(VfsError::NotSupported, QStringLiteral("access is unknown here"));
}

Result<QList<DriveLeftover>> IFileSystem::leftovers(std::chrono::seconds, const CancelToken&)
{
    return VfsError::make(VfsError::NotSupported, QStringLiteral("this drive does not keep anything back"));
}

Result<void> IFileSystem::discardLeftover(const DriveLeftover&)
{
    return VfsError::make(VfsError::NotSupported, QStringLiteral("this drive does not keep anything back"));
}

Result<FileEntryList> IFileSystem::search(const VfsUri&, const QString&, const CancelToken&)
{
    return VfsError::make(VfsError::NotSupported, QStringLiteral("native search not supported"));
}

FileActionList IFileSystem::actionsFor(const VfsUri&, const FileEntry&)
{
    return {};
}

Result<QStringList> IFileSystem::entriesWithActions(const VfsUri&, const CancelToken&)
{
    return QStringList();
}

Result<FileActionOutcome> IFileSystem::invoke(const QString& id, const VfsUri&, const CancelToken&)
{
    // Named, because an id reaching a drive that never offered it is the shell
    // and the drive disagreeing, and the id is the only thing that says where.
    return VfsError::make(
        VfsError::NotSupported, QStringLiteral("this drive offers no action called \"%1\"").arg(id));
}

DriveOffers IFileSystem::offers() const
{
    QMutexLocker lock(&m_offersMutex);
    return m_offers;
}

void IFileSystem::probe(const VfsUri& target, const CancelToken& cancel)
{
    {
        QMutexLocker lock(&m_offersMutex);
        // Asked once. A second caller does not wait for the first either: it has
        // a listing of its own to get on with, and the answer it came for will
        // be there the next time anything looks.
        if (m_offers.state != DriveOffers::State::Unasked || m_probing)
            return;
        m_probing = true;
    }

    const Result<QStringList> answer = askWhatIsOffered(target, cancel);

    QMutexLocker lock(&m_offersMutex);
    m_probing = false;
    if (cancel.isCancelled()) {
        // Left Unasked rather than Failed: a probe that was called off never
        // found anything out, and the next folder opened here should ask again.
        return;
    }
    if (!answer.ok()) {
        qCWarning(driveLog, "probe of %s failed: %s", qPrintable(target.toString()),
            qPrintable(answer.error().message));
        m_offers.state = DriveOffers::State::Failed;
        return;
    }
    m_offers.state = DriveOffers::State::Answered;
    m_offers.ids = answer.value();
}

Result<QStringList> IFileSystem::askWhatIsOffered(const VfsUri&, const CancelToken&)
{
    return QStringList();
}

Result<void> closeAndReport(QIODevice& device)
{
    device.close();

    // Asked by interface rather than by inspecting errorString(): a QFile that
    // never failed still answers "Unknown error" there, so reading it would turn
    // every local write into a reported failure.
    if (auto* buffered = dynamic_cast<ICommitsOnClose*>(&device)) {
        const VfsError error = buffered->commitError();
        if (error.isError())
            return Result<void>(error);
    }
    return {};
}

} // namespace mole
