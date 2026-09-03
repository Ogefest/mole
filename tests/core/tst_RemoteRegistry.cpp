#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/credentials/SecretStore.h"
#include "core/vfs/RemoteRegistry.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace mole;
using namespace mole::test;

/// Configured drives, and the line between what is written down and what is not.
class TestRemoteRegistry : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void storesADriveAndReadsItBack();
    void requiresAUniqueName();
    void derivesASchemeFromTheName();

    void keepsPasswordsOutOfTheSettingsFile();
    void refusesToSaveASecretWhileLocked();
    void handsSecretsBackOnlyWhenUnlocked();
    void refusesToConnectWithAMissingSecret();
    void renamingADriveKeepsItsCredentials();
    void deletingADriveTakesItsCredentials();

    void saysWhenItNeedsUnlocking();

    void aDriveListThatCannotBeParsedIsKeptRatherThanReplaced();
    void aDriveThatCannotBeWrittenSaysSo();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<RemoteRegistry> m_registry;

    RemoteDrive sampleDrive() const;
};

void TestRemoteRegistry::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());

    m_secrets
        = std::make_unique<SecretStore>(QDir(m_dir->path()).filePath(QStringLiteral("credentials.enc")));
    m_registry = std::make_unique<RemoteRegistry>(
        QDir(m_dir->path()).filePath(QStringLiteral("drives.json")), m_secrets.get());
}

void TestRemoteRegistry::cleanup()
{
    m_registry.reset();
    m_secrets.reset();
    m_dir.reset();
}

RemoteDrive TestRemoteRegistry::sampleDrive() const
{
    RemoteDrive drive;
    drive.name = QStringLiteral("Office NAS");
    drive.factoryScheme = QStringLiteral("sftp");
    drive.variant = QStringLiteral("sftp");
    drive.root = QStringLiteral("/data");
    drive.settings.insert(QStringLiteral("host"), QStringLiteral("nas.example.org"));
    drive.settings.insert(QStringLiteral("user"), QStringLiteral("ada"));
    return drive;
}

void TestRemoteRegistry::storesADriveAndReadsItBack()
{
    QVERIFY(m_registry->put(sampleDrive(), {}));
    QCOMPARE(m_registry->drives().size(), 1);

    RemoteRegistry reopened(QDir(m_dir->path()).filePath(QStringLiteral("drives.json")), m_secrets.get());
    QVERIFY(reopened.load());
    QCOMPARE(reopened.drives().size(), 1);

    const RemoteDrive drive = reopened.drives().first();
    QCOMPARE(drive.name, QStringLiteral("Office NAS"));
    QCOMPARE(drive.variant, QStringLiteral("sftp"));
    QCOMPARE(drive.settings.value(QStringLiteral("host")).toString(), QStringLiteral("nas.example.org"));
}

void TestRemoteRegistry::requiresAUniqueName()
{
    QVERIFY(m_registry->put(sampleDrive(), {}));

    RemoteDrive clash = sampleDrive();
    clash.id.clear();
    QString error;
    // The name becomes the uri scheme, so two drives answering to one would
    // make every bookmark ambiguous.
    QVERIFY2(!m_registry->put(clash, {}, &error), "a duplicate name must be refused");
    QVERIFY(!error.isEmpty());
}

void TestRemoteRegistry::derivesASchemeFromTheName()
{
    RemoteDrive drive = sampleDrive();
    QCOMPARE(drive.scheme(), QStringLiteral("officenas"));
    QCOMPARE(drive.rootUri().scheme(), QStringLiteral("officenas"));
}

