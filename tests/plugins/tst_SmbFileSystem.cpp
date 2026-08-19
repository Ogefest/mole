#include "plugins/network/SmbFileSystem.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"

#include <QCoreApplication>

using namespace mole;
using namespace mole::test;

namespace {

/// The share to work against, from the environment like every other live suite.
struct Account
{
    QString host;
    QString share;
    QString user;
    QString password;
    QString domain;
    QString base;

    bool isConfigured() const { return !host.isEmpty() && !share.isEmpty(); }

    QVariantMap asConfig() const
    {
        return QVariantMap { { QStringLiteral("host"), host }, { QStringLiteral("share"), share },
            { QStringLiteral("user"), user }, { QStringLiteral("password"), password },
            { QStringLiteral("domain"), domain } };
    }
};

Account accountFromEnvironment()
{
    const auto value = [](const char* name) { return QString::fromLocal8Bit(qgetenv(name)); };
    Account account;
    account.host = value("MOLE_TEST_SMB_HOST");
    account.share = value("MOLE_TEST_SMB_SHARE");
    account.user = value("MOLE_TEST_SMB_USER");
    account.password = value("MOLE_TEST_SMB_PASS");
    account.domain = value("MOLE_TEST_SMB_DOMAIN");
    account.base = value("MOLE_TEST_SMB_BASE");
    return account;
}

} // namespace

/// Windows shares.
class TestSmbFileSystem : public QObject
{
    Q_OBJECT

private slots:
    void aFormWithoutAServerIsRefused();
    void aPastedUncPathIsAccepted();
    void theFormAsksOnlyWhatSmbNeeds();
    void aUrlIsBuiltFromTheShareTheRootAndThePath();
    void itSatisfiesTheConformanceSuite();
};

void TestSmbFileSystem::aFormWithoutAServerIsRefused()
{
    SmbFileSystemFactory factory;
    QString error;
    QVERIFY(factory.create(QVariantMap {}, &error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("server")), qPrintable(error));

    // And a server with no share is half an address.
    error.clear();
    QVERIFY(factory.create(QVariantMap { { QStringLiteral("host"), QStringLiteral("fileserver") } }, &error)
        == nullptr);
    QVERIFY2(error.contains(QStringLiteral("share")), qPrintable(error));
}

void TestSmbFileSystem::aPastedUncPathIsAccepted()
{
    // What somebody actually has in front of them is `\\fileserver\photos`,
    // from a Windows address bar or a colleague's message. Refusing it and
    // asking for the two halves separately is asking a question whose answer is
    // already on their clipboard.
    const SmbSettings pasted = SmbFileSystemFactory::settingsFrom(
        QVariantMap { { QStringLiteral("share"), QStringLiteral("\\\\fileserver\\photos") } });
    QCOMPARE(pasted.host, QStringLiteral("fileserver"));
    QCOMPARE(pasted.share, QStringLiteral("photos"));

    // The same thing written the other way round, which is what a Mac or a
    // Linux desktop shows.
    const SmbSettings slashes = SmbFileSystemFactory::settingsFrom(
        QVariantMap { { QStringLiteral("share"), QStringLiteral("//fileserver/photos/") } });
    QCOMPARE(slashes.host, QStringLiteral("fileserver"));
    QCOMPARE(slashes.share, QStringLiteral("photos"));

    // And a host given separately is left alone.
    const SmbSettings split
        = SmbFileSystemFactory::settingsFrom(QVariantMap { { QStringLiteral("host"), QStringLiteral("nas") },
            { QStringLiteral("share"), QStringLiteral("backup") } });
    QCOMPARE(split.host, QStringLiteral("nas"));
    QCOMPARE(split.share, QStringLiteral("backup"));
}

