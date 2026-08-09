#include "core/vfs/IFileSystem.h"

namespace mole {

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

Result<std::unique_ptr<QIODevice>> IFileSystem::openRead(const VfsUri&, qint64)
{
    return VfsError::make(VfsError::NotSupported, QStringLiteral("openRead not supported"));
}

Result<std::unique_ptr<QIODevice>> IFileSystem::openWrite(const VfsUri&)
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

Result<FileEntryList> IFileSystem::search(const VfsUri&, const QString&, const CancelToken&)
{
    return VfsError::make(VfsError::NotSupported, QStringLiteral("native search not supported"));
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
