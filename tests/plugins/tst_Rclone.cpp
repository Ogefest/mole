#include "plugins/rclone/RcloneFactory.h"
#include "plugins/rclone/RcloneFileSystem.h"
#include "plugins/rclone/RcloneLibrary.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// rclone as a library. The conformance suite does the heavy lifting; these
/// tests are about the things specific to reaching a remote through it.
class TestRclone : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void reportsWhetherTheLibraryIsThere();
    void offersEveryBackendRcloneHas();
    void buildsFormsFromRcloneItself();

    void quotesValuesThatWouldBreakTheConnectionString();
    void leavesOrdinaryValuesAlone();
    void ignoresTheHostsOwnBookkeepingKeys();
    void producesAStableStringForTheSameConfig();

    void behavesLikeAFileSystem();
    void reportsAMissingFileAsNotFound();

private:
    FileSystemPtr mountLocalThroughRclone(const QString& path);

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestRclone::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void TestRclone::cleanup()
{
    m_dir.reset();
}

FileSystemPtr TestRclone::mountLocalThroughRclone(const QString& path)
{
    // rclone's "local" backend, reached the same way every remote is: through a
    // connection string, with no entry in rclone.conf. It stands in for a real
    // remote here because the code path is identical and a test must not need
    // credentials to somebody's cloud account.
    RcloneFactory factory;
    QVariantMap config;
    config.insert(IFileSystemFactory::variantKey(), QStringLiteral("local"));
    config.insert(QStringLiteral("__root"), path);
    config.insert(QStringLiteral("__scheme"), QStringLiteral("rtest"));

    QString error;
    return factory.create(config, &error);
}

void TestRclone::reportsWhetherTheLibraryIsThere()
{
    RcloneFactory factory;
    if (factory.isAvailable()) {
        // Loaded in-process: no binary on PATH, no daemon, no port.
        QVERIFY(!RcloneLibrary::instance().version().isEmpty());
    } else {
        // And when it is not, it says so rather than offering drives that
        // cannot possibly connect.
        QVERIFY(!factory.unavailableReason().isEmpty());
        QVERIFY(factory.variants().isEmpty());
    }
}

void TestRclone::offersEveryBackendRcloneHas()
{
    RcloneFactory factory;
    if (!factory.isAvailable())
        QSKIP("librclone is not built; run `make librclone`");

    const QList<BackendVariant> variants = factory.variants();
    QVERIFY2(variants.size() > 30, "rclone knows dozens of backends and all of them should appear");

    const auto has = [&variants](const QString& id) {
        return std::any_of(variants.begin(), variants.end(),
            [&id](const BackendVariant& variant) { return variant.id == id; });
    };
    QVERIFY(has(QStringLiteral("s3")));
    QVERIFY(has(QStringLiteral("sftp")));
    QVERIFY(has(QStringLiteral("webdav")));
    QVERIFY(has(QStringLiteral("drive")));

    // "local" is the disk this application already reads directly, and going
    // through rclone to reach it would be slower for nothing.
    QVERIFY(!has(QStringLiteral("local")));
}

void TestRclone::buildsFormsFromRcloneItself()
{
    RcloneFactory factory;
    if (!factory.isAvailable())
        QSKIP("librclone is not built; run `make librclone`");

    const QList<BackendVariant> variants = factory.variants();
    const auto find = [&variants](const QString& id) {
        for (const BackendVariant& variant : variants) {
            if (variant.id == id)
                return variant;
        }
        return BackendVariant {};
    };

    const BackendVariant sftp = find(QStringLiteral("sftp"));
    QVERIFY(!sftp.fields.isEmpty());

    const auto field = [&sftp](const QString& key) {
        for (const ConnectionField& candidate : sftp.fields) {
            if (candidate.key == key)
                return candidate;
        }
        return ConnectionField {};
    };

    // A password is marked as one, which is what sends it to the encrypted
    // store instead of the settings file.
    QCOMPARE(field(QStringLiteral("pass")).kind, ConnectionField::Password);
    QVERIFY(!field(QStringLiteral("host")).help.isEmpty());

    // S3 asks completely different questions per provider, and rclone says so.
    // Without that, a form would show eighty fields at once.
    const BackendVariant s3 = find(QStringLiteral("s3"));
    const bool hasConditionalFields = std::any_of(s3.fields.begin(), s3.fields.end(),
        [](const ConnectionField& candidate) { return !candidate.dependsOnKey.isEmpty(); });
    QVERIFY2(hasConditionalFields, "S3's provider-specific options must be marked as such");

    // And several fields are advanced, or the form would be unusable.
    const bool hasAdvanced = std::any_of(s3.fields.begin(), s3.fields.end(),
        [](const ConnectionField& candidate) { return candidate.advanced; });
    QVERIFY(hasAdvanced);
}

