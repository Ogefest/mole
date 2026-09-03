#include "core/credentials/SecretStore.h"

#include "core/platform/HostPlatform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef MOLE_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#endif

namespace mole {
namespace {

    constexpr char kMagic[] = "SFMSEC";
    constexpr int kMagicSize = 6;
    constexpr quint8 kVersion = 1;
    constexpr quint8 kKdfScrypt = 1;

    constexpr int kSaltSize = 16;
    constexpr int kNonceSize = 12;
    constexpr int kTagSize = 16;
    constexpr int kKeySize = 32;

    // No scrypt cost here any more: it lives on SecretStore::Cost, so a store can
    // be read with the numbers its own header names rather than with this build's.
    // What this build chooses for a *new* key is that struct's defaults. ADR-0090.

    /// The header, authenticated alongside the contents. Without this an attacker
    /// could rewrite the cost parameters down to nothing and the file would happily
    /// accept a key derived the cheap way.
    QByteArray buildHeader(const QByteArray& salt, const QByteArray& nonce, quint32 n, quint32 r, quint32 p)
    {
        QByteArray header;
        header.append(kMagic, kMagicSize);
        header.append(static_cast<char>(kVersion));
        header.append(static_cast<char>(kKdfScrypt));

        const auto appendNumber = [&header](quint32 value) {
            for (int i = 0; i < 4; ++i)
                header.append(static_cast<char>((value >> (i * 8)) & 0xff));
        };
        appendNumber(n);
        appendNumber(r);
        appendNumber(p);

        header.append(salt);
        header.append(nonce);
        return header;
    }

    quint32 readNumber(const QByteArray& data, int offset)
    {
        quint32 value = 0;
        for (int i = 0; i < 4; ++i)
            value |= static_cast<quint32>(static_cast<quint8>(data.at(offset + i))) << (i * 8);
        return value;
    }

    QByteArray randomBytes(int size)
    {
        QByteArray out(size, 0);
#ifdef MOLE_HAVE_OPENSSL
        if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), size) == 1)
            return out;
#endif
        // Only reached if the system random source failed, which is close to
        // catastrophic; falling back keeps the store usable rather than crashing.
        QRandomGenerator::system()->generate(out.begin(), out.end());
        return out;
    }

