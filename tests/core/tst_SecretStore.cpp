#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/credentials/SecretStore.h"
#include "core/platform/HostPlatform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <atomic>
#include <thread>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

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
    void aStoreWrittenWithOtherCostParametersKeepsThem();
    void aFileTruncatedToItsHeaderIsRefusedUnderEveryPassphrase();
    void aWriteThatCouldNotLandLeavesTheStoreAsItWas();
    void aChangeOfPassphraseThatCouldNotBeWrittenLeavesTheOldOneWorking();
    void whetherAStoreIsOpenCanBeAskedWhileItIsOpening();
    void aSecretCanBeAskedForWhileTheStoreIsOpening();

    void changingThePassphraseKeepsTheSecrets();
    void removesSecretsByPrefix();
    void reportsWhetherItCanEncryptAtAll();
    void beingDestroyedIsNotAStateChange();

private:
    QString path() const;
    /// Makes `folder` a directory this account can read and walk but not write
    /// into, and answers whether that really took. A read-only configuration
    /// directory is what this is standing in for, and it has to leave reading
    /// working: a write that fails only because the file could not be *opened*
    /// would not reach the code these cases are about.
    static bool madeReadOnly(const QString& folder);
    /// Writes a store at `where` holding one secret, at a cost far below
    /// this build's: the cases that use it are about a crossing between
    /// threads and not about scrypt, and at 2^15 each would spend a third of
    /// a second deriving a key it does not care about.
    static bool layAStore(const QString& where, const QString& passphrase);

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

bool TestSecretStore::madeReadOnly(const QString& folder)
{
#ifndef Q_OS_UNIX
    Q_UNUSED(folder)
    return false;
#else
    if (!QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner))
        return false;
    // Root writes into a directory with no write bit at all, and so does a
    // filesystem that does not enforce them. Asked rather than assumed, because
    // a case that cannot fail for the reason it names is worse than none.
    QFile probe(QDir(folder).filePath(QStringLiteral("probe")));
    if (!probe.open(QIODevice::WriteOnly))
        return true;
    probe.close();
    probe.remove();
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return false;
#endif
}

bool TestSecretStore::layAStore(const QString& where, const QString& passphrase)
{
    SecretStore store(where);
    return store.create(passphrase, nullptr, SecretStore::Cost { 1024, 8, 1 })
        && store.setSecret(QStringLiteral("k"), QStringLiteral("v"));
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

    if (hostPlatform() == HostPlatform::Windows) {
        // Nothing is claimed here, and that is the documented position rather
        // than an omission. Qt maps its permission flags onto the read-only
        // attribute on Windows and cannot express "only this account", so a mode
        // is not set at all -- the file keeps the ACL it inherits from the user's
        // profile directory, and the encryption is what protects it. Asserting a
        // mode here would be asserting something the platform does not have.
        QVERIFY(store.exists());
        return;
    }

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

/// The parameters in the header are the parameters the key was derived with.
///
/// unlock() derives with the n, r and p it reads from the file and then keeps
/// only the key and the salt; every later write built the header from the
/// compile-time constants. The two agree only while the constants have never
/// changed -- and the comment on them says they are in the file "so a future
/// build can raise them without orphaning existing stores", which is the
/// opposite of what happened. The first write after such a build produced a
/// header whose cost no longer matched the key, and the next unlock reported a
/// wrong passphrase for ever. Every credential lost, looking exactly like a
/// forgotten passphrase.
///
/// Stood in for here by going the other way -- a store written with a *cheaper*
/// cost than this build's -- because a test cannot raise a constant.
void TestSecretStore::aStoreWrittenWithOtherCostParametersKeepsThem()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    // N = 1024 rather than this build's 32768: what a store written by a build
    // with a different constant looks like, in the only way a test can produce
    // one -- the header is authenticated, so editing the number in place would
    // be tampering rather than a different parameter.
    {
        SecretStore store(path());
        QVERIFY(store.create(QStringLiteral("phrase"), nullptr, SecretStore::Cost { 1024, 8, 1 }));
        QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));
    }

    SecretStore store(path());
    QString error;
    QVERIFY2(store.unlock(QStringLiteral("phrase"), &error), qPrintable(error));
    QCOMPARE(store.secret(QStringLiteral("k")), QStringLiteral("v"));

    // The write that used to break it: it built the header from this build's
    // constants while the key had been derived with the file's.
    QVERIFY2(store.setSecret(QStringLiteral("k2"), QStringLiteral("v2"), &error), qPrintable(error));

    SecretStore reopened(path());
    QVERIFY2(reopened.unlock(QStringLiteral("phrase"), &error),
        "a store whose cost parameters are not this build's was orphaned by its own next write");
    QCOMPARE(reopened.secret(QStringLiteral("k")), QStringLiteral("v"));
    QCOMPARE(reopened.secret(QStringLiteral("k2")), QStringLiteral("v2"));
}

