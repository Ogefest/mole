#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/OfferingFileSystem.h"

#include "core/diagnostics/LoggingFileSystem.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

using namespace mole;
using namespace mole::test;

class TestFileActions : public QObject
{
    Q_OBJECT

private slots:
    void aDriveThatContributesNothingRefusesToBeAsked();
    void anOutcomeMustCarryWhatItsKindPromises();
    void conformanceOnADriveThatContributesActions();
    void anActionSeesTheCancelToken();
    void theLogWrapperDoesNotSwallowWhatADriveOffers();
};

/// Which is every backend in the tree, and the reason none of them needed an
/// edit: the default offers nothing, and a drive that offers nothing must not
/// perform anything either. Nothing will ever ask it -- but a drive that
/// answered anyway is how a feature comes to work by accident on one backend.
void TestFileActions::aDriveThatContributesNothingRefusesToBeAsked()
{
    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/notes.txt"), QByteArray("x"));
    const VfsUri target = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));
    const Result<FileEntry> entry = memory->stat(target);
    QVERIFY(entry.ok());

    QVERIFY(memory->actionsFor(target, entry.value()).isEmpty());

    const Result<FileActionOutcome> refused
        = memory->invoke(QStringLiteral("org.mole.test.link"), target, CancelToken());
    QVERIFY(!refused.ok());
    QCOMPARE(refused.error().code, VfsError::NotSupported);
    // The id is in the message: an id arriving at a drive that never offered it
    // is the shell and the drive disagreeing, and the id says about what.
    QVERIFY(refused.error().message.contains(QStringLiteral("org.mole.test.link")));

    LocalFileSystem disk;
    QVERIFY(disk.actionsFor(VfsUri::fromLocalPath(QDir::tempPath()), FileEntry {}).isEmpty());
    QCOMPARE(disk.invoke(QStringLiteral("org.mole.test.link"), VfsUri::fromLocalPath(QDir::tempPath()),
                     CancelToken())
                 .error()
                 .code,
        VfsError::NotSupported);
}

/// The rule the conformance suite holds every contributing drive to, stated on
/// its own. An outcome is text or it is uris; one that says "text" and carries
/// none is neither kind, and there is nothing for the interface to show.
void TestFileActions::anOutcomeMustCarryWhatItsKindPromises()
{
    QVERIFY(!FileActionOutcome().isValid());
    QVERIFY(!FileActionOutcome::fromText(QString()).isValid());
    QVERIFY(!FileActionOutcome::fromUris({}).isValid());

    QVERIFY(FileActionOutcome::fromText(QStringLiteral("https://example.invalid/x")).isValid());
    QVERIFY(FileActionOutcome::fromUris({ VfsUri::fromString(QStringLiteral("mem:///a.1")) }).isValid());

    // An answer that goes stale says when, and one that does not says nothing
    // rather than a date somebody would read as fact.
    QVERIFY(!FileActionOutcome::fromUris({ VfsUri::fromString(QStringLiteral("mem:///a.1")) })
                 .validUntil.isValid());
}

/// The other direction of the same suite: a drive that does offer something has
/// to be able to perform it, and has to refuse an id it never handed out.
void TestFileActions::conformanceOnADriveThatContributesActions()
{
    auto fs = std::make_shared<OfferingFileSystem>();

    ConformanceContext context;
    context.fileSystem = fs;
    context.root = VfsUri::fromString(QStringLiteral("mem:///"));
    context.seedFile = [fs](const QString& path, const QByteArray& data) {
        fs->memory()->addFile(QLatin1Char('/') + path, data);
        return true;
    };
    context.seedDir = [fs](const QString& path) {
        fs->memory()->addDirectory(QLatin1Char('/') + path);
        return true;
    };

    runFileSystemConformance(context);
}

void TestFileActions::anActionSeesTheCancelToken()
{
    auto fs = std::make_shared<OfferingFileSystem>();
    fs->memory()->addFile(QStringLiteral("/notes.txt"), QByteArray("x"));
    const VfsUri target = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));

    CancelToken cancelled;
    cancelled.cancel();

    const Result<FileActionOutcome> stopped = fs->invoke(OfferingFileSystem::linkAction(), target, cancelled);
    QVERIFY(!stopped.ok());
    QCOMPARE(stopped.error().code, VfsError::Cancelled);
}

/// Every mount goes through the log wrapper on its way into VfsManager, so a
/// wrapper that did not forward these would make the whole extension point
/// unreachable in the running application while every test of it stayed green.
void TestFileActions::theLogWrapperDoesNotSwallowWhatADriveOffers()
{
    auto fs = std::make_shared<OfferingFileSystem>();
    fs->memory()->addFile(QStringLiteral("/notes.txt"), QByteArray("x"));
    fs->addVersion(QStringLiteral("/notes.txt"), QStringLiteral("v1"), QByteArray("older"));
    fs->addVersion(QStringLiteral("/notes.txt"), QStringLiteral("v2"), QByteArray("old"));
    const FileSystemPtr wrapped = withLogging(fs, QStringLiteral("scratch"));

    const VfsUri target = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));
    const Result<FileEntry> entry = wrapped->stat(target);
    QVERIFY(entry.ok());

    const FileActionList actions = wrapped->actionsFor(target, entry.value());
    QCOMPARE(actions.size(), 2);
    QCOMPARE(actions.first().id, OfferingFileSystem::linkAction());

    const Result<FileActionOutcome> outcome
        = wrapped->invoke(OfferingFileSystem::versionsAction(), target, CancelToken());
    QVERIFY2(outcome.ok(), qPrintable(outcome.error().message));
    QCOMPARE(outcome.value().kind, FileActionOutcome::Kind::Uris);
    QCOMPARE(outcome.value().uris.size(), 2);

    // And a refusal comes back as a refusal rather than as an empty answer.
    QCOMPARE(wrapped->invoke(QStringLiteral("org.mole.test.absent"), target, CancelToken()).error().code,
        VfsError::NotSupported);
}

MOLE_TEST_MAIN(TestFileActions)
#include "tst_FileActions.moc"
