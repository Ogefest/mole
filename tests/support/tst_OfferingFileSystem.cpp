#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/OfferingFileSystem.h"
#include "support/TestSupport.h"

#include "core/tasks/ProbeDriveTask.h"
#include "core/tasks/TaskManager.h"

#include <thread>

using namespace mole;
using namespace mole::test;

class TestOfferingFileSystem : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<OfferingFileSystem> m_fs;

    VfsUri uriOf(const QString& path) const { return VfsUri::fromString(QStringLiteral("mem://") + path); }
    FileEntry entryOf(const QString& path) const;

private slots:
    void init();
    void cleanup();

    void conformance();
    void aDriveWithNothingToOfferForAPathOffersNothingForIt();
    void bothOutcomeKindsComeOutOfOneDrive();
    void anEarlierVersionIsAnOrdinaryFileThatReadsBackItsOwnContents();
    void whichVersionWasOpenedIsAssertable();
    void whatIsOfferedIsSettablePerPath();
    void aDirectoryAndAnEarlierVersionAreOfferedNothing();
    void anActionThatIsSlowCanBeCancelled();
    void theProbeAnswersWithBothOfThem();
};

FileEntry TestOfferingFileSystem::entryOf(const QString& path) const
{
    const Result<FileEntry> stat = m_fs->stat(uriOf(path));
    return stat.ok() ? stat.value() : FileEntry {};
}

void TestOfferingFileSystem::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<OfferingFileSystem>();
    m_fs->memory()->addFile(QStringLiteral("/report.txt"), QByteArray("the third draft"));
    m_fs->memory()->addFile(QStringLiteral("/untouched.txt"), QByteArray("never edited"));
    m_fs->memory()->addDirectory(QStringLiteral("/folder"));

    m_fs->addVersion(QStringLiteral("/report.txt"), QStringLiteral("v1"), QByteArray("the first draft"));
    m_fs->addVersion(QStringLiteral("/report.txt"), QStringLiteral("v2"), QByteArray("the second draft"));
}

void TestOfferingFileSystem::cleanup()
{
    // See MOLE-273.
    m_tasks.reset();
    m_fs.reset();
}

/// Held to the same contract as any backend. A fake that disagrees with the disk
/// about what NotFound means makes every suite built on it green for the wrong
/// reason -- which is the argument tst_FaultyFileSystem already makes.
void TestOfferingFileSystem::conformance()
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

void TestOfferingFileSystem::aDriveWithNothingToOfferForAPathOffersNothingForIt()
{
    m_fs->setLinkable(QStringLiteral("/untouched.txt"), false);
    QVERIFY(
        m_fs->actionsFor(uriOf(QStringLiteral("/untouched.txt")), entryOf(QStringLiteral("/untouched.txt")))
            .isEmpty());
}

void TestOfferingFileSystem::bothOutcomeKindsComeOutOfOneDrive()
{
    const VfsUri report = uriOf(QStringLiteral("/report.txt"));
    const FileActionList actions = m_fs->actionsFor(report, entryOf(QStringLiteral("/report.txt")));
    QCOMPARE(actions.size(), 2);

    const Result<FileActionOutcome> text
        = m_fs->invoke(OfferingFileSystem::linkAction(), report, CancelToken());
    QVERIFY2(text.ok(), qPrintable(text.error().message));
    QCOMPARE(text.value().kind, FileActionOutcome::Kind::Text);
    QVERIFY(text.value().isValid());
    QVERIFY2(text.value().validUntil.isValid(), "a link that expires has to say when");

    const Result<FileActionOutcome> uris
        = m_fs->invoke(OfferingFileSystem::versionsAction(), report, CancelToken());
    QVERIFY2(uris.ok(), qPrintable(uris.error().message));
    QCOMPARE(uris.value().kind, FileActionOutcome::Kind::Uris);
    QCOMPARE(uris.value().uris.size(), 2);
}

void TestOfferingFileSystem::anEarlierVersionIsAnOrdinaryFileThatReadsBackItsOwnContents()
{
    const VfsUri earlier = uriOf(QStringLiteral("/report.txt")).withVersion(QStringLiteral("v1"));

    const Result<FileEntry> stat = m_fs->stat(earlier);
    QVERIFY2(stat.ok(), qPrintable(stat.error().message));
    QCOMPARE(stat.value().name, QStringLiteral("report.txt"));
    QCOMPARE(stat.value().size, 15);

    const Result<std::unique_ptr<QIODevice>> read = m_fs->openRead(earlier);
    QVERIFY2(read.ok(), qPrintable(read.error().message));
    QCOMPARE(read.value()->readAll(), QByteArray("the first draft"));

    // A version nobody seeded is missing rather than the current file.
    QCOMPARE(
        m_fs->openRead(uriOf(QStringLiteral("/report.txt")).withVersion(QStringLiteral("v9"))).error().code,
        VfsError::NotFound);
}