void TestRemoteRegistry::keepsPasswordsOutOfTheSettingsFile()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    QVERIFY(m_secrets->create(QStringLiteral("phrase")));

    QVariantMap secretValues;
    secretValues.insert(QStringLiteral("pass"), QStringLiteral("s3cr3t-value"));
    QVERIFY(m_registry->put(sampleDrive(), secretValues));

    QFile file(QDir(m_dir->path()).filePath(QStringLiteral("drives.json")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();

    // The settings file is meant to be readable, diffable and backed up like
    // any other configuration -- which is exactly why the password is not in it.
    QVERIFY2(!contents.contains("s3cr3t-value"), "the password must not be in the settings file");
    // Only that the field has one, so the registry knows to go and ask.
    QVERIFY(contents.contains("pass"));
    QVERIFY(contents.contains("nas.example.org"));
}

void TestRemoteRegistry::refusesToSaveASecretWhileLocked()
{
    QVariantMap secretValues;
    secretValues.insert(QStringLiteral("pass"), QStringLiteral("s3cr3t"));

    QString error;
    // Writing it into the readable file instead is the one thing this design
    // exists to prevent, so the save fails rather than degrading.
    QVERIFY2(!m_registry->put(sampleDrive(), secretValues, &error),
        "a locked store must refuse rather than fall back to plain text");
    QVERIFY(!error.isEmpty());
    QVERIFY(m_registry->drives().isEmpty());
}

void TestRemoteRegistry::handsSecretsBackOnlyWhenUnlocked()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    QVERIFY(m_secrets->create(QStringLiteral("phrase")));
    QVariantMap secretValues;
    secretValues.insert(QStringLiteral("pass"), QStringLiteral("s3cr3t"));
    QVERIFY(m_registry->put(sampleDrive(), secretValues));

    const RemoteDrive drive = m_registry->drives().first();
    QVariantMap config = m_registry->configFor(drive);
    QCOMPARE(config.value(QStringLiteral("pass")).toString(), QStringLiteral("s3cr3t"));
    QCOMPARE(config.value(QStringLiteral("host")).toString(), QStringLiteral("nas.example.org"));

    m_secrets->lock();
    QString error;
    config = m_registry->configFor(drive, &error);
    // Empty rather than partial. Connecting with a blank password fails in a
    // way that looks like a wrong one -- and on some backends it succeeds as an
    // anonymous user, which is worse.
    QVERIFY2(config.isEmpty(), "a locked store must not yield a config with a blank password");
    QVERIFY(!error.isEmpty());
}

void TestRemoteRegistry::refusesToConnectWithAMissingSecret()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    QVERIFY(m_secrets->create(QStringLiteral("phrase")));
    QVariantMap secretValues;
    secretValues.insert(QStringLiteral("pass"), QStringLiteral("s3cr3t"));
    QVERIFY(m_registry->put(sampleDrive(), secretValues));

    const RemoteDrive drive = m_registry->drives().first();
    // The credential store was restored without this drive's entry, which is
    // what a partial backup looks like.
    QVERIFY(m_secrets->removeSecret(drive.secretKeyFor(QStringLiteral("pass"))));

    QString error;
    QVERIFY(m_registry->configFor(drive, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("missing")));
}

void TestRemoteRegistry::renamingADriveKeepsItsCredentials()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    QVERIFY(m_secrets->create(QStringLiteral("phrase")));
    QVariantMap secretValues;
    secretValues.insert(QStringLiteral("pass"), QStringLiteral("s3cr3t"));
    QVERIFY(m_registry->put(sampleDrive(), secretValues));

    RemoteDrive renamed = m_registry->drives().first();
    renamed.name = QStringLiteral("Home NAS");
    QVERIFY(m_registry->put(renamed, {}));

    // Credentials are keyed by id, not by name, so renaming does not orphan
    // them -- which would leave the drive unusable and the password stranded.
    const RemoteDrive stored = m_registry->drives().first();
    QCOMPARE(stored.name, QStringLiteral("Home NAS"));
    QCOMPARE(
        m_registry->configFor(stored).value(QStringLiteral("pass")).toString(), QStringLiteral("s3cr3t"));
}