/// A file that is nothing but its header opened under any passphrase at all.
///
/// decrypt() answers empty for anything shorter than a tag, and the failure test
/// was `plaintext.isEmpty() && !sealed.isEmpty()`. For exactly the header both
/// halves are false, so the store was marked unlocked and empty with no tag ever
/// checked -- and the next write re-keyed the file under whatever had been
/// typed. A store written by this code can never be header-only, so that shape
/// is always truncation or tampering.
void TestSecretStore::aFileTruncatedToItsHeaderIsRefusedUnderEveryPassphrase()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    {
        SecretStore store(path());
        QVERIFY(store.create(QStringLiteral("phrase")));
        QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));
    }

    // Everything up to the sealed body: magic, version, a reserved byte, the
    // three cost numbers, the salt and the nonce. Measured rather than counted,
    // from a store with nothing in it -- its body is "{}" and a sixteen-byte tag.
    const QString empty = QDir(m_dir->path()).filePath(QStringLiteral("empty.enc"));
    {
        SecretStore store(empty);
        QVERIFY(store.create(QStringLiteral("phrase"), nullptr, SecretStore::Cost { 1024, 8, 1 }));
    }
    const qint64 headerSize = QFileInfo(empty).size() - 16 - 2;
    QVERIFY(headerSize > 0);

    QFile file(path());
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.resize(headerSize));
    file.close();

    for (const QString& tried : { QStringLiteral("phrase"), QStringLiteral("anything at all") }) {
        SecretStore store(path());
        QString error;
        QVERIFY2(!store.unlock(tried, &error),
            qPrintable(QStringLiteral("a header with nothing after it opened under \"%1\"").arg(tried)));
        QVERIFY2(!store.isUnlocked(), "and it must not be left unlocked either");
    }
}

/// A write that did not land must not leave the object ahead of the file.
///
/// setSecret changed the map and then wrote; on failure it returned false and
/// kept the change. So secret() answered with something the file has never held,
/// and the next successful write would put it there without anybody having asked
/// again.
void TestSecretStore::aWriteThatCouldNotLandLeavesTheStoreAsItWas()
{
#ifndef Q_OS_UNIX
    QSKIP("permissions work differently on this platform");
#else
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");
    if (geteuid() == 0)
        QSKIP("running as root, where a read-only directory is not read-only");

    const QString folder = QDir(m_dir->path()).filePath(QStringLiteral("locked"));
    QVERIFY(QDir().mkpath(folder));
    const QString file = QDir(folder).filePath(QStringLiteral("credentials.enc"));

    SecretStore store(file);
    QVERIFY(store.create(QStringLiteral("phrase")));
    QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));

    if (!madeReadOnly(folder))
        QSKIP("this account can write into a directory it has no write bit on");

    QString error;
    const bool stored = store.setSecret(QStringLiteral("k"), QStringLiteral("changed"), &error);
    const QString afterwards = store.secret(QStringLiteral("k"));
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QVERIFY2(!stored, "a secret that could not be written was reported as stored");
    QCOMPARE(afterwards, QStringLiteral("v"));

    // And the file agrees with the object rather than with either half of it.
    SecretStore reopened(file);
    QVERIFY2(reopened.unlock(QStringLiteral("phrase"), &error), qPrintable(error));
    QCOMPARE(reopened.secret(QStringLiteral("k")), QStringLiteral("v"));
#endif
}

