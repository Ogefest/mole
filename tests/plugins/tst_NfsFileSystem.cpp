#include "plugins/network/NfsFileSystem.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"

#include <QCoreApplication>
#include <QElapsedTimer>

using namespace mole;
using namespace mole::test;

namespace {

/// The export to work against, from the environment like every other live suite.
struct Export
{
    QString host;
    QString exportPath;
    QString base;

    bool isConfigured() const { return !host.isEmpty() && !exportPath.isEmpty(); }

    QVariantMap asConfig() const
    {
        return QVariantMap { { QStringLiteral("host"), host }, { QStringLiteral("export"), exportPath } };
    }
};

Export exportFromEnvironment()
{
    const auto value = [](const char* name) { return QString::fromLocal8Bit(qgetenv(name)); };
    Export target;
    target.host = value("MOLE_TEST_NFS_HOST");
    target.exportPath = value("MOLE_TEST_NFS_EXPORT");
    target.base = value("MOLE_TEST_NFS_BASE");
    return target;
}

} // namespace

/// NFS exports.
class TestNfsFileSystem : public QObject
{
    Q_OBJECT

private slots:
    void aFormWithoutAServerIsRefused();
    void aPastedShowmountLineIsAccepted();
    void theFormAsksOnlyWhatNfsNeeds();
    void aPathIsBuiltFromTheRootAndTheUri();
    void twoDrivesInsideOneExportShareItsConnection();
    void anExportThatIsNotThereFailsRatherThanHangs();
    void itSatisfiesTheConformanceSuite();
};

void TestNfsFileSystem::aFormWithoutAServerIsRefused()
{
    NfsFileSystemFactory factory;
    QString error;
    QVERIFY(factory.create(QVariantMap {}, &error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("server")), qPrintable(error));

    // And a server with no export is half an address.
    error.clear();
    QVERIFY(factory.create(QVariantMap { { QStringLiteral("host"), QStringLiteral("fileserver") } }, &error)
        == nullptr);
    QVERIFY2(error.contains(QStringLiteral("export")), qPrintable(error));
}

void TestNfsFileSystem::aPastedShowmountLineIsAccepted()
{
    // What somebody actually has in front of them is what `showmount -e` printed
    // or what is in their /etc/fstab: `fileserver:/srv/media`. Refusing it and
    // asking for the two halves separately is asking a question whose answer is
    // already on their clipboard.
    const NfsSettings pasted = NfsFileSystemFactory::settingsFrom(
        QVariantMap { { QStringLiteral("export"), QStringLiteral("fileserver:/srv/media") } });
    QCOMPARE(pasted.host, QStringLiteral("fileserver"));
    QCOMPARE(pasted.exportPath, QStringLiteral("/srv/media"));

    // An export named without its leading slash is the same export.
    const NfsSettings bare
        = NfsFileSystemFactory::settingsFrom(QVariantMap { { QStringLiteral("host"), QStringLiteral("nas") },
            { QStringLiteral("export"), QStringLiteral("srv/backup/") } });
    QCOMPARE(bare.host, QStringLiteral("nas"));
    QCOMPARE(bare.exportPath, QStringLiteral("/srv/backup"));

    // Nothing is claimed unless it was asked for: an empty user id field means
    // "whatever this process runs as", not user zero.
    QCOMPARE(bare.uid, -1);
    QCOMPARE(bare.gid, -1);
    const NfsSettings claimed
        = NfsFileSystemFactory::settingsFrom(QVariantMap { { QStringLiteral("host"), QStringLiteral("nas") },
            { QStringLiteral("export"), QStringLiteral("/srv") }, { QStringLiteral("uid"), 1000 },
            { QStringLiteral("gid"), 100 } });
    QCOMPARE(claimed.uid, 1000);
    QCOMPARE(claimed.gid, 100);
}

void TestNfsFileSystem::theFormAsksOnlyWhatNfsNeeds()
{
    NfsFileSystemFactory factory;
    QStringList keys;
    QStringList required;
    for (const ConnectionField& field : factory.connectionFields()) {
        keys.append(field.key);
        if (field.required && !field.advanced)
            required.append(field.key);
    }
    QCOMPARE(keys, QStringList({ "host", "export", "root", "uid", "gid" }));

    // No password, because NFS has none. Asking for one would be inventing a
    // security story the protocol does not have.
    QCOMPARE(required, QStringList({ "host", "export" }));
}