void TestRemoteRegistry::deletingADriveTakesItsCredentials()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    QVERIFY(m_secrets->create(QStringLiteral("phrase")));
    QVariantMap secretValues;
    secretValues.insert(QStringLiteral("pass"), QStringLiteral("s3cr3t"));
    QVERIFY(m_registry->put(sampleDrive(), secretValues));

    const QString id = m_registry->drives().first().id;
    QVERIFY(m_registry->remove(id));

    // Otherwise the password for a drive that no longer exists sits in the
    // store for ever with nothing referring to it.
    QVERIFY(m_secrets->keys().isEmpty());
}

void TestRemoteRegistry::saysWhenItNeedsUnlocking()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    // A drive with no credentials needs nothing.
    QVERIFY(m_registry->put(sampleDrive(), {}));
    QVERIFY(!m_registry->needsUnlocking());

    QVERIFY(m_secrets->create(QStringLiteral("phrase")));
    RemoteDrive withSecret = m_registry->drives().first();
    QVariantMap secretValues;
    secretValues.insert(QStringLiteral("pass"), QStringLiteral("s3cr3t"));
    QVERIFY(m_registry->put(withSecret, secretValues));

    m_secrets->lock();
    // So the interface can ask once at startup rather than failing per drive.
    QVERIFY(m_registry->needsUnlocking());
}

/// One stray byte in drives.json used to cost every configured drive.
///
/// load() cleared the list, failed to parse, returned false and kept nothing --
/// and the first put() afterwards wrote the empty list over the file. The
/// secrets stayed in the credential store under ids that no longer existed
/// anywhere, so they could not even be found to be removed. A hand edit with a
/// bad comma was enough. See ADR-0089.
void TestRemoteRegistry::aDriveListThatCannotBeParsedIsKeptRatherThanReplaced()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("drives.json"));
    const QByteArray typedByHand("{ \"drives\": [ { \"name\": \"Office NAS\", } ] }");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(typedByHand);
    }

    RemoteRegistry registry(path, m_secrets.get());
    QVERIFY2(!registry.load(), "a file that could not be parsed is not a load that succeeded");
    QVERIFY(registry.isDamaged());

    // Beside itself, whole. This is the only copy of what the drives were.
    const QString kept = registry.damagedCopyPath();
    QVERIFY2(!kept.isEmpty(), "the unreadable file has to be somewhere");
    QFile keptFile(kept);
    QVERIFY(keptFile.open(QIODevice::ReadOnly));
    QCOMPARE(keptFile.readAll(), typedByHand);

    // And the registry goes on working, because there is nothing left to lose:
    // a new file where the old one was is not the old one being destroyed.
    QVERIFY(registry.put(sampleDrive(), {}));
    RemoteRegistry reopened(path, m_secrets.get());
    QVERIFY(reopened.load());
    QCOMPARE(reopened.drives().size(), 1);
}

void TestRemoteRegistry::aDriveThatCannotBeWrittenSaysSo()
{
#ifndef Q_OS_UNIX
    QSKIP("permissions work differently on this platform");
#else
    if (geteuid() == 0)
        QSKIP("running as root, where a read-only directory is not read-only");

    const QString folder = QDir(m_dir->path()).filePath(QStringLiteral("locked"));
    QVERIFY(QDir().mkpath(folder));
    RemoteRegistry registry(QDir(folder).filePath(QStringLiteral("drives.json")), m_secrets.get());
    QVERIFY(registry.load());

    if (!madeUnreadable(folder))
        QSKIP("this account can write into a directory with no permissions at all");

    QSignalSpy complained(&registry, &JsonFileStore::saveFailed);
    CapturedWarnings logged;
    const bool stored = registry.put(sampleDrive(), {});
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QVERIFY2(!stored, "a drive that could not be written was reported as stored");
    QCOMPARE(complained.count(), 1);
    QVERIFY2(logged.contains(QStringLiteral("drives.json")), qPrintable(logged.joined()));
#endif
}

MOLE_TEST_MAIN(TestRemoteRegistry)
#include "tst_RemoteRegistry.moc"
