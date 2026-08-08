#pragma once

#include "core/vfs/IFileSystem.h"

#include <QJsonObject>

namespace mole {

/// One configured remote, served through rclone.
///
/// The remote is addressed by a *connection string* -- `:sftp,host=…,user=…:` --
/// rather than by a name in rclone's own configuration file. That is the whole
/// reason credentials never leave this application: rclone writes its config in
/// a reversible obfuscation it calls "obscure", which is not encryption, and a
/// password put there would be recoverable by anyone who reads the file.
///
/// So nothing is ever written to rclone.conf. The credentials live in the
/// encrypted store, are decrypted into the connection string for the length of
/// one call, and go no further.
class RcloneFileSystem final : public IFileSystem
{
public:
    /// `connectionString` is the `:backend,key=value:` prefix; `root` is the
    /// path within it that this mount is rooted at.
    RcloneFileSystem(QString scheme, QString connectionString, QString root);

    QString scheme() const override { return m_scheme; }
    VfsCapabilities capabilities() const override;

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override;
    Result<FileEntry> stat(const VfsUri& target) override;
    Result<SpaceInfo> space(const VfsUri& target) override;

    Result<void> makeDirectory(const VfsUri& target) override;
    Result<void> remove(const VfsUri& target, bool recursive) override;
    Result<void> rename(const VfsUri& from, const VfsUri& to) override;

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target) override;
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target) override;

private:
    /// The path inside the remote for a uri of this mount, relative to the
    /// mount root -- which is part of the fs specification, not of the path.
    QString remotePathFor(const VfsUri& uri) const;
    /// What rclone calls `fs`: the connection string with the mount root
    /// appended. Putting the root here rather than in every remote path is what
    /// makes the paths relative to the mount instead of to rclone's own idea of
    /// where it is.
    QString fsSpec() const { return m_connectionString + m_root; }
    FileEntry entryFromJson(const QJsonObject& json, const VfsUri& parent) const;
    static VfsError errorFrom(const QString& message);

    QString m_scheme;
    QString m_connectionString;
    QString m_root;
};

} // namespace mole
