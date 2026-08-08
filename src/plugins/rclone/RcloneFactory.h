#pragma once

#include "core/vfs/IFileSystemFactory.h"

namespace mole {

/// Every backend rclone can talk to, offered as drives.
///
/// The list and every field on it come from rclone itself, at run time. That is
/// the point: this file does not know what S3 needs or what changed in the last
/// rclone release, so it cannot be wrong about either, and a new provider
/// appears without a line changing here.
class RcloneFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("rclone"); }
    QString displayName() const override { return QStringLiteral("Cloud and network drives"); }

    bool isAvailable() const override;
    QString unavailableReason() const override;

    /// One per rclone backend, with its options as fields. Built once and
    /// cached: it is a few thousand fields and it does not change while the
    /// application is running.
    QList<BackendVariant> variants() const override;

    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override;

    /// Turns a filled-in form into an rclone connection string. Public because
    /// it is worth testing directly -- getting the escaping wrong is how a
    /// password with a comma in it silently connects to the wrong place.
    static QString connectionStringFor(const QString& backend, const QVariantMap& config);

private:
    mutable QList<BackendVariant> m_variants;
};

} // namespace mole
