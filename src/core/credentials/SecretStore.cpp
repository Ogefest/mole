#include "core/credentials/SecretStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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

    /// scrypt cost. 2^15 with r=8 needs about 32 MB and takes a noticeable fraction
    /// of a second -- deliberately, because the whole point is that guessing a
    /// passphrase should be expensive. Stored in the file so a future build can
    /// raise them without orphaning existing stores.
    constexpr quint32 kScryptN = 32768;
    constexpr quint32 kScryptR = 8;
    constexpr quint32 kScryptP = 1;

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
    return QFileInfo::exists(m_path);
}

void SecretStore::lock()
{
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
    m_key = deriveKey(passphrase, m_salt, kScryptN, kScryptR, kScryptP);
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
    m_unlocked = true;
    emit unlockedChanged();
    return true;
#else
    return fail(QStringLiteral("This build cannot decrypt credentials"));
#endif
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
    const QByteArray header = buildHeader(m_salt, nonce, kScryptN, kScryptR, kScryptP);
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
    if (!file.commit())
        return fail(QStringLiteral("Could not commit the credential store"));

    // Readable only by its owner. The contents are encrypted, but there is no
    // reason to hand the ciphertext to every process on the machine either.
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return true;
#else
    Q_UNUSED(path)
    Q_UNUSED(key)
    return fail(QStringLiteral("This build cannot encrypt credentials"));
#endif
}

QString SecretStore::secret(const QString& key) const
{
    return m_unlocked ? m_secrets.value(key) : QString();
}

bool SecretStore::setSecret(const QString& key, const QString& value, QString* errorOut)
{
    if (!m_unlocked) {
        if (errorOut)
            *errorOut = QStringLiteral("The credential store is locked");
        return false;
    }
    m_secrets.insert(key, value);
    if (!writeTo(m_path, m_key, errorOut))
        return false;
    emit changed();
    return true;
}

bool SecretStore::removeSecret(const QString& key, QString* errorOut)
{
    if (!m_unlocked) {
        if (errorOut)
            *errorOut = QStringLiteral("The credential store is locked");
        return false;
    }
    if (m_secrets.remove(key) == 0)
        return true;
    if (!writeTo(m_path, m_key, errorOut))
        return false;
    emit changed();
    return true;
}

int SecretStore::removeSecretsWithPrefix(const QString& prefix, QString* errorOut)
{
    if (!m_unlocked || prefix.isEmpty())
        return 0;

    int removed = 0;
    for (const QString& key : m_secrets.keys()) {
        if (key.startsWith(prefix))
            removed += static_cast<int>(m_secrets.remove(key));
    }
    if (removed > 0) {
        if (!writeTo(m_path, m_key, errorOut))
            return 0;
        emit changed();
    }
    return removed;
}

QStringList SecretStore::keys() const
{
    if (!m_unlocked)
        return {};
    QStringList out = m_secrets.keys();
    out.sort();
    return out;
}

bool SecretStore::changePassphrase(
    const QString& oldPassphrase, const QString& newPassphrase, QString* errorOut)
{
    if (!unlock(oldPassphrase, errorOut))
        return false;
    if (newPassphrase.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("A passphrase is required");
        return false;
    }

#ifdef MOLE_HAVE_OPENSSL
    const QHash<QString, QString> kept = m_secrets;
    m_salt = randomBytes(kSaltSize);
    QByteArray key = deriveKey(newPassphrase, m_salt, kScryptN, kScryptR, kScryptP);
    if (key.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("Could not derive a key");
        return false;
    }

    m_key.fill('\0');
    m_key = key;
    m_secrets = kept;
    return writeTo(m_path, m_key, errorOut);
#else
    return false;
#endif
}

} // namespace mole
