#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/sets/FileSetStore.h"
#include "core/vfs/VersionGuard.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QBuffer>
#include <QDir>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

namespace {

QString earlier()
{
    return QStringLiteral("an-earlier-one");
}

/// A drive that does know what a version is, so the pass-through half of the
/// guard has something to be checked against.
///
/// The two backends that will really answer this are a filesystem keeping
/// snapshots (MOLE-200) and an object store keeping earlier objects (MOLE-201).
/// What is being held here is the boundary, not either of them.
class VersionedFileSystem final : public IFileSystem
{
public:
    QString scheme() const override { return QStringLiteral("mem"); }
    VfsCapabilities capabilities() const override { return VfsCapability::Read; }
    bool understandsVersions() const override { return true; }

    Result<FileEntryList> list(const VfsUri&, const CancelToken&) override { return FileEntryList(); }

    Result<FileEntry> stat(const VfsUri& target) override
    {
        FileEntry entry;
        entry.name = target.fileName();
        entry.uri = target;
        entry.size = contentsFor(target).size();
        return entry;
    }

    Result<std::unique_ptr<QIODevice>> openRead(
        const VfsUri& target, qint64, const CancelToken& = {}) override
    {
        auto buffer = std::make_unique<QBuffer>();
        buffer->setData(contentsFor(target));
        buffer->open(QIODevice::ReadOnly);
        return Result<std::unique_ptr<QIODevice>>(std::unique_ptr<QIODevice>(std::move(buffer)));
    }

private:
    static QByteArray contentsFor(const VfsUri& target)
    {
        return target.hasVersion() ? QByteArray("as it was") : QByteArray("as it is");
    }
};

} // namespace

class TestFileVersions : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::shared_ptr<MemoryFileSystem> m_fs;

    VfsUri current() const { return VfsUri::fromString(QStringLiteral("mem:///notes.txt")); }
    VfsUri versioned() const { return current().withVersion(earlier()); }

private slots:
    void init();
    void cleanup();

    void aDriveThatDoesNotDoVersionsRefusesOneRatherThanAnswering();
    void theRefusalSaysWhatWasAskedFor();
    void theSameDriveGoesOnAnsweringAboutTheFileAsItIs();
    void everyDoorIsGuardedRatherThanTheOnesSomebodyThoughtOf();
    void aDriveThatUnderstandsVersionsIsHandedTheUriUnchanged();
    void everyMountIsGuardedWithoutAnybodyAskingForIt();
    void aSetKeepsTheVersionItWasGivenAcrossASaveAndALoad();
};

void TestFileVersions::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    m_fs = std::make_shared<MemoryFileSystem>();
    m_fs->addFile(QStringLiteral("/notes.txt"), QByteArray("as it is"));
}

void TestFileVersions::cleanup()
{
    m_fs.reset();
    m_dir.reset();
}

/// The risk the whole ticket is about. A backend that ignored a token it did not
/// recognise would hand back the current file while the window says it is
/// showing an earlier one.
void TestFileVersions::aDriveThatDoesNotDoVersionsRefusesOneRatherThanAnswering()
{
    QVERIFY(!m_fs->understandsVersions());
    const FileSystemPtr guarded = withVersionGuard(m_fs);

    const Result<std::unique_ptr<QIODevice>> read = guarded->openRead(versioned());
    QVERIFY2(!read.ok(), "the current file's bytes must not be handed over for a version");
    QCOMPARE(read.error().code, VfsError::NotSupported);
}

void TestFileVersions::theRefusalSaysWhatWasAskedFor()
{
    const FileSystemPtr guarded = withVersionGuard(m_fs);
    const QString message = guarded->stat(versioned()).error().message;

    // Somebody looking at a bookmark that has stopped working cannot tell from
    // its name that it points at a state of the file rather than the file.
    QVERIFY2(message.contains(earlier()), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("mem:///notes.txt")), qPrintable(message));
}

void TestFileVersions::theSameDriveGoesOnAnsweringAboutTheFileAsItIs()
{
    const FileSystemPtr guarded = withVersionGuard(m_fs);

    const Result<FileEntry> stat = guarded->stat(current());
    QVERIFY2(stat.ok(), qPrintable(stat.error().message));
    QCOMPARE(stat.value().size, 8);

    const Result<std::unique_ptr<QIODevice>> read = guarded->openRead(current());
    QVERIFY(read.ok());
    QCOMPARE(read.value()->readAll(), QByteArray("as it is"));
}