#ifdef MOLE_HAVE_OPENSSL

    QByteArray deriveKey(const QString& passphrase, const QByteArray& salt, quint32 n, quint32 r, quint32 p)
    {
        EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "SCRYPT", nullptr);
        if (!kdf)
            return {};

        EVP_KDF_CTX* context = EVP_KDF_CTX_new(kdf);
        EVP_KDF_free(kdf);
        if (!context)
            return {};

        const QByteArray password = passphrase.toUtf8();
        // OpenSSL's own integer types, not Qt's: on this platform quint64 is
        // "unsigned long long" while uint64_t is "unsigned long", and OSSL_PARAM
        // takes a pointer to the latter.
        uint64_t wideN = n;
        uint32_t wideR = r;
        uint32_t wideP = p;

        OSSL_PARAM parameters[]
            = { OSSL_PARAM_construct_octet_string(
                    "pass", const_cast<char*>(password.constData()), static_cast<size_t>(password.size())),
                  OSSL_PARAM_construct_octet_string(
                      "salt", const_cast<char*>(salt.constData()), static_cast<size_t>(salt.size())),
                  OSSL_PARAM_construct_uint64("n", &wideN), OSSL_PARAM_construct_uint32("r", &wideR),
                  OSSL_PARAM_construct_uint32("p", &wideP), OSSL_PARAM_construct_end() };

        QByteArray key(kKeySize, 0);
        const int ok = EVP_KDF_derive(context, reinterpret_cast<unsigned char*>(key.data()),
            static_cast<size_t>(key.size()), parameters);
        EVP_KDF_CTX_free(context);
        return ok == 1 ? key : QByteArray {};
    }

    /// Returns ciphertext with the tag appended.
    QByteArray encrypt(
        const QByteArray& key, const QByteArray& nonce, const QByteArray& header, const QByteArray& plaintext)
    {
        EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
        if (!context)
            return {};

        QByteArray out(plaintext.size() + kTagSize, 0);
        int length = 0;
        bool ok = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
            && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) == 1
            && EVP_EncryptInit_ex(context, nullptr, nullptr,
                   reinterpret_cast<const unsigned char*>(key.constData()),
                   reinterpret_cast<const unsigned char*>(nonce.constData()))
                == 1;

        if (ok && !header.isEmpty()) {
            ok = EVP_EncryptUpdate(context, nullptr, &length,
                     reinterpret_cast<const unsigned char*>(header.constData()),
                     static_cast<int>(header.size()))
                == 1;
        }

        int produced = 0;
        if (ok) {
            ok = EVP_EncryptUpdate(context, reinterpret_cast<unsigned char*>(out.data()), &length,
                     reinterpret_cast<const unsigned char*>(plaintext.constData()),
                     static_cast<int>(plaintext.size()))
                == 1;
            produced = length;
        }
        if (ok) {
            ok = EVP_EncryptFinal_ex(
                     context, reinterpret_cast<unsigned char*>(out.data()) + produced, &length)
                == 1;
            produced += length;
        }
        if (ok) {
            ok = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, kTagSize, out.data() + produced) == 1;
        }

        EVP_CIPHER_CTX_free(context);
        if (!ok)
            return {};
        out.truncate(produced + kTagSize);
        return out;
    }

    /// Empty on any failure, including a wrong key -- which is exactly what an
    /// authenticated cipher is for: a wrong passphrase is a failed tag check, not a
    /// plausible-looking mess.
    QByteArray decrypt(
        const QByteArray& key, const QByteArray& nonce, const QByteArray& header, const QByteArray& sealed)
    {
        if (sealed.size() < kTagSize)
            return {};

        const QByteArray ciphertext = sealed.left(sealed.size() - kTagSize);
        QByteArray tag = sealed.right(kTagSize);

        EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
        if (!context)
            return {};

        QByteArray out(ciphertext.size(), 0);
        int length = 0;
        bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
            && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) == 1
            && EVP_DecryptInit_ex(context, nullptr, nullptr,
                   reinterpret_cast<const unsigned char*>(key.constData()),
                   reinterpret_cast<const unsigned char*>(nonce.constData()))
                == 1;

        if (ok && !header.isEmpty()) {
            ok = EVP_DecryptUpdate(context, nullptr, &length,
                     reinterpret_cast<const unsigned char*>(header.constData()),
                     static_cast<int>(header.size()))
                == 1;
        }

        int produced = 0;
        if (ok) {
            ok = EVP_DecryptUpdate(context, reinterpret_cast<unsigned char*>(out.data()), &length,
                     reinterpret_cast<const unsigned char*>(ciphertext.constData()),
                     static_cast<int>(ciphertext.size()))
                == 1;
            produced = length;
        }
        if (ok) {
            ok = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, kTagSize, tag.data()) == 1;
        }
        if (ok) {
            ok = EVP_DecryptFinal_ex(
                     context, reinterpret_cast<unsigned char*>(out.data()) + produced, &length)
                == 1;
            produced += length;
        }

        EVP_CIPHER_CTX_free(context);
        if (!ok)
            return {};
        out.truncate(produced);
        return out;
    }

#endif // MOLE_HAVE_OPENSSL

} // namespace

SecretStore::SecretStore(QString path, QObject* parent)
    : QObject(parent)
    , m_path(std::move(path))
{
}

SecretStore::~SecretStore()
{
    // Wiped, but not announced. A signal from a destructor hands control to a
    // slot at the point where this object is half gone -- and in the
    // application, where whatever is being torn down alongside it may be too.
    // The drive list took that as an invitation to ask a part-destroyed task
    // manager for a capacity check, and the process died there.
    wipe();
}