void TestRclone::quotesValuesThatWouldBreakTheConnectionString()
{
    QVariantMap config;
    config.insert(QStringLiteral("pass"), QStringLiteral("has,comma"));

    const QString built = RcloneFactory::connectionStringFor(QStringLiteral("sftp"), config);
    // An unquoted comma would end the parameter list early -- with a password,
    // that means the rest of it becomes a different option and the connection
    // goes somewhere it should not.
    QCOMPARE(built, QStringLiteral(":sftp,pass=\"has,comma\":"));

    config.clear();
    config.insert(QStringLiteral("pass"), QStringLiteral("quote\"inside"));
    QCOMPARE(RcloneFactory::connectionStringFor(QStringLiteral("sftp"), config),
        QStringLiteral(":sftp,pass=\"quote\"\"inside\":"));
}

void TestRclone::leavesOrdinaryValuesAlone()
{
    QVariantMap config;
    config.insert(QStringLiteral("host"), QStringLiteral("example.org"));
    config.insert(QStringLiteral("user"), QStringLiteral("ada"));

    QCOMPARE(RcloneFactory::connectionStringFor(QStringLiteral("sftp"), config),
        QStringLiteral(":sftp,host=example.org,user=ada:"));
}

void TestRclone::ignoresTheHostsOwnBookkeepingKeys()
{
    QVariantMap config;
    config.insert(QStringLiteral("host"), QStringLiteral("example.org"));
    config.insert(QStringLiteral("__root"), QStringLiteral("/data"));
    config.insert(QStringLiteral("__scheme"), QStringLiteral("nas"));
    config.insert(IFileSystemFactory::variantKey(), QStringLiteral("sftp"));

    // Keys this application adds for its own purposes are not rclone options,
    // and passing them on would make rclone reject the whole connection.
    QCOMPARE(RcloneFactory::connectionStringFor(QStringLiteral("sftp"), config),
        QStringLiteral(":sftp,host=example.org:"));
}

void TestRclone::producesAStableStringForTheSameConfig()
{
    QVariantMap first;
    first.insert(QStringLiteral("user"), QStringLiteral("ada"));
    first.insert(QStringLiteral("host"), QStringLiteral("example.org"));

    QVariantMap second;
    second.insert(QStringLiteral("host"), QStringLiteral("example.org"));
    second.insert(QStringLiteral("user"), QStringLiteral("ada"));

    // The same configuration always produces the same string, whatever order it
    // was built in -- which is what makes it comparable and cacheable.
    QCOMPARE(RcloneFactory::connectionStringFor(QStringLiteral("sftp"), first),
        RcloneFactory::connectionStringFor(QStringLiteral("sftp"), second));
}

void TestRclone::behavesLikeAFileSystem()
{
    RcloneFactory factory;
    if (!factory.isAvailable())
        QSKIP("librclone is not built; run `make librclone`");

    FileSystemPtr fs = mountLocalThroughRclone(m_dir->path());
    QVERIFY(fs);

    // The same suite every backend passes. A remote that disagrees with the
    // local disk about what NotFound means would break the layers above it in
    // ways nobody could trace back to here.
    ConformanceContext context;
    context.fileSystem = fs;
    context.root = VfsUri::fromString(QStringLiteral("rtest://remote/"));
    // Seeded outside the backend, so the suite can test reading things it did
    // not write -- which is the case a read-only remote is all about.
    const QString base = m_dir->path();
    context.seedFile = [base](const QString& relative, const QByteArray& contents) {
        const QString path = QDir(base).filePath(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        return file.write(contents) == contents.size();
    };
    context.seedDir
        = [base](const QString& relative) { return QDir().mkpath(QDir(base).filePath(relative)); };
    runFileSystemConformance(context);
}

void TestRclone::reportsAMissingFileAsNotFound()
{
    RcloneFactory factory;
    if (!factory.isAvailable())
        QSKIP("librclone is not built; run `make librclone`");

    FileSystemPtr fs = mountLocalThroughRclone(m_dir->path());
    QVERIFY(fs);

    Result<FileEntry> missing = fs->stat(VfsUri::fromString(QStringLiteral("rtest://remote/nothing.txt")));
    QVERIFY(!missing.ok());
    // Mapped to this application's own code, not left as rclone's wording --
    // the layers above act on the code.
    QCOMPARE(missing.error().code, VfsError::NotFound);
}

MOLE_TEST_MAIN(TestRclone)
#include "tst_Rclone.moc"
