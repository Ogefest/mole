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

    /// What the platform does, which is what the volume usually does: NTFS and a
    /// default APFS volume fold case, ext4 does not. A volume that disagrees with
    /// its platform -- a case-sensitive APFS one, a FAT stick on Linux -- is not
    /// asked, because Qt has no portable way to ask it and guessing wrong in the
    /// permissive direction is what loses a file.
    Qt::CaseSensitivity pathCaseSensitivity() const override
    {
        return VfsUri::caseSensitivityFor(QStringLiteral("file"));
    }

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<SpaceInfo> space(const VfsUri& target) override;
    Result<AccessInfo> access(const VfsUri& target) override;
    Result<FileEntry> stat(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override;
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
