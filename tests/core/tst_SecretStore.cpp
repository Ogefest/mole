#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/credentials/SecretStore.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// Credentials at rest. The properties that matter are that the file is
/// unreadable without the passphrase, and that it is not tied to this machine.
class TestSecretStore : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void createsAndReopensAStore();
    void refusesTheWrongPassphrase();
    void refusesToOverwriteAnExistingStore();
    void aLockedStoreGivesNothingAway();

    void theFileNeverContainsTheSecret();
    void theFileIsReadableOnlyByItsOwner();
    void survivesBeingCopiedToAnotherMachine();

    void detectsATamperedFile();
    void detectsWeakenedParameters();

    void changingThePassphraseKeepsTheSecrets();
    void removesSecretsByPrefix();
    void reportsWhetherItCanEncryptAtAll();
    void beingDestroyedIsNotAStateChange();

private:
    QString path() const;

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestSecretStore::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void TestSecretStore::cleanup()
{
    m_dir.reset();
}

QString TestSecretStore::path() const
{
    return QDir(m_dir->path()).filePath(QStringLiteral("credentials.enc"));
}

void TestSecretStore::createsAndReopensAStore()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    {
        SecretStore store(path());
        QVERIFY(!store.exists());
        QString error;
        QVERIFY2(store.create(QStringLiteral("correct horse battery"), &error), qPrintable(error));
        QVERIFY(store.isUnlocked());
        QVERIFY(store.setSecret(QStringLiteral("nas/password"), QStringLiteral("hunter2")));
    }

    SecretStore reopened(path());
    QVERIFY(reopened.exists());
    QString error;
    QVERIFY2(reopened.unlock(QStringLiteral("correct horse battery"), &error), qPrintable(error));
    QCOMPARE(reopened.secret(QStringLiteral("nas/password")), QStringLiteral("hunter2"));
}

void TestSecretStore::refusesTheWrongPassphrase()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    {
        SecretStore store(path());
        QVERIFY(store.create(QStringLiteral("right")));
        QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));
    }

    SecretStore store(path());
    QString error;
    QVERIFY2(!store.unlock(QStringLiteral("wrong"), &error), "a wrong passphrase must not open it");
    QVERIFY(!store.isUnlocked());
    // It cannot half-open: nothing is readable after a failed attempt.
    QCOMPARE(store.secret(QStringLiteral("k")), QString());
    QVERIFY(!error.isEmpty());
}

void TestSecretStore::refusesToOverwriteAnExistingStore()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    SecretStore store(path());
    QVERIFY(store.create(QStringLiteral("first")));
    QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));

    SecretStore second(path());
    QString error;
    // Creating over an existing store would destroy every credential in it
    // without asking, which is not a thing to do quietly.
    QVERIFY2(!second.create(QStringLiteral("second"), &error), "creating must not overwrite");
    QVERIFY(!error.isEmpty());
}

void TestSecretStore::aLockedStoreGivesNothingAway()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    SecretStore store(path());
    QVERIFY(store.create(QStringLiteral("phrase")));
    QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));

    store.lock();
    QVERIFY(!store.isUnlocked());
    QCOMPARE(store.secret(QStringLiteral("k")), QString());
    QVERIFY(store.keys().isEmpty());
    // And it refuses to write rather than silently dropping the change.
    QVERIFY(!store.setSecret(QStringLiteral("k"), QStringLiteral("other")));
}

void TestSecretStore::theFileNeverContainsTheSecret()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    SecretStore store(path());
    QVERIFY(store.create(QStringLiteral("phrase")));
    QVERIFY(store.setSecret(
        QStringLiteral("s3/secret_access_key"), QStringLiteral("AKIAIOSFODNN7EXAMPLE-secret-value")));

    QFile file(path());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray blob = file.readAll();

    // The whole requirement in one assertion: neither the value nor its key is
    // anywhere in the file.
    QVERIFY2(!blob.contains("AKIAIOSFODNN7EXAMPLE"), "the secret must not be in the file");
    QVERIFY2(!blob.contains("secret_access_key"), "nor the name of the thing it is");
    QVERIFY2(!blob.contains("phrase"), "nor the passphrase");
}

void TestSecretStore::theFileIsReadableOnlyByItsOwner()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    SecretStore store(path());
    QVERIFY(store.create(QStringLiteral("phrase")));
    QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));

    const QFile::Permissions permissions = QFile::permissions(path());
    // Encrypted or not, there is no reason to hand the ciphertext to every
    // process on the machine.
    QVERIFY(!permissions.testFlag(QFile::ReadGroup));
    QVERIFY(!permissions.testFlag(QFile::ReadOther));
}

void TestSecretStore::survivesBeingCopiedToAnotherMachine()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    {
        SecretStore store(path());
        QVERIFY(store.create(QStringLiteral("carried in my head")));
        QVERIFY(store.setSecret(QStringLiteral("sftp/pass"), QStringLiteral("s3cret")));
    }

    // What a config backup and a fresh install amounts to: the same bytes, a
    // different place, nothing else carried over. A keyring-backed secret would
    // be unreadable here, which is exactly why this store does not use one.
    QTemporaryDir elsewhere;
    QVERIFY(elsewhere.isValid());
    const QString copied = QDir(elsewhere.path()).filePath(QStringLiteral("restored.enc"));
    QVERIFY(QFile::copy(path(), copied));

    SecretStore restored(copied);
    QString error;
    QVERIFY2(restored.unlock(QStringLiteral("carried in my head"), &error), qPrintable(error));
    QCOMPARE(restored.secret(QStringLiteral("sftp/pass")), QStringLiteral("s3cret"));
}

