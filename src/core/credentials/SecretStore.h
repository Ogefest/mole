#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

namespace mole {

/// Where credentials live: encrypted with a passphrase, and portable.
///
/// The two requirements pull against each other and the resolution is worth
/// stating, because it is the whole design.
///
/// "Not plain text" usually means the system keyring -- Secret Service, kwallet,
/// the Windows credential store. But a keyring is deliberately tied to the login
/// it belongs to, so a keyring-backed secret does *not* survive reinstalling the
/// operating system, however carefully the configuration was backed up. That is
/// the point of a keyring, and it is the opposite of what is wanted here.
///
/// So the key comes from a passphrase the user carries, not from the machine.
/// Nothing in the file depends on this computer: copy it to a fresh install,
/// type the passphrase, and the credentials are there. The cost is a prompt
/// once per session, which is the honest price of portability.
///
/// scrypt for the derivation, because a passphrase is low-entropy and a
/// memory-hard function is what makes guessing it expensive. AES-256-GCM for the
/// contents, so tampering is detected rather than silently decrypting to
/// rubbish -- and the header is authenticated too, or an attacker could weaken
/// the derivation parameters and have the file accept the result.
///
/// WHAT PROTECTS THE FILE AT REST, PER PLATFORM
/// --------------------------------------------
/// The encryption is the protection everywhere. What differs is what else there
/// is, and it is written down here because a security property that varies by
/// platform and is documented on none is the part that turns into a surprise.
///
///   Unix     0600 as well, set when the file is written. The ciphertext is not
///            handed to every process on the machine.
///   Windows  Whatever ACL the file inherits from the user's profile directory,
///            which on an ordinary installation is already account-scoped. No
///            mode is set: Qt maps its permission flags onto the read-only
///            attribute there and cannot express "only this account", so the
///            call would have done nothing and returned success.
///
/// Setting a real ACL on Windows was considered and deliberately not done. The
/// lock that matters is a passphrase-derived AES-256-GCM blob, the profile
/// directory is already account-scoped, and platform code that could only be
/// tested on a machine nobody has buys very little over saying plainly what is
/// there. LocalFileSystem handles the same Qt limitation the same way, clearing
/// the permission string on Windows rather than showing something synthesised.
class SecretStore : public QObject
{
    Q_OBJECT

public:
    explicit SecretStore(QString path, QObject* parent = nullptr);
    ~SecretStore() override;

    /// Honours MOLE_SECRETS_PATH so tests never touch the user's own.
    static QString defaultPath();
    /// Whether this build can encrypt at all. False means the store refuses to
    /// hold anything rather than writing secrets in the clear.
    static bool isAvailable();

    /// True when a store already exists on disk. A fresh profile has none, and
    /// the interface asks the user to choose a passphrase rather than for one
    /// they have not set.
    bool exists() const;
    bool isUnlocked() const { return m_unlocked; }

    /// Creates a new store. Fails if one is already there -- overwriting would
    /// destroy every credential without asking.
    bool create(const QString& passphrase, QString* errorOut = nullptr);
    /// Opens an existing one. A wrong passphrase fails; it cannot half-open.
    bool unlock(const QString& passphrase, QString* errorOut = nullptr);
    /// Forgets the key and the contents. The file is untouched.
    void lock();

    /// Re-encrypts everything under a new passphrase.
    bool changePassphrase(
        const QString& oldPassphrase, const QString& newPassphrase, QString* errorOut = nullptr);

    QString secret(const QString& key) const;
    bool setSecret(const QString& key, const QString& value, QString* errorOut = nullptr);
    bool removeSecret(const QString& key, QString* errorOut = nullptr);
    /// Drops every secret whose key begins with `prefix`. Used when a drive is
    /// deleted, so its credentials go with it.
    int removeSecretsWithPrefix(const QString& prefix, QString* errorOut = nullptr);
    QStringList keys() const;

signals:
    void unlockedChanged();
    void changed();

private:
    /// Clears the key, the salt and everything decrypted from the file, and
    /// says whether there was anything to clear. Silent: lock() announces it,
    /// the destructor does not, because a signal from a destructor reaches a
    /// slot that has no way of knowing what else is already gone.
    bool wipe();

    bool writeTo(const QString& path, const QByteArray& key, QString* errorOut) const;

    QString m_path;
    /// The derived key, held only while unlocked. Wiped on lock.
    QByteArray m_key;
    /// Fixed for the life of a store: the key is derived from it, so a new salt
    /// means a new key. Only a change of passphrase generates one. The nonce, by
    /// contrast, is fresh on every write -- reusing one with the same key is the
    /// classic way to break GCM outright.
    QByteArray m_salt;
    QHash<QString, QString> m_secrets;
    bool m_unlocked = false;
};

} // namespace mole