QString SecretStore::defaultPath()
{
    const QByteArray override = qgetenv("MOLE_SECRETS_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("credentials.enc"));
}

bool SecretStore::isAvailable()
{
#ifdef MOLE_HAVE_OPENSSL
    return true;
#else
    return false;
#endif
}

bool SecretStore::exists() const
{
    QMutexLocker lock(&m_lock);
    return QFileInfo::exists(m_path);
}

void SecretStore::lock()
{
    QMutexLocker locked(&m_lock);
    if (!wipe())
        return;
    emit unlockedChanged();
}

bool SecretStore::wipe()
{
    if (!m_unlocked && m_key.isEmpty())
        return false;

    // Overwritten before release. Not a guarantee -- a copy may have been made
    // when the buffer grew -- but leaving the key sitting in freed memory for
    // the rest of the process would be worse.
    m_key.fill('\0');
    m_key.clear();
    m_salt.clear();
    m_secrets.clear();
    m_unlocked = false;
    return true;
}

bool SecretStore::create(const QString& passphrase, QString* errorOut)
{
    return create(passphrase, errorOut, Cost {});
}

bool SecretStore::create(const QString& passphrase, QString* errorOut, Cost cost)
{
    QMutexLocker held(&m_lock);
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    if (!isAvailable())
        return fail(QStringLiteral("This build cannot encrypt credentials"));
    if (exists())
        return fail(QStringLiteral("A credential store is already there"));
    if (passphrase.isEmpty())
        return fail(QStringLiteral("A passphrase is required"));

#ifdef MOLE_HAVE_OPENSSL
    m_salt = randomBytes(kSaltSize);
    m_cost = cost;
    m_key = deriveKey(passphrase, m_salt, m_cost.n, m_cost.r, m_cost.p);
    if (m_key.isEmpty())
        return fail(QStringLiteral("Could not derive a key"));

    m_secrets.clear();
    m_unlocked = true;
    if (!writeTo(m_path, m_key, errorOut)) {
        lock();
        return false;
    }
    emit unlockedChanged();
    return true;
#else
    return fail(QStringLiteral("This build cannot encrypt credentials"));
#endif
}

bool SecretStore::unlock(const QString& passphrase, QString* errorOut)
{
    QMutexLocker held(&m_lock);
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    if (!isAvailable())
        return fail(QStringLiteral("This build cannot decrypt credentials"));

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("No credential store here yet"));
    const QByteArray blob = file.readAll();

    const int headerSize = kMagicSize + 2 + 12 + kSaltSize + kNonceSize;
    if (blob.size() < headerSize || !blob.startsWith(QByteArray(kMagic, kMagicSize)))
        return fail(QStringLiteral("This is not a credential store"));
    if (static_cast<quint8>(blob.at(kMagicSize)) != kVersion)
        return fail(QStringLiteral("This store was written by a newer version"));

#ifdef MOLE_HAVE_OPENSSL
    int offset = kMagicSize + 2;
    const quint32 n = readNumber(blob, offset);
    const quint32 r = readNumber(blob, offset + 4);
    const quint32 p = readNumber(blob, offset + 8);
    offset += 12;
    const QByteArray salt = blob.mid(offset, kSaltSize);
    offset += kSaltSize;
    const QByteArray nonce = blob.mid(offset, kNonceSize);
    offset += kNonceSize;

    const QByteArray header = blob.left(offset);
    const QByteArray sealed = blob.mid(offset);

    QByteArray key = deriveKey(passphrase, salt, n, r, p);
    if (key.isEmpty())
        return fail(QStringLiteral("Could not derive a key"));

    // Shorter than a tag is not a store this code ever wrote: even an empty one
    // seals to sixteen bytes. decrypt() answers empty for it, and the test below
    // reads that as "there was nothing to decrypt" -- so a file cut back to its
    // header opened as unlocked and empty under any passphrase at all, with no
    // tag ever checked, and the next write re-keyed it under whatever had been
    // typed. See MOLE-343.
    if (sealed.size() < kTagSize) {
        key.fill('\0');
        return fail(QStringLiteral("Wrong passphrase, or the file has been altered"));
    }

    const QByteArray plaintext = decrypt(key, nonce, header, sealed);
    if (plaintext.isEmpty() && !sealed.isEmpty()) {
        key.fill('\0');
        // A failed tag check. Could be the wrong passphrase or a tampered file,
        // and saying which would tell an attacker something they should not get.
        return fail(QStringLiteral("Wrong passphrase, or the file has been altered"));
    }

    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(plaintext, &parseError);
    if (!plaintext.isEmpty() && (parseError.error != QJsonParseError::NoError || !document.isObject())) {
        key.fill('\0');
        return fail(QStringLiteral("The store decrypted but its contents make no sense"));
    }

    m_secrets.clear();
    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        m_secrets.insert(it.key(), it.value().toString());

    m_key = key;
    m_salt = salt;
    // What the key was actually derived with, so the next write puts it back
    // rather than this build's constants. See ADR-0090.
    m_cost = Cost { n, r, p };
    m_unlocked = true;
    emit unlockedChanged();
    return true;
#else
    return fail(QStringLiteral("This build cannot decrypt credentials"));
#endif
}