void TestSmbFileSystem::theFormAsksOnlyWhatSmbNeeds()
{
    SmbFileSystemFactory factory;
    QStringList keys;
    QStringList required;
    for (const ConnectionField& field : factory.connectionFields()) {
        keys.append(field.key);
        if (field.required && !field.advanced)
            required.append(field.key);
    }
    QCOMPARE(keys, QStringList({ "host", "share", "user", "password", "domain", "root" }));

    // A share that allows guests needs neither, so neither may be demanded.
    QCOMPARE(required, QStringList({ "host", "share" }));
}

void TestSmbFileSystem::aUrlIsBuiltFromTheShareTheRootAndThePath()
{
    SmbSettings settings;
    settings.host = QStringLiteral("fileserver");
    settings.share = QStringLiteral("photos");
    settings.remoteRoot = QStringLiteral("/holidays/");
    SmbFileSystem fs(QStringLiteral("smb"), settings);

    QCOMPARE(fs.urlFor(VfsUri::fromString(QStringLiteral("smb://drive/"))),
        QStringLiteral("smb://fileserver/photos/holidays"));
    QCOMPARE(fs.urlFor(VfsUri::fromString(QStringLiteral("smb://drive/2019/sunset.raw"))),
        QStringLiteral("smb://fileserver/photos/holidays/2019/sunset.raw"));

    // A drive rooted at the share itself has no extra segment, and no stray
    // slash where one used to be.
    SmbSettings bare = settings;
    bare.remoteRoot.clear();
    SmbFileSystem atTheRoot(QStringLiteral("smb"), bare);
    QCOMPARE(atTheRoot.urlFor(VfsUri::fromString(QStringLiteral("smb://drive/"))),
        QStringLiteral("smb://fileserver/photos"));
}

void TestSmbFileSystem::itSatisfiesTheConformanceSuite()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No SMB share in the environment; set MOLE_TEST_SMB_HOST and MOLE_TEST_SMB_SHARE "
              "to run this against a real server.");
    }

    // The same catalogue every other backend answers. A new backend held to it
    // from its first day is the whole reason the servers went up before the code
    // did -- see MOLE-36.
    SmbSettings settings = SmbFileSystemFactory::settingsFrom(account.asConfig());
    settings.remoteRoot = (account.base.isEmpty() ? QString() : account.base)
        + QStringLiteral("/mole-smb-%1").arg(QCoreApplication::applicationPid());

    auto fileSystem = std::make_shared<SmbFileSystem>(QStringLiteral("smb"), settings);

    // The working directory is made through the backend, which the conformance
    // suite then works inside. Seeding through the code under test is not ideal
    // and is unavoidable here: libsmbclient is the only client on this machine,
    // so a "raw" fixture would be the same library talking to the same share.
    const VfsUri root(QStringLiteral("smb"), QString(), QStringLiteral("/"));
    fileSystem->remove(root, true);
    const Result<void> made = fileSystem->makeDirectory(root);
    QVERIFY2(made.ok(), qPrintable(made.error().message));

    ConformanceContext context;
    context.fileSystem = fileSystem;
    context.root = root;
    context.seedFile = [fileSystem, root](const QString& relative, const QByteArray& contents) {
        Result<std::unique_ptr<QIODevice>> stream
            = fileSystem->openWrite(root.child(relative), contents.size());
        if (!stream.ok()) {
            qWarning("seeding %s: %s", qPrintable(relative), qPrintable(stream.error().message));
            return false;
        }
        const qint64 put = stream.value()->write(contents);
        if (put != contents.size()) {
            qWarning("seeding %s: wrote %lld of %lld -- %s", qPrintable(relative),
                static_cast<long long>(put), static_cast<long long>(contents.size()),
                qPrintable(stream.value()->errorString()));
        }
        stream.value()->close();
        return put == contents.size();
    };
    context.seedDir = [fileSystem, root](const QString& relative) {
        return fileSystem->makeDirectory(root.child(relative)).ok();
    };

    runFileSystemConformance(context);

    fileSystem->remove(root, true);
}

MOLE_TEST_MAIN(TestSmbFileSystem)

#include "tst_SmbFileSystem.moc"
