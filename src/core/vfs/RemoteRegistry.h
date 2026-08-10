#pragma once

#include "core/credentials/SecretStore.h"
#include "core/vfs/VfsUri.h"

#include <QObject>
#include <QVariantMap>

namespace mole {

/// One drive the user has configured.
///
/// Split in two on purpose. The settings are ordinary and live in a readable
/// file, so a configuration can be inspected, diffed and backed up like anything
/// else. The secrets are not there at all -- only the fact that a field has one,
/// so the registry knows to ask the credential store for it at connect time.
struct RemoteDrive
{
    QString id;
    QString name;
    /// Which factory makes it, by scheme.
    QString factoryScheme;
    /// Which of that factory's variants, when it offers several.
    QString variant;
    /// Where inside the remote this drive is rooted.
    QString root;
    /// Everything that is not a secret.
    QVariantMap settings;
    /// Field keys whose values live in the credential store.
    QStringList secretFields;
    /// Connect as soon as the application starts, rather than on first use.
    bool mountAtStartup = true;

    bool isValid() const { return !id.isEmpty() && !name.isEmpty() && !factoryScheme.isEmpty(); }
    /// The uri scheme this drive is reached by. Derived from the name so two
    /// drives cannot collide, and stable so bookmarks keep working.
    QString scheme() const;
    VfsUri rootUri() const;
    /// Where in the credential store a field's value lives.
    QString secretKeyFor(const QString& field) const;
};

/// The configured drives, and the only place that puts credentials back into a
/// configuration.
class RemoteRegistry : public QObject
{
    Q_OBJECT

public:
    RemoteRegistry(QString path, SecretStore* secrets, QObject* parent = nullptr);

    /// Honours MOLE_REMOTES_PATH so tests never touch the user's own drives.
    static QString defaultPath();

    bool load();
    bool save() const;

    QList<RemoteDrive> drives() const { return m_drives; }
    RemoteDrive drive(const QString& id) const;
    /// The configured drive a uri belongs to, matched on its scheme. An invalid
    /// drive when the uri is not one of theirs — a local path, an archive, a
    /// mount nobody configured. Here rather than in a caller because the rule
    /// that a drive's scheme comes from its name is this file's.
    RemoteDrive driveForUri(const VfsUri& uri) const;
    /// Adds or replaces by id, storing `secrets` in the credential store rather
    /// than in the settings file. Returns false when a secret was given but the
    /// store is locked -- writing it in the clear instead is not an option.
    ///
    /// A drive saved without an id is given one here, and `storedIdOut` reports
    /// it. Callers need it: a new drive cannot be looked up again by anything but
    /// its id, and the caller is the only one that knows it just made it.
    bool put(const RemoteDrive& drive, const QVariantMap& secretValues, QString* errorOut = nullptr,
        QString* storedIdOut = nullptr);
    /// Removes the drive and its credentials together.
    bool remove(const QString& id);

    /// The settings a factory needs, with secrets filled back in. Empty when the
    /// store is locked and the drive needs one, so a caller cannot accidentally
    /// connect with a blank password.
    QVariantMap configFor(const RemoteDrive& drive, QString* errorOut = nullptr) const;

    /// Whether every drive's secrets are reachable right now.
    bool needsUnlocking() const;
    /// The same question about one drive: it has secrets and the store is shut.
    /// Asked by anything that has to say why a drive cannot be connected
    /// without decrypting a credential to find out.
    bool needsUnlocking(const RemoteDrive& drive) const;

signals:
    void drivesChanged();

private:
    QString m_path;
    SecretStore* m_secrets = nullptr;
    QList<RemoteDrive> m_drives;
};

} // namespace mole