SecretStore::Rollback SecretStore::take() const
{
    return Rollback { m_secrets, m_key, m_salt, m_cost };
}

void SecretStore::restore(const Rollback& previous)
{
    // The key first and by hand, because the one being dropped is a derived key
    // sitting in ordinary memory and the whole class is careful about that.
    m_key.fill('\0');
    m_secrets = previous.secrets;
    m_key = previous.key;
    m_salt = previous.salt;
    m_cost = previous.cost;
}

bool SecretStore::writeTo(const QString& path, const QByteArray& key, QString* errorOut) const
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

#ifdef MOLE_HAVE_OPENSSL
    QJsonObject object;
    for (auto it = m_secrets.constBegin(); it != m_secrets.constEnd(); ++it)
        object.insert(it.key(), it.value());
    const QByteArray plaintext = QJsonDocument(object).toJson(QJsonDocument::Compact);

    // The salt stays put -- the key was derived from it and we do not have the
    // passphrase here to derive another. The nonce is fresh every time, which is
    // the part that actually has to be.
    const QByteArray nonce = randomBytes(kNonceSize);
    const QByteArray header = buildHeader(m_salt, nonce, m_cost.n, m_cost.r, m_cost.p);
    const QByteArray sealed = encrypt(key, nonce, header, plaintext);
    if (sealed.isEmpty() && !plaintext.isEmpty())
        return fail(QStringLiteral("Could not encrypt the store"));

    const QFileInfo info(path);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return fail(QStringLiteral("Could not create the profile directory"));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Could not write the credential store"));
    file.write(header);
    file.write(sealed);

    // Readable only by its owner, where that means something -- and set *before*
    // the commit, on the temporary, so the file is never in place with any other
    // mode. QSaveFile widens its temporary to 0666 & ~umask when the target does
    // not exist, so a store created on a machine with a permissive umask spent
    // the instant between the rename and this call world-readable. The contents
    // are encrypted, but there is no reason to hand the ciphertext to every
    // process on the machine either, and the answer is checked rather than
    // dropped: a mode that could not be set is a promise this class makes on its
    // own page and would not be keeping.
    //
    // Not attempted on Windows, because it does nothing there and returns
    // success. Qt maps its permission flags onto the read-only attribute and
    // cannot express "only this account", so the file keeps whatever ACL it
    // inherited from its directory -- and code that reads as though the file
    // were mode-protected everywhere is how a security property that differs by
    // platform turns into a surprise. What protects it there is the encryption,
    // plus the profile directory's own ACL; see the note on the class.
    if (hostPlatform() != HostPlatform::Windows
        && !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        return fail(QStringLiteral("Could not make the credential store readable only by you"));
    }

    if (!file.commit())
        return fail(QStringLiteral("Could not commit the credential store"));
    return true;