/// The worst of the four, because it locks the user out.
///
/// changePassphrase() assigned the new salt and key before writing. When the
/// write failed the file still carried the old passphrase, the object encrypted
/// with the new one, and the next successful setSecret() wrote a file that only
/// the passphrase the user had been *told was rejected* would open.
void TestSecretStore::aChangeOfPassphraseThatCouldNotBeWrittenLeavesTheOldOneWorking()
{
#ifndef Q_OS_UNIX
    QSKIP("permissions work differently on this platform");
#else
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");
    if (geteuid() == 0)
        QSKIP("running as root, where a read-only directory is not read-only");

    const QString folder = QDir(m_dir->path()).filePath(QStringLiteral("locked"));
    QVERIFY(QDir().mkpath(folder));
    const QString file = QDir(folder).filePath(QStringLiteral("credentials.enc"));

    SecretStore store(file);
    QVERIFY(store.create(QStringLiteral("old")));
    QVERIFY(store.setSecret(QStringLiteral("k"), QStringLiteral("v")));

    if (!madeReadOnly(folder))
        QSKIP("this account can write into a directory it has no write bit on");

    QString error;
    const bool changed = store.changePassphrase(QStringLiteral("old"), QStringLiteral("new"), &error);
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QVERIFY2(!changed, "a passphrase change that could not be written was reported as done");

    // Whatever the object now holds, the next write it manages has to be one the
    // old passphrase opens -- because the old passphrase is what the file has.
    QVERIFY2(store.setSecret(QStringLiteral("k2"), QStringLiteral("v2"), &error), qPrintable(error));

    SecretStore reopened(file);
    QVERIFY2(reopened.unlock(QStringLiteral("old"), &error),
        "the passphrase the user still has stopped opening their own store");
    QCOMPARE(reopened.secret(QStringLiteral("k2")), QStringLiteral("v2"));
#endif
}

/// The reason there is a lock at all: unlock() runs on a worker thread now.
///
/// Deriving a key is a noticeable fraction of a second by design, so it happens
/// off the thread that draws -- and the window goes on asking the store
/// questions while it does: isUnlocked() for the dialog's own state, secret()
/// and keys() through RemoteRegistry. unlock() writes every member those answers
/// come from, from whichever thread called it.
///
/// So what these two run is the crossing itself, and what reads it is
/// ThreadSanitizer: `make tsan TESTS=SecretStore`. Neither can fail on its own
/// for the thing it is about -- a guarded read and an unguarded one give the same
/// answer -- which is also why neither says anything about what the answers are
/// *during* the crossing. A store that has not opened yet answering with nothing
/// and one that has answering with the secret are both right.
///
/// **One question per crossing, and that is not tidiness.** Taking a mutex
/// orders everything the taker did before it, so a loop that asked all of these
/// questions together would have its own guarded calls order its unguarded read
/// against the worker's write -- measured, with the lock taken back out of
/// isUnlocked(): asked alone the sanitizer sees the race, asked between two
/// guarded calls it sees nothing at all. Splitting them is what leaves anything
/// to find.
void TestSecretStore::whetherAStoreIsOpenCanBeAskedWhileItIsOpening()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(layAStore(path(), QStringLiteral("phrase")));

    SecretStore store(path());
    std::atomic_bool asking { false };
    std::atomic_bool opened { false };
    bool unlocked = false;

    std::thread worker([&store, &asking, &opened, &unlocked] {
        // Started only once this thread is reading, so the derivation really is
        // in flight while the question is being asked rather than finished
        // before it is first put.
        while (!asking.load())
            std::this_thread::yield();
        unlocked = store.unlock(QStringLiteral("phrase"));
        opened = true;
    });

    asking = true;
    while (!opened.load()) {
        if (store.isUnlocked())
            break;
        // Yielded rather than spun flat out: the thread that has to take the
        // store's lock once and hold it for the whole derivation should not have
        // to fight for it. A yield orders nothing, which is the point.
        std::this_thread::yield();
    }
    worker.join();

    QVERIFY2(unlocked, "the store did not open on the worker thread");
    QVERIFY(store.isUnlocked());
}

/// The same crossing, against the question RemoteRegistry asks -- and so against
/// the members unlock() fills in rather than the flag it ends with. `keys()`
/// reads the same map behind the same lock, so it is the one question here. See
/// the case above for why it is asked on its own.
void TestSecretStore::aSecretCanBeAskedForWhileTheStoreIsOpening()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(layAStore(path(), QStringLiteral("phrase")));

    SecretStore store(path());
    std::atomic_bool asking { false };
    std::atomic_bool opened { false };
    bool unlocked = false;

    std::thread worker([&store, &asking, &opened, &unlocked] {
        while (!asking.load())
            std::this_thread::yield();
        unlocked = store.unlock(QStringLiteral("phrase"));
        opened = true;
    });

    asking = true;
    while (!opened.load()) {
        store.secret(QStringLiteral("k"));
        std::this_thread::yield();
    }
    worker.join();

    QVERIFY2(unlocked, "the store did not open on the worker thread");
    QCOMPARE(store.secret(QStringLiteral("k")), QStringLiteral("v"));
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