/// A guard on the doors somebody remembered is a guard on the doors somebody
/// remembered. Written once and applied to the drive, rather than to each of the
/// ninety methods across the backends in the tree.
void TestFileVersions::everyDoorIsGuardedRatherThanTheOnesSomebodyThoughtOf()
{
    const FileSystemPtr fs = withVersionGuard(m_fs);
    const VfsUri target = versioned();
    const CancelToken noCancel;

    QCOMPARE(fs->stat(target).error().code, VfsError::NotSupported);
    QCOMPARE(fs->list(target, noCancel).error().code, VfsError::NotSupported);
    QCOMPARE(fs->openRead(target).error().code, VfsError::NotSupported);
    QCOMPARE(fs->openWrite(target).error().code, VfsError::NotSupported);
    QCOMPARE(fs->makeDirectory(target).error().code, VfsError::NotSupported);
    QCOMPARE(fs->remove(target, false).error().code, VfsError::NotSupported);
    QCOMPARE(fs->rename(target, current()).error().code, VfsError::NotSupported);
    QCOMPARE(fs->rename(current(), target).error().code, VfsError::NotSupported);
    QCOMPARE(fs->space(target).error().code, VfsError::NotSupported);
    QCOMPARE(fs->access(target).error().code, VfsError::NotSupported);
    QCOMPARE(fs->search(target, QStringLiteral("x"), noCancel).error().code, VfsError::NotSupported);
    QCOMPARE(
        fs->invoke(QStringLiteral("org.mole.test.x"), target, noCancel).error().code, VfsError::NotSupported);
    // Nothing rather than a refusal: there is nowhere in a list of actions to
    // put an error.
    QVERIFY(fs->actionsFor(target, FileEntry {}).isEmpty());
}

void TestFileVersions::aDriveThatUnderstandsVersionsIsHandedTheUriUnchanged()
{
    const FileSystemPtr guarded = withVersionGuard(std::make_shared<VersionedFileSystem>());
    QVERIFY(guarded->understandsVersions());

    const Result<std::unique_ptr<QIODevice>> earlierRead = guarded->openRead(versioned());
    QVERIFY2(earlierRead.ok(), qPrintable(earlierRead.error().message));
    QCOMPARE(earlierRead.value()->readAll(), QByteArray("as it was"));

    const Result<std::unique_ptr<QIODevice>> now = guarded->openRead(current());
    QVERIFY(now.ok());
    QCOMPARE(now.value()->readAll(), QByteArray("as it is"));
}

/// The guard has to be on the drive as it is reached, not on one a caller
/// remembered to wrap: nothing in Mole holds a backend it did not resolve.
void TestFileVersions::everyMountIsGuardedWithoutAnybodyAskingForIt()
{
    VfsManager vfs;
    Mount mount;
    mount.displayName = QStringLiteral("scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = m_fs;
    QVERIFY(!vfs.addMount(mount).isEmpty());

    const FileSystemPtr resolved = vfs.resolve(current());
    QVERIFY(resolved);
    QCOMPARE(resolved->stat(versioned()).error().code, VfsError::NotSupported);
    QVERIFY2(resolved->stat(current()).ok(), "and the file itself is still reachable through it");
}

/// Written down and read back, through something that really writes it down.
void TestFileVersions::aSetKeepsTheVersionItWasGivenAcrossASaveAndALoad()
{
    const QString path = QDir(m_dir->path()).filePath(QStringLiteral("sets.json"));
    const QString member = versioned().toString();

    {
        FileSetStore store(path);
        const FileSet set = store.create(QStringLiteral("Before the rewrite"), { member });
        QVERIFY(set.isValid());
        QVERIFY(store.save());
    }

    FileSetStore reopened(path);
    QVERIFY(reopened.load());
    QCOMPARE(reopened.sets().size(), 1);
    QCOMPARE(reopened.sets().first().uris, QList<QString> { member });

    const QList<VfsUri> targets = reopened.sets().first().targets();
    QCOMPARE(targets.size(), 1);
    QCOMPARE(targets.first(), versioned());
    QCOMPARE(targets.first().version(), earlier());
}

MOLE_TEST_MAIN(TestFileVersions)
#include "tst_FileVersions.moc"