#else
    Q_UNUSED(path)
    Q_UNUSED(key)
    return fail(QStringLiteral("This build cannot encrypt credentials"));
#endif
}

QString SecretStore::secret(const QString& key) const
{
    QMutexLocker lock(&m_lock);
    return m_unlocked ? m_secrets.value(key) : QString();
}

bool SecretStore::setSecret(const QString& key, const QString& value, QString* errorOut)
{
    QMutexLocker lock(&m_lock);
    if (!m_unlocked) {
        if (errorOut)
            *errorOut = QStringLiteral("The credential store is locked");
        return false;
    }
    // Written before it is believed: a change kept in memory after a write that
    // did not land is an answer from secret() that the file has never held, and
    // the next successful write puts it there without anybody asking again.
    const Rollback previous = take();
    m_secrets.insert(key, value);
    if (!writeTo(m_path, m_key, errorOut)) {
        restore(previous);
        return false;
    }
    emit changed();
    return true;
}

bool SecretStore::removeSecret(const QString& key, QString* errorOut)
{
    QMutexLocker lock(&m_lock);
    if (!m_unlocked) {
        if (errorOut)
            *errorOut = QStringLiteral("The credential store is locked");
        return false;
    }
    const Rollback previous = take();
    if (m_secrets.remove(key) == 0)
        return true;
    if (!writeTo(m_path, m_key, errorOut)) {
        restore(previous);
        return false;
    }
    emit changed();
    return true;
}

int SecretStore::removeSecretsWithPrefix(const QString& prefix, QString* errorOut)
{
    QMutexLocker lock(&m_lock);
    if (!m_unlocked || prefix.isEmpty())
        return 0;

    const Rollback previous = take();
    int removed = 0;
    for (const QString& key : m_secrets.keys()) {
        if (key.startsWith(prefix))
            removed += static_cast<int>(m_secrets.remove(key));
    }
    if (removed > 0) {
        if (!writeTo(m_path, m_key, errorOut)) {
            restore(previous);
            return 0;
        }
        emit changed();
    }
    return removed;
}

QStringList SecretStore::keys() const
{
    QMutexLocker lock(&m_lock);
    if (!m_unlocked)
        return {};
    QStringList out = m_secrets.keys();
    out.sort();
    return out;
}

bool SecretStore::changePassphrase(
    const QString& oldPassphrase, const QString& newPassphrase, QString* errorOut)
{
    QMutexLocker held(&m_lock);
    if (!unlock(oldPassphrase, errorOut))
        return false;
    if (newPassphrase.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("A passphrase is required");
        return false;
    }

#ifdef MOLE_HAVE_OPENSSL
    // The one place a key is *made* rather than read, so the one place this
    // build's own cost applies -- raising it here is what upgrades a store, and
    // it is the only moment the passphrase is in hand to re-derive with.
    const QByteArray salt = randomBytes(kSaltSize);
    const Cost cost;
    QByteArray key = deriveKey(newPassphrase, salt, cost.n, cost.r, cost.p);
    if (key.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("Could not derive a key");
        return false;
    }

    // Written before any of it is believed. Assigning first and writing after
    // left the file carrying the old passphrase while the object encrypted with
    // the new one, so the next successful write produced a file that only the
    // passphrase the user had been told was rejected would open. See MOLE-343.
    const Rollback previous = take();
    m_salt = salt;
    m_cost = cost;
    m_key = key;
    if (!writeTo(m_path, m_key, errorOut)) {
        restore(previous);
        return false;
    }
    return true;
#else
    return false;
#endif
}

} // namespace mole
