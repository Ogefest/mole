#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
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
///
/// THREADING
/// ---------
/// Every public method takes the store's own lock, so it is safe to use from any
/// thread. That is not decoration: deriving a key is a noticeable fraction of a
/// second by design, and the only place to put it is a worker thread while the
/// window stays live -- which means the window is free to ask this object
/// questions while the derivation is running. See ADR-0090.
///
/// Recursive, because changePassphrase() is unlock() followed by a write.
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
    /// Under the lock like everything else, and not because reading a bool is
    /// slow: unlock() sets it on whichever thread derived the key, and this is
    /// read from the window and from RemoteRegistry while that is running.
    bool isUnlocked() const
    {
        QMutexLocker held(&m_lock);
        return m_unlocked;
    }

    /// What deriving a key from a passphrase costs.
    ///
    /// Written into the file rather than assumed, so a later build can raise it
    /// without orphaning the stores earlier ones wrote -- which is what the
    /// numbers being in the header has always been for. A store is read with the
    /// cost *its own header* names, whatever this build would choose now.
    ///
    /// The defaults are this build's choice: N = 2^15 with r = 8 is a noticeable
    /// fraction of a second and about 32 MB of memory, which is the point.
    struct Cost
    {
        quint32 n = 32768;
        quint32 r = 8;
        quint32 p = 1;
    };

    /// Creates a new store. Fails if one is already there -- overwriting would
    /// destroy every credential without asking.
    ///
    /// `cost` is a parameter and not a constant for one reason: the promise that
    /// a store survives a change of it cannot be tested without writing a store
    /// at a different one. See ADR-0090.
    bool create(const QString& passphrase, QString* errorOut = nullptr);
    bool create(const QString& passphrase, QString* errorOut, Cost cost);
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

    /// Everything a failed write has to be able to put back.
    ///
    /// A mutator changes the object, writes, and undoes the change when the
    /// write did not land -- so the object and the file never disagree. They
    /// used to: a secret kept in memory after a failed write was an answer the
    /// file had never held, and a passphrase changed but not written locked the
    /// user out of their own store. See MOLE-343.
    struct Rollback
    {
        QHash<QString, QString> secrets;
        QByteArray key;
        QByteArray salt;
        Cost cost;
    };
    Rollback take() const;
    void restore(const Rollback& previous);

    QString m_path;
    /// The derived key, held only while unlocked. Wiped on lock.
    QByteArray m_key;
    /// Fixed for the life of a store: the key is derived from it, so a new salt
    /// means a new key. Only a change of passphrase generates one. The nonce, by
    /// contrast, is fresh on every write -- reusing one with the same key is the
    /// classic way to break GCM outright.
    QByteArray m_salt;
    /// What the key in m_key was actually derived with, which is what the next
    /// write has to put back in the header. Read from the file on unlock and
    /// chosen by this build only where a key is *made* -- create() and a change
    /// of passphrase. Writing the constants back instead is what turned a raised
    /// cost into every credential lost, with a message saying the passphrase was
    /// wrong. See ADR-0090.
    Cost m_cost;
    QHash<QString, QString> m_secrets;
    bool m_unlocked = false;
    /// Taken by every public method. Recursive because changePassphrase() is
    /// unlock() and then a write, and splitting that in two to avoid one lock
    /// would be two ways of opening a store.
    mutable QRecursiveMutex m_lock;
};

} // namespace mole