void TestNfsFileSystem::aPathIsBuiltFromTheRootAndTheUri()
{
    NfsSettings settings;
    settings.host = QStringLiteral("fileserver");
    settings.exportPath = QStringLiteral("/srv/media");
    settings.remoteRoot = QStringLiteral("/holidays/");
    NfsFileSystem fs(QStringLiteral("nfs"), settings);

    // Inside the export and nothing else: the export is the root, and the path
    // the protocol is given never names the export itself.
    QCOMPARE(fs.pathFor(VfsUri::fromString(QStringLiteral("nfs://drive/"))), QStringLiteral("/holidays"));
    QCOMPARE(fs.pathFor(VfsUri::fromString(QStringLiteral("nfs://drive/2019/sunset.raw"))),
        QStringLiteral("/holidays/2019/sunset.raw"));

    // A drive rooted at the export itself is the export's own root, and that is
    // a single slash rather than an empty string -- which libnfs reads as a
    // relative path and refuses.
    NfsSettings atTheTop = settings;
    atTheTop.remoteRoot.clear();
    NfsFileSystem bare(QStringLiteral("nfs"), atTheTop);
    QCOMPARE(bare.pathFor(VfsUri::fromString(QStringLiteral("nfs://drive/"))), QStringLiteral("/"));
    QCOMPARE(bare.pathFor(VfsUri::fromString(QStringLiteral("nfs://drive/notes.txt"))),
        QStringLiteral("/notes.txt"));
}

void TestNfsFileSystem::twoDrivesInsideOneExportShareItsConnection()
{
    // Mounting costs two round trips, so what decides whether a connection can
    // be reused has to be the mount and not the drive: somebody with a drive per
    // project inside one export would otherwise pay for a mount per project, and
    // a directory walk would pay for one per directory.
    NfsSettings photos;
    photos.host = QStringLiteral("fileserver");
    photos.exportPath = QStringLiteral("/srv/media");
    photos.remoteRoot = QStringLiteral("/photos");

    NfsSettings videos = photos;
    videos.remoteRoot = QStringLiteral("/videos");
    QCOMPARE(photos.mountKey(), videos.mountKey());

    // What does decide it: the server, the export, and the ids being claimed --
    // a connection claiming to be somebody else is not this drive's connection.
    NfsSettings elsewhere = photos;
    elsewhere.exportPath = QStringLiteral("/srv/backup");
    QVERIFY(photos.mountKey() != elsewhere.mountKey());

    NfsSettings asSomebodyElse = photos;
    asSomebodyElse.uid = 1000;
    QVERIFY(photos.mountKey() != asSomebodyElse.mountKey());

    NfsSettings otherServer = photos;
    otherServer.host = QStringLiteral("nas");
    QVERIFY(photos.mountKey() != otherServer.mountKey());
}

void TestNfsFileSystem::anExportThatIsNotThereFailsRatherThanHangs()
{
    // Nothing here needs a server, which is the point: a drive pointed at a
    // machine that is not answering has to come back and say so. libnfs left to
    // itself waits for the kernel, and the difference between twenty seconds and
    // several minutes is the difference between an error and a hung window.
    NfsSettings nowhere;
    nowhere.host = QStringLiteral("127.0.0.1");
    nowhere.exportPath = QStringLiteral("/mole-no-such-export");
    NfsFileSystem fs(QStringLiteral("nfs"), nowhere);

    QElapsedTimer clock;
    clock.start();
    const Result<FileEntryList> listing
        = fs.list(VfsUri(QStringLiteral("nfs"), QString(), QStringLiteral("/")), CancelToken());
    QVERIFY(!listing.ok());
    QCOMPARE(listing.error().code, VfsError::NetworkError);
    QVERIFY2(!listing.error().message.isEmpty(), "a failure with no sentence is a failure nobody can act on");
    // Generous, because it is bounding a hang and not measuring a connection: the
    // patience the backend sets is twenty seconds, and a refused connection comes
    // back in microseconds.
    QVERIFY2(clock.elapsed() < 60000, qPrintable(QStringLiteral("took %1 ms").arg(clock.elapsed())));
}

void TestNfsFileSystem::itSatisfiesTheConformanceSuite()
{
    const Export target = exportFromEnvironment();
    if (!target.isConfigured()) {
        QSKIP("No NFS export in the environment; set MOLE_TEST_NFS_HOST and MOLE_TEST_NFS_EXPORT "
              "to run this against a real server.");
    }

    // The same catalogue every other backend answers, from this one's first day.
    NfsSettings settings = NfsFileSystemFactory::settingsFrom(target.asConfig());
    settings.remoteRoot = (target.base.isEmpty() ? QString() : target.base)
        + QStringLiteral("/mole-nfs-%1").arg(QCoreApplication::applicationPid());

    auto fileSystem = std::make_shared<NfsFileSystem>(QStringLiteral("nfs"), settings);

    // The working directory is made through the backend, which the conformance
    // suite then works inside. Seeding through the code under test is not ideal
    // and is unavoidable here: libnfs is the only NFS client this process has, so
    // a "raw" fixture would be the same library talking to the same export.
    const VfsUri root(QStringLiteral("nfs"), QString(), QStringLiteral("/"));
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
    // Nothing is left connected: the pool would otherwise hold sockets open past
    // the end of the suite, and a leak at the end of a test looks like a leak in
    // the code under test.
    NfsFileSystem::forgetPooledMounts();
}

MOLE_TEST_MAIN(TestNfsFileSystem)

#include "tst_NfsFileSystem.moc"
