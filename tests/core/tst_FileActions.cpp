#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"

#include "core/diagnostics/LoggingFileSystem.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

using namespace mole;
using namespace mole::test;

namespace {

QString linkActionId()
{
    return QStringLiteral("org.mole.test.link");
}
QString versionsActionId()
{
    return QStringLiteral("org.mole.test.versions");
}

/// A drive that contributes actions, wrapped around one that does not.
///
/// Everything else is the memory backend underneath, so it answers the whole
/// conformance suite while answering the two new questions on its own -- which
/// is the only way to run the offering direction of that suite at all, since no
/// backend in the tree offers anything yet.
///
/// Deliberately small. The fake a test of the *interface* will want -- alternate
/// versions that are really readable, a different answer per path, a query slow
/// enough to cancel -- is MOLE-196, and belongs in tests/support/ when it
/// arrives rather than here.
class ActingFileSystem final : public IFileSystem
{
public:
    explicit ActingFileSystem(std::shared_ptr<MemoryFileSystem> inner)
        : m_inner(std::move(inner))
    {
    }

    // ---- the two this suite is about -------------------------------------

    FileActionList actionsFor(const VfsUri& target, const FileEntry& entry) override
    {
        if (entry.isDir || !target.isValid())
            return {};
        return { FileAction { linkActionId(), QStringLiteral("Copy a temporary link"), true },
            FileAction { versionsActionId(), QStringLiteral("Earlier versions"), true } };
    }

    Result<FileActionOutcome> invoke(
        const QString& id, const VfsUri& target, const CancelToken& cancel) override
    {
        // Every call into a drive polls the token, and an action is a call into
        // a drive: signing a link is a round trip, and listing what a container
        // kept can be several.
        if (cancel.isCancelled())
            return VfsError::make(VfsError::Cancelled, QStringLiteral("cancelled"));

        if (id == linkActionId()) {
            return FileActionOutcome::fromText(QStringLiteral("https://example.invalid/") + target.fileName(),
                QDateTime::fromSecsSinceEpoch(1'000'000));
        }
        if (id == versionsActionId()) {
            return FileActionOutcome::fromUris(
                { target.parent().child(target.fileName() + QStringLiteral(".1")),
                    target.parent().child(target.fileName() + QStringLiteral(".2")) });
        }
        return IFileSystem::invoke(id, target, cancel);
    }

    // ---- everything else is the drive underneath --------------------------

    QString scheme() const override { return m_inner->scheme(); }
    VfsCapabilities capabilities() const override { return m_inner->capabilities(); }
    Qt::CaseSensitivity pathCaseSensitivity() const override { return m_inner->pathCaseSensitivity(); }
    NameRules nameRules() const override { return m_inner->nameRules(); }

    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override
    {
        return m_inner->list(dir, cancel);
    }
    Result<FileEntry> stat(const VfsUri& target) override { return m_inner->stat(target); }
    Result<void> makeDirectory(const VfsUri& target) override { return m_inner->makeDirectory(target); }
    Result<void> remove(const VfsUri& target, bool recursive) override
    {
        return m_inner->remove(target, recursive);
    }
    Result<void> rename(const VfsUri& from, const VfsUri& to) override { return m_inner->rename(from, to); }
    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override
    {
        return m_inner->openRead(target, expectedSize);
    }
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override
    {
        return m_inner->openWrite(target, expectedSize);
    }

private:
    std::shared_ptr<MemoryFileSystem> m_inner;
};

} // namespace

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
    auto memory = std::make_shared<MemoryFileSystem>();
    auto fs = std::make_shared<ActingFileSystem>(memory);

    ConformanceContext context;
    context.fileSystem = fs;
    context.root = VfsUri::fromString(QStringLiteral("mem:///"));
    context.seedFile = [memory](const QString& path, const QByteArray& data) {
        memory->addFile(QLatin1Char('/') + path, data);
        return true;
    };
    context.seedDir = [memory](const QString& path) {
        memory->addDirectory(QLatin1Char('/') + path);
        return true;
    };

    runFileSystemConformance(context);
}

void TestFileActions::anActionSeesTheCancelToken()
{
    auto fs = std::make_shared<ActingFileSystem>(std::make_shared<MemoryFileSystem>());
    const VfsUri target = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));

    CancelToken cancelled;
    cancelled.cancel();

    const Result<FileActionOutcome> stopped = fs->invoke(linkActionId(), target, cancelled);
    QVERIFY(!stopped.ok());
    QCOMPARE(stopped.error().code, VfsError::Cancelled);
}

/// Every mount goes through the log wrapper on its way into VfsManager, so a
/// wrapper that did not forward these would make the whole extension point
/// unreachable in the running application while every test of it stayed green.
void TestFileActions::theLogWrapperDoesNotSwallowWhatADriveOffers()
{
    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/notes.txt"), QByteArray("x"));
    const FileSystemPtr wrapped
        = withLogging(std::make_shared<ActingFileSystem>(memory), QStringLiteral("scratch"));

    const VfsUri target = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));
    const Result<FileEntry> entry = wrapped->stat(target);
    QVERIFY(entry.ok());

    const FileActionList actions = wrapped->actionsFor(target, entry.value());
    QCOMPARE(actions.size(), 2);
    QCOMPARE(actions.first().id, linkActionId());

    const Result<FileActionOutcome> outcome = wrapped->invoke(versionsActionId(), target, CancelToken());
    QVERIFY2(outcome.ok(), qPrintable(outcome.error().message));
    QCOMPARE(outcome.value().kind, FileActionOutcome::Kind::Uris);
    QCOMPARE(outcome.value().uris.size(), 2);

    // And a refusal comes back as a refusal rather than as an empty answer.
    QCOMPARE(wrapped->invoke(QStringLiteral("org.mole.test.absent"), target, CancelToken()).error().code,
        VfsError::NotSupported);
}

MOLE_TEST_MAIN(TestFileActions)
#include "tst_FileActions.moc"
