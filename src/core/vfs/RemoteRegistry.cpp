#include "core/vfs/RemoteRegistry.h"

#include "core/vfs/IFileSystemFactory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace mole {

QString driveSchemeFor(const QString& name)
{
    // Derived from the name so a uri reads as something a person recognises,
    // and reduced to what a scheme may contain. Two drives called the same
    // thing would collide, which is why the name is required to be unique.
    QString slug = name.toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QString());
    if (slug.isEmpty())
        slug = QStringLiteral("drive");
    return slug;
}

QString RemoteDrive::scheme() const
{
    return driveSchemeFor(name);
}

VfsUri RemoteDrive::rootUri() const
{
    return VfsUri(scheme(), name, QStringLiteral("/"));
}

QString RemoteDrive::secretKeyFor(const QString& field) const
{
    // Keyed by drive id rather than by name, so renaming a drive does not
    // orphan its credentials.
    return QStringLiteral("drive/%1/%2").arg(id, field);
}

RemoteRegistry::RemoteRegistry(QString path, SecretStore* secrets, QObject* parent)
    : JsonFileStore(std::move(path), parent)
    , m_secrets(secrets)
{
    // Opening or shutting the credential store does not change a drive, but it
    // changes every answer this gives about one: whether it can be connected,
    // and whether its settings can be handed to a factory. Anything watching
    // the drives is watching for that too, and has no way to see it otherwise.
    if (m_secrets)
        connect(m_secrets, &SecretStore::unlockedChanged, this, &RemoteRegistry::drivesChanged);
}

QString RemoteRegistry::defaultPath()
{
    return pathFor("MOLE_REMOTES_PATH", QStringLiteral("drives.json"));
}

bool RemoteRegistry::load()
{
    QJsonObject root;
    const Read read = readRoot(&root);
    if (read == Read::Damaged)
        return false; // kept, and nothing is written over it until somebody says

    m_drives.clear();
    if (read == Read::Missing) {
        emit drivesChanged();
        return true; // no drives configured yet is not an error
    }

    const QJsonArray drives = root.value(QStringLiteral("drives")).toArray();
    for (const QJsonValue& value : drives) {
        const QJsonObject object = value.toObject();

        RemoteDrive drive;
        drive.id = object.value(QStringLiteral("id")).toString();
        drive.name = object.value(QStringLiteral("name")).toString();
        drive.factoryScheme = object.value(QStringLiteral("factory")).toString();
        drive.variant = object.value(QStringLiteral("variant")).toString();
        drive.root = object.value(QStringLiteral("root")).toString();
        drive.settings = object.value(QStringLiteral("settings")).toObject().toVariantMap();
        drive.mountAtStartup = object.value(QStringLiteral("mountAtStartup")).toBool(true);

        const QJsonArray secretFields = object.value(QStringLiteral("secretFields")).toArray();
        for (const QJsonValue& field : secretFields)
            drive.secretFields.append(field.toString());

        if (drive.isValid())
            m_drives.append(drive);
    }

    emit drivesChanged();
    return true;
}

bool RemoteRegistry::save()
{
    QJsonArray drives;
    for (const RemoteDrive& drive : m_drives) {
        QJsonArray secretFields;
        for (const QString& field : drive.secretFields)
            secretFields.append(field);

        drives.append(QJsonObject {
            { QStringLiteral("id"), drive.id },
            { QStringLiteral("name"), drive.name },
            { QStringLiteral("factory"), drive.factoryScheme },
            { QStringLiteral("variant"), drive.variant },
            { QStringLiteral("root"), drive.root },
            // Only the field *names*. The values are in the credential store,
            // and nothing here ever writes one to this file.
            { QStringLiteral("secretFields"), secretFields },
            { QStringLiteral("settings"), QJsonObject::fromVariantMap(drive.settings) },
            { QStringLiteral("mountAtStartup"), drive.mountAtStartup },
        });
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("drives")] = drives;

    return writeRoot(root);
}

RemoteDrive RemoteRegistry::drive(const QString& id) const
{
    for (const RemoteDrive& drive : m_drives) {
        if (drive.id == id)
            return drive;
    }
    return {};
}

RemoteDrive RemoteRegistry::driveForUri(const VfsUri& uri) const
{
    if (!uri.isValid())
        return {};
    for (const RemoteDrive& drive : m_drives) {
        if (drive.scheme() == uri.scheme())
            return drive;
    }
    return {};
}