/// The reason each version has contents of its own. A test of the interface has
/// to be able to say *which* one was opened, not that something opened.
void TestOfferingFileSystem::whichVersionWasOpenedIsAssertable()
{
    const Result<FileActionOutcome> outcome = m_fs->invoke(
        OfferingFileSystem::versionsAction(), uriOf(QStringLiteral("/report.txt")), CancelToken());
    QVERIFY(outcome.ok());

    QStringList read;
    for (const VfsUri& uri : outcome.value().uris) {
        const Result<std::unique_ptr<QIODevice>> stream = m_fs->openRead(uri);
        QVERIFY2(stream.ok(), qPrintable(uri.toString()));
        read.append(QString::fromUtf8(stream.value()->readAll()));
    }

    QCOMPARE(read, QStringList({ QStringLiteral("the first draft"), QStringLiteral("the second draft") }));
    QVERIFY2(read.at(0) != QString::fromUtf8("the third draft"),
        "an earlier version that reads back as the current file proves nothing");
}

/// The case the row markers actually have to get right: a folder where some
/// entries have something to offer and some do not.
void TestOfferingFileSystem::whatIsOfferedIsSettablePerPath()
{
    m_fs->setLinkable(QStringLiteral("/untouched.txt"), false);

    const FileActionList reportActions
        = m_fs->actionsFor(uriOf(QStringLiteral("/report.txt")), entryOf(QStringLiteral("/report.txt")));
    const FileActionList untouchedActions = m_fs->actionsFor(
        uriOf(QStringLiteral("/untouched.txt")), entryOf(QStringLiteral("/untouched.txt")));

    QCOMPARE(reportActions.size(), 2);
    QVERIFY(untouchedActions.isEmpty());

    // And a file with a link but nothing earlier gets one of the two.
    m_fs->setLinkable(QStringLiteral("/untouched.txt"), true);
    QCOMPARE(
        m_fs->actionsFor(uriOf(QStringLiteral("/untouched.txt")), entryOf(QStringLiteral("/untouched.txt")))
            .size(),
        1);
}

void TestOfferingFileSystem::aDirectoryAndAnEarlierVersionAreOfferedNothing()
{
    QVERIFY(m_fs->actionsFor(uriOf(QStringLiteral("/folder")), entryOf(QStringLiteral("/folder"))).isEmpty());

    const VfsUri earlier = uriOf(QStringLiteral("/report.txt")).withVersion(QStringLiteral("v1"));
    QVERIFY2(m_fs->actionsFor(earlier, entryOf(QStringLiteral("/report.txt"))).isEmpty(),
        "what is on offer is about the file as it is");
}

/// The query runs on a worker thread and has to honour its token like every
/// other call into storage.
void TestOfferingFileSystem::anActionThatIsSlowCanBeCancelled()
{
    m_fs->setActionDelayMs(60000);

    CancelToken cancel;
    Result<FileActionOutcome> outcome = FileActionOutcome::fromText(QStringLiteral("not yet"));
    std::thread worker([this, &cancel, &outcome] {
        outcome
            = m_fs->invoke(OfferingFileSystem::linkAction(), uriOf(QStringLiteral("/report.txt")), cancel);
    });

    // Waited for, never slept past: the cancel has to arrive while the drive is
    // inside the call, which is the whole point.
    QVERIFY(waitFor([this] { return m_fs->isWorking(); }));
    cancel.cancel();
    worker.join();

    QVERIFY(!outcome.ok());
    QCOMPARE(outcome.error().code, VfsError::Cancelled);
}

void TestOfferingFileSystem::theProbeAnswersWithBothOfThem()
{
    auto* task = new ProbeDriveTask(m_fs, VfsUri::fromString(QStringLiteral("mem:///")));
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    const DriveOffers offers = m_fs->offers();
    QVERIFY(offers.isKnown());
    QVERIFY(offers.has(OfferingFileSystem::linkAction()));
    QVERIFY(offers.has(OfferingFileSystem::versionsAction()));
}

MOLE_TEST_MAIN(TestOfferingFileSystem)
#include "tst_OfferingFileSystem.moc"