void TestSecretStore::detectsATamperedFile()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    {
        SecretStore store(path());
        QVERIFY(store.create(QStringLiteral("phrase")));
        QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));
    }

    QFile file(path());
    QVERIFY(file.open(QIODevice::ReadWrite));
    QByteArray blob = file.readAll();
    // Flip a bit in the ciphertext.
    blob[blob.size() - 20] = static_cast<char>(blob.at(blob.size() - 20) ^ 0x01);
    file.seek(0);
    file.write(blob);
    file.close();

    SecretStore store(path());
    QString error;
    // An authenticated cipher, so this fails outright rather than decrypting to
    // plausible-looking rubbish that would then be used as a password.
    QVERIFY2(!store.unlock(QStringLiteral("phrase"), &error), "a tampered file must not open");
}

void TestSecretStore::detectsWeakenedParameters()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    {
        SecretStore store(path());
        QVERIFY(store.create(QStringLiteral("phrase")));
        QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));
    }

    QFile file(path());
    QVERIFY(file.open(QIODevice::ReadWrite));
    QByteArray blob = file.readAll();
    // The scrypt cost sits at offset 8. Drop it to something trivial, which is
    // what an attacker would do to make guessing cheap.
    blob[8] = 0x02;
    blob[9] = 0x00;
    file.seek(0);
    file.write(blob);
    file.close();

    SecretStore store(path());
    QString error;
    // The header is authenticated too, so the change is caught rather than
    // quietly accepted with a key that took no effort to find.
    QVERIFY2(!store.unlock(QStringLiteral("phrase"), &error), "weakened parameters must not be accepted");
}

void TestSecretStore::changingThePassphraseKeepsTheSecrets()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    {
        SecretStore store(path());
        QVERIFY(store.create(QStringLiteral("old one")));
        QVERIFY(store.setSecret(QStringLiteral("a"), QStringLiteral("1")));
        QVERIFY(store.setSecret(QStringLiteral("b"), QStringLiteral("2")));
    }

    {
        SecretStore store(path());
        QString error;
        QVERIFY2(store.changePassphrase(QStringLiteral("old one"), QStringLiteral("new one"), &error),
            qPrintable(error));
    }

    SecretStore store(path());
    QVERIFY(!store.unlock(QStringLiteral("old one")));
    QVERIFY(store.unlock(QStringLiteral("new one")));
    QCOMPARE(store.secret(QStringLiteral("a")), QStringLiteral("1"));
    QCOMPARE(store.secret(QStringLiteral("b")), QStringLiteral("2"));
}

void TestSecretStore::removesSecretsByPrefix()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    SecretStore store(path());
    QVERIFY(store.create(QStringLiteral("phrase")));
    QVERIFY(store.setSecret(QStringLiteral("drive-1/pass"), QStringLiteral("x")));
    QVERIFY(store.setSecret(QStringLiteral("drive-1/key"), QStringLiteral("y")));
    QVERIFY(store.setSecret(QStringLiteral("drive-2/pass"), QStringLiteral("z")));

    // Deleting a drive has to take its credentials with it, or they linger in
    // the store for ever with nothing referring to them.
    QCOMPARE(store.removeSecretsWithPrefix(QStringLiteral("drive-1/")), 2);
    QCOMPARE(store.keys(), QStringList { QStringLiteral("drive-2/pass") });
}

void TestSecretStore::reportsWhetherItCanEncryptAtAll()
{
    SecretStore store(path());
    if (SecretStore::isAvailable()) {
        QVERIFY(store.create(QStringLiteral("phrase")));
    } else {
        QString error;
        // A store that quietly stopped encrypting would be worse than one that
        // says it cannot.
        QVERIFY(!store.create(QStringLiteral("phrase"), &error));
        QVERIFY(!error.isEmpty());
    }
}

/// The destructor wipes the key by calling lock(), and lock() announces that
/// the store has shut. Announcing it from a destructor hands control to a slot
/// at a point where the object emitting is half gone -- and, in the
/// application, where whatever else was being torn down alongside it may be
/// too. It crashed exactly there: the drive list reacted by asking a
/// part-destroyed task manager for a capacity check.
///
/// Wiping the key is the part that matters and it still happens. What must not
/// happen is anyone being told about it.
void TestSecretStore::beingDestroyedIsNotAStateChange()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    auto store = std::make_unique<SecretStore>(path());
    QVERIFY(store->create(QStringLiteral("a passphrase")));

    QSignalSpy shut(store.get(), &SecretStore::unlockedChanged);
    store.reset();
    QCOMPARE(shut.count(), 0);
}

MOLE_TEST_MAIN(TestSecretStore)
#include "tst_SecretStore.moc"