bool RemoteRegistry::put(
    const RemoteDrive& drive, const QVariantMap& secretValues, QString* errorOut, QString* storedIdOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    RemoteDrive stored = drive;
    if (stored.id.isEmpty())
        stored.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!stored.isValid())
        return fail(QStringLiteral("A drive needs a name and a kind"));

    // A name has to be unique: it becomes the uri scheme, and two drives
    // answering to one scheme would make every bookmark ambiguous.
    for (const RemoteDrive& other : m_drives) {
        if (other.id != stored.id && other.scheme() == stored.scheme())
            return fail(QStringLiteral("Another drive is already called that"));
    }

    if (!secretValues.isEmpty()) {
        if (!m_secrets || !m_secrets->isUnlocked()) {
            // The alternative is writing a password into a readable file, which
            // is the one thing this design exists to prevent.
            return fail(QStringLiteral("Unlock the credential store before saving a password"));
        }
        stored.secretFields.clear();
        for (auto it = secretValues.constBegin(); it != secretValues.constEnd(); ++it) {
            const QString value = it.value().toString();
            if (value.isEmpty())
                continue;
            QString secretError;
            if (!m_secrets->setSecret(stored.secretKeyFor(it.key()), value, &secretError))
                return fail(secretError);
            stored.secretFields.append(it.key());
        }
    }

    // Belt and braces: a secret must never reach the settings map, whatever the
    // caller passed in.
    for (const QString& field : stored.secretFields)
        stored.settings.remove(field);

    bool replaced = false;
    for (int i = 0; i < m_drives.size(); ++i) {
        if (m_drives.at(i).id == stored.id) {
            m_drives[i] = stored;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        m_drives.append(stored);

    if (!save())
        return fail(QStringLiteral("Could not write the drive list"));

    if (storedIdOut)
        *storedIdOut = stored.id;
    emit drivesChanged();
    return true;
}

bool RemoteRegistry::remove(const QString& id)
{
    const auto position = std::find_if(
        m_drives.begin(), m_drives.end(), [&id](const RemoteDrive& drive) { return drive.id == id; });
    if (position == m_drives.end())
        return false;

    // The credentials go with it. Leaving them behind would keep a password for
    // a drive that no longer exists, for ever.
    if (m_secrets && m_secrets->isUnlocked())
        m_secrets->removeSecretsWithPrefix(QStringLiteral("drive/%1/").arg(id));

    m_drives.erase(position);
    const bool written = save();
    emit drivesChanged();
    // The drive is gone from the model either way -- its credentials have
    // already been removed and there is no putting that back -- but a removal
    // the file did not take is one that comes back at the next start, and
    // saying so is the difference between a puzzle and a message.
    return written;
}

QVariantMap RemoteRegistry::configFor(const RemoteDrive& drive, QString* errorOut) const
{
    QVariantMap config = drive.settings;
    config.insert(IFileSystemFactory::variantKey(), drive.variant);
    config.insert(QStringLiteral("__root"), drive.root);
    config.insert(QStringLiteral("__scheme"), drive.scheme());

    if (drive.secretFields.isEmpty())
        return config;

    if (!m_secrets || !m_secrets->isUnlocked()) {
        if (errorOut)
            *errorOut = QStringLiteral("The credential store is locked");
        // Empty rather than partial: connecting with a blank password would
        // fail in a way that looks like a wrong password, and on some backends
        // it would succeed as an anonymous user, which is worse.
        return {};
    }

    for (const QString& field : drive.secretFields) {
        const QString value = m_secrets->secret(drive.secretKeyFor(field));
        if (value.isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral("The password for %1 is missing").arg(drive.name);
            return {};
        }
        config.insert(field, value);
    }

    return config;
}

bool RemoteRegistry::needsUnlocking() const
{
    return std::any_of(
        m_drives.begin(), m_drives.end(), [this](const RemoteDrive& drive) { return needsUnlocking(drive); });
}

bool RemoteRegistry::needsUnlocking(const RemoteDrive& drive) const
{
    if (drive.secretFields.isEmpty())
        return false;
    return !m_secrets || !m_secrets->isUnlocked();
}

} // namespace mole
