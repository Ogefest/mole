#pragma once

#include "core/vfs/IFileSystem.h"
#include "core/vfs/IFileSystemFactory.h"

namespace mole {

/// The reference backend: plain local disk through QFileInfo/QDirIterator.
/// Read it as the worked example when writing sftp/s3/webdav.
class LocalFileSystem final : public IFileSystem
{
public:
    QString scheme() const override { return QStringLiteral("file"); }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<SpaceInfo> space(const VfsUri& target) override;
    Result<AccessInfo> access(const VfsUri& target) override;
    Result<FileEntry> stat(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target) override;
};

class LocalFileSystemFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("file"); }
    QString displayName() const override { return QStringLiteral("Local disk"); }
    QString iconName() const override { return QStringLiteral("drive-harddisk"); }

    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;
};

} // namespace mole
