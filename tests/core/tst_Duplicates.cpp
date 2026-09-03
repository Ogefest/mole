#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/CoreMetaTypes.h"
#include "core/duplicates/ContentComparison.h"
#include "core/duplicates/FindDuplicatesTask.h"
#include "core/duplicates/Strategies.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QFile>
#include <QMutex>
#include <QSemaphore>
#include <QSet>

#include <atomic>

using namespace mole;
using namespace mole::test;

/// Finding duplicates: what each strategy calls a match, and the staged narrowing
/// that makes the expensive one affordable.
class TestDuplicates : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void everyStrategyDescribesItself();

    void sameSizeGroupsBySizeAlone();
    void sameNameFindsCopiesThatWereEditedApart();
    void sameNameAndSizeIsLessNoisyThanEither();
    void identicalContentsProvesIt();
    void identicalContentsSeparatesFilesSharingAHeader();

    void ignoresEmptyFiles();
    void honoursAMinimumSize();
    void reportsWhatCouldBeFreed();
    void groupsAreOrderedByTheSavingTheyOffer();
    void aTreeWithNoDuplicatesReportsNone();
    void theExpensiveStageOnlySeesWhatSurvivedTheCheapOnes();
    void aFileThatChangesWhileItIsBeingComparedIsLeftOutOfEveryGroup();
    void filesSharingALongHeaderAreSeparatedWithoutReadingThemWhole();

    void contentsAreProvedByComparisonRatherThanByADigest();
    void aBucketLargerThanTheOpenLimitIsStillOneGroup();
    void aFileAloneInItsSliceIsNotLost();
    void nothingIsEverHeldWholeAndNothingBeyondTheOpenLimitIsEverOpen();
    void theAnswerIsTheSameHoweverManyThreadsRead();

    void groupsArriveAsTheyAreConfirmedRatherThanAllAtTheEnd();
    void aGroupIsNeverAnnouncedAndThenTakenBack();
    void theListIsInOrderAtEveryInstantAndNotOnlyAtTheEnd();
    void aScanStoppedPartWayKeepsWhatItHadAlreadyConfirmed();
    void aBurstOfConfirmationsIsBoundedByTheDrainAndNotByTheGroupCount();
    void aGroupReachesTheWindowWhileTheScanIsStillStandingInConfirm();

    void aRootInsideAnotherRootDoesNotMakeEveryFileItsOwnDuplicate();
    void aSymbolicLinkIsNotADuplicateOfWhatItPointsAt();
    void aSubtreeThatCouldNotBeReadIsCountedRatherThanPassedOver();
    void twoSpellingsOfOneNameLandInOneBucket();

private:
    QList<DuplicateGroup> find(std::unique_ptr<IDuplicateStrategy> strategy, qint64 minimumSize = 1);
    /// The same, over roots this case chooses, and with the task kept so its
    /// counts can be read.
    FindDuplicatesTask* findUnder(
        const QList<VfsUri>& roots, std::unique_ptr<IDuplicateStrategy> strategy, qint64 minimumSize = 1);

    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<TaskManager> m_tasks;
};

void TestDuplicates::init()
{
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());

    m_vfs = std::make_unique<VfsManager>();
    Mount mount;
    mount.id = QStringLiteral("local");
    mount.root = VfsUri::fromLocalPath(QStringLiteral("/"));
    mount.fileSystem = std::make_shared<LocalFileSystem>();
    m_vfs->addMount(mount);

    m_tasks = std::make_unique<TaskManager>();
}

void TestDuplicates::cleanup()
{
    m_tasks.reset();
    m_vfs.reset();
    m_tree.reset();
}

void TestDuplicates::aFileThatChangesWhileItIsBeingComparedIsLeftOutOfEveryGroup()
{
    // A scan of a large tree takes minutes, and something else is writing while
    // it runs. A key taken from content that has since changed puts the file in
    // a group it does not belong to -- and the next thing that happens to a
    // group is that all but one of it is deleted.
    class DriveThatChangesAFileWhileItIsRead final : public IFileSystem
    {
    public:
        DriveThatChangesAFileWhileItIsRead(FileSystemPtr inner, QString path)
            : m_inner(std::move(inner))
            , m_path(std::move(path))
        {
        }

        QString scheme() const override { return m_inner->scheme(); }
        VfsCapabilities capabilities() const override { return m_inner->capabilities(); }
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
        Result<void> rename(const VfsUri& from, const VfsUri& to) override
        {
            return m_inner->rename(from, to);
        }
        Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override
        {
            const QMutexLocker locked(&m_guard);
            Result<std::unique_ptr<QIODevice>> reader = m_inner->openRead(target, expectedSize);
            // Rewritten after the reader has its copy, so what was compared really
            // is the old content and the file really is the new one.
            if (target.path() == m_path && !m_changed) {
                m_changed = true;
                if (Result<std::unique_ptr<QIODevice>> writer = m_inner->openWrite(target); writer.ok()) {
                    writer.value()->write(QByteArray(9000, 'z'));
                    closeAndReport(*writer.value());
                }
            }
            return reader;
        }
        Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override
        {
            return m_inner->openWrite(target, expectedSize);
        }

    private:
        FileSystemPtr m_inner;
        QString m_path;
        bool m_changed = false;
        // Files are opened several at a time now, and this one is opened by
        // whichever thread got to it.
        mutable QMutex m_guard;
    };

    const QByteArray payload(8000, 'p');
    auto memory = std::make_shared<MemoryFileSystem>();
    memory->addFile(QStringLiteral("/one.bin"), payload);
    memory->addFile(QStringLiteral("/two.bin"), payload);
    memory->addFile(QStringLiteral("/three.bin"), payload);

    VfsManager vfs;
    Mount mount;
    mount.id = QStringLiteral("mem");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem
        = std::make_shared<DriveThatChangesAFileWhileItIsRead>(memory, QStringLiteral("/two.bin"));
    vfs.addMount(mount);

    auto* task = new FindDuplicatesTask(
        &vfs, { VfsUri::fromString(QStringLiteral("mem:///")) }, std::make_unique<SameContentStrategy>());
    task->setMinimumSize(1);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QCOMPARE(task->changedDuringTheScan(), 1);
    QCOMPARE(task->groups().size(), 1);

    // By value, for the same reason as everywhere else here: groups() returns a
    // copy, and a reference into it dangles as soon as the statement ends.
    const QList<FileEntry> files = task->groups().first().files;
    QCOMPARE(files.size(), 2);
    for (const FileEntry& entry : files) {
        QVERIFY2(entry.name != QLatin1String("two.bin"),
            "a file that changed while it was being read must not be offered as a copy of anything");
    }
}

QList<DuplicateGroup> TestDuplicates::find(std::unique_ptr<IDuplicateStrategy> strategy, qint64 minimumSize)
{
    auto* task = new FindDuplicatesTask(m_vfs.get(), { m_tree->rootUri() }, std::move(strategy));
    task->setMinimumSize(minimumSize);
    m_tasks->submit(task);
    if (!waitFor([task] { return task->isFinished(); }, 30000))
        return {};
    return task->groups();
}

FindDuplicatesTask* TestDuplicates::findUnder(
    const QList<VfsUri>& roots, std::unique_ptr<IDuplicateStrategy> strategy, qint64 minimumSize)
{
    auto* task = new FindDuplicatesTask(m_vfs.get(), roots, std::move(strategy));
    task->setMinimumSize(minimumSize);
    m_tasks->submit(task);
    if (!waitFor([task] { return task->isFinished(); }, 30000))
        return nullptr;
    return task;
}

/// `/a` and `/a/b` together, which is what a file set holds or two console
/// arguments give.
///
/// Both roots were walked, so everything under the inner one arrived twice --
/// and a file is identical to itself, so it was confirmed as a group whose two
/// members are the same uri and whose "could be freed" is its whole size. The
/// next thing that happens to a group is that all but one of it is deleted. See
/// MOLE-341.
void TestDuplicates::aRootInsideAnotherRootDoesNotMakeEveryFileItsOwnDuplicate()
{
    QVERIFY(m_tree->makeDirs(QStringLiteral("inner")));
    QVERIFY(m_tree->writeFile(QStringLiteral("inner/only-one-of-me.txt"), QByteArray(4096, 'x')));

    FindDuplicatesTask* task
        = findUnder({ m_tree->rootUri(), m_tree->rootUri().child(QStringLiteral("inner")) },
            std::make_unique<SameContentStrategy>());
    QVERIFY(task);

    for (const DuplicateGroup& group : task->groups()) {
        QSet<QString> uris;
        for (const FileEntry& file : group.files)
            uris.insert(file.uri.toString());
        QVERIFY2(uris.size() == group.files.size(),
            qPrintable(QStringLiteral("a group listed the same file twice: %1")
                           .arg(group.files.first().uri.toString())));
    }
    QVERIFY2(task->groups().isEmpty(), "one file under two roots is not two files");
    QCOMPARE(task->reclaimableBytes(), qint64(0));

    // And the same root twice, which is the other way to arrive here.
    FindDuplicatesTask* again
        = findUnder({ m_tree->rootUri(), m_tree->rootUri() }, std::make_unique<SameContentStrategy>());
    QVERIFY(again);
    QVERIFY2(again->groups().isEmpty(), "one root given twice is one root");
}

/// A link has its target's size, its hash and its bytes.
///
/// So the two were confirmed as a group with `reclaimable = size`, and deleting
/// "the copy" either removed the target and left a dangling link or removed the
/// link and freed nothing -- whichever the keep rule happened to pick. See
/// MOLE-341.
void TestDuplicates::aSymbolicLinkIsNotADuplicateOfWhatItPointsAt()
{
#ifndef Q_OS_UNIX
    QSKIP("this platform has no symbolic links to make");
#else
    QVERIFY(m_tree->writeFile(QStringLiteral("real.bin"), QByteArray(8192, 'r')));
    QVERIFY(QFile::link(m_tree->absolute(QStringLiteral("real.bin")), m_tree->absolute(QStringLiteral("pointer"))));

    FindDuplicatesTask* task = findUnder({ m_tree->rootUri() }, std::make_unique<SameContentStrategy>());
    QVERIFY(task);

    QVERIFY2(task->groups().isEmpty(),
        "a link and its target are one file, and offering to delete one of them frees nothing");
    QCOMPARE(task->reclaimableBytes(), qint64(0));
    // Left out and said so, because a file quietly missing from a scan is the
    // other way to be wrong.
    QCOMPARE(task->linksLeftOut(), 1);
#endif
}

/// "No duplicates" about a tree most of which could not be opened.
///
/// walker.walk()'s result and walker.errors() were both dropped, so an
/// unreadable subtree was absent from the answer with nothing saying so --
/// while ScanTask, AnalyseDirectoryTask and SyncPlan all report what they could
/// not read (ADR-0030). On the one feature whose output is a deletion list. See
/// MOLE-341.
void TestDuplicates::aSubtreeThatCouldNotBeReadIsCountedRatherThanPassedOver()
{
    auto drive = std::make_shared<MemoryFileSystem>();
    drive->addDirectory(QStringLiteral("/readable"));
    drive->addFile(QStringLiteral("/readable/one.txt"), QByteArray(4096, 'a'));
    drive->addDirectory(QStringLiteral("/locked"));
    drive->addFile(QStringLiteral("/locked/two.txt"), QByteArray(4096, 'a'));

    Mount mount;
    mount.id = QStringLiteral("mem");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = drive;
    m_vfs->addMount(mount);

    drive->setFault(QStringLiteral("/locked"), VfsError::AccessDenied);

    FindDuplicatesTask* task = findUnder(
        { VfsUri::fromString(QStringLiteral("mem:///")) }, std::make_unique<SameContentStrategy>());
    QVERIFY(task);

    // The two identical files would have been a group had both been readable, so
    // this really is an answer that is smaller than the truth.
    QVERIFY(task->groups().isEmpty());
    QVERIFY2(task->unreadablePlaces() > 0,
        "a scan that could not read part of the tree answered exactly as one that read all of it");
    QVERIFY2(
        task->statusText().contains(QStringLiteral("could not be read")), qPrintable(task->statusText()));
}

/// Two names Mole calls one node, put in two buckets.
///
/// The same-name key folded with toLower() -- the *search* fold -- for an
/// identity question, while VfsUri::equals() folds with toCaseFolded(). The two
/// disagree over a handful of code points, Greek final sigma among them, so
/// "ΟΔΥΣΣΕΥΣ" and "οδυσσευς" were never compared with each other at all. See
/// MOLE-341.
void TestDuplicates::twoSpellingsOfOneNameLandInOneBucket()
{
    const QString upper = QString::fromUtf8("ΟΔΥΣΣΕΥΣ.txt");
    const QString lower = QString::fromUtf8("οδυσσευς.txt");
    QVERIFY(m_tree->makeDirs(QStringLiteral("one")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("two")));
    QVERIFY(m_tree->writeFile(QStringLiteral("one/") + upper, QByteArray("the same text")));
    QVERIFY(m_tree->writeFile(QStringLiteral("two/") + lower, QByteArray("a different text entirely")));

    // The same-name strategy, because the question is the key and not the bytes:
    // these two files differ in content on purpose, so a group can only come
    // from their names being read as one.
    const QList<DuplicateGroup> groups = find(std::make_unique<SameNameStrategy>());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().files.size(), 2);
}

void TestDuplicates::everyStrategyDescribesItself()
{
    const auto strategies = IDuplicateStrategy::all();
    QVERIFY(strategies.size() >= 4);

    for (const auto& strategy : strategies) {
        QVERIFY(!strategy->id().isEmpty());
        QVERIFY(!strategy->label().isEmpty());
        // The description has to say what it costs, not only what it matches:
        // that is the part someone needs before starting a scan on a NAS.
        QVERIFY(!strategy->description().isEmpty());
        QVERIFY(strategy->stageCount() >= 1);
        QCOMPARE(strategy->stageNames().size(), strategy->stageCount());
    }
}

void TestDuplicates::sameSizeGroupsBySizeAlone()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("a.txt"), QByteArray(500, 'a')));
    QVERIFY(m_tree->writeFile(QStringLiteral("b.txt"), QByteArray(500, 'b')));
    QVERIFY(m_tree->writeFile(QStringLiteral("c.txt"), QByteArray(900, 'c')));

    const QList<DuplicateGroup> groups = find(std::make_unique<SameSizeStrategy>());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().files.size(), 2);

    // Different contents, same size. That is exactly what this strategy is for
    // and exactly why it is not proof.
    QVERIFY(groups.first().files.at(0).name != groups.first().files.at(1).name);
}

void TestDuplicates::sameNameFindsCopiesThatWereEditedApart()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("notes.txt"), QByteArray("one version")));
    QVERIFY(m_tree->writeFile(QStringLiteral("backup/notes.txt"), QByteArray("a different, longer version")));

    // No content scan would pair these, and pairing them is the whole point of
    // "where else did this file end up".
    const QList<DuplicateGroup> groups = find(std::make_unique<SameNameStrategy>());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().files.size(), 2);

    QCOMPARE(find(std::make_unique<SameContentStrategy>()).size(), 0);
}

void TestDuplicates::sameNameAndSizeIsLessNoisyThanEither()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("report.txt"), QByteArray(400, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("copy/report.txt"), QByteArray(400, 'y')));
    // Same name, different size: not a match for the combined strategy.
    QVERIFY(m_tree->writeFile(QStringLiteral("other/report.txt"), QByteArray(900, 'z')));
    // Same size, different name: likewise.
    QVERIFY(m_tree->writeFile(QStringLiteral("unrelated.txt"), QByteArray(400, 'q')));

    QCOMPARE(find(std::make_unique<SameNameStrategy>()).size(), 1);
    QCOMPARE(find(std::make_unique<SameSizeStrategy>()).size(), 1);

    const QList<DuplicateGroup> combined = find(std::make_unique<SameNameAndSizeStrategy>());
    QCOMPARE(combined.size(), 1);
    QCOMPARE(combined.first().files.size(), 2);
}

void TestDuplicates::identicalContentsProvesIt()
{
    const QByteArray payload = QByteArray(50000, 'p');
    QVERIFY(m_tree->writeFile(QStringLiteral("one.bin"), payload));
    QVERIFY(m_tree->writeFile(QStringLiteral("nested/two.bin"), payload));
    QVERIFY(m_tree->writeFile(QStringLiteral("three.bin"), QByteArray(50000, 'q')));

    const QList<DuplicateGroup> groups = find(std::make_unique<SameContentStrategy>());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().files.size(), 2);

    // Different names, different folders, identical bytes.
    QStringList names { groups.first().files.at(0).name, groups.first().files.at(1).name };
    names.sort();
    QCOMPARE(names, QStringList({ "one.bin", "two.bin" }));
}

void TestDuplicates::identicalContentsSeparatesFilesSharingAHeader()
{
    // The same head, then different. The cheap middle stage groups them
    // and the final stage has to pull them apart -- if it did not, the strategy
    // would report a false match, which is the one failure that matters here.
    QByteArray head(SameContentStrategy::kHeadBytes, 'h');
    QVERIFY(m_tree->writeFile(QStringLiteral("a.bin"), head + QByteArray(1000, 'a')));
    QVERIFY(m_tree->writeFile(QStringLiteral("b.bin"), head + QByteArray(1000, 'b')));

    QCOMPARE(find(std::make_unique<SameContentStrategy>()).size(), 0);

    // And the same files with identical tails do match, so the test is not
    // merely proving that nothing ever matches.
    QVERIFY(m_tree->writeFile(QStringLiteral("c.bin"), head + QByteArray(1000, 'a')));
    QCOMPARE(find(std::make_unique<SameContentStrategy>()).size(), 1);
}

void TestDuplicates::ignoresEmptyFiles()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("empty1.txt"), QByteArray()));
    QVERIFY(m_tree->writeFile(QStringLiteral("empty2.txt"), QByteArray()));

    // Every empty file is identical to every other, and listing thousands of
    // them buries the results that matter.
    QCOMPARE(find(std::make_unique<SameContentStrategy>()).size(), 0);
    QCOMPARE(find(std::make_unique<SameSizeStrategy>()).size(), 0);
}

void TestDuplicates::honoursAMinimumSize()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("small1.txt"), QByteArray(10, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("small2.txt"), QByteArray(10, 'x')));
    QVERIFY(m_tree->writeFile(QStringLiteral("big1.bin"), QByteArray(5000, 'y')));
    QVERIFY(m_tree->writeFile(QStringLiteral("big2.bin"), QByteArray(5000, 'y')));

    QCOMPARE(find(std::make_unique<SameContentStrategy>(), 1).size(), 2);
    QCOMPARE(find(std::make_unique<SameContentStrategy>(), 1000).size(), 1);
}

void TestDuplicates::reportsWhatCouldBeFreed()
{
    const QByteArray payload(4000, 'p');
    QVERIFY(m_tree->writeFile(QStringLiteral("one.bin"), payload));
    QVERIFY(m_tree->writeFile(QStringLiteral("two.bin"), payload));
    QVERIFY(m_tree->writeFile(QStringLiteral("three.bin"), payload));

    const QList<DuplicateGroup> groups = find(std::make_unique<SameContentStrategy>());
    QCOMPARE(groups.size(), 1);

    // Three copies, two of them redundant. The first is not a saving -- it is
    // the file.
    QCOMPARE(groups.first().reclaimable, 8000);
}

void TestDuplicates::groupsAreOrderedByTheSavingTheyOffer()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("smallA.bin"), QByteArray(1000, 'a')));
    QVERIFY(m_tree->writeFile(QStringLiteral("smallB.bin"), QByteArray(1000, 'a')));
    QVERIFY(m_tree->writeFile(QStringLiteral("bigA.bin"), QByteArray(9000, 'b')));
    QVERIFY(m_tree->writeFile(QStringLiteral("bigB.bin"), QByteArray(9000, 'b')));

    const QList<DuplicateGroup> groups = find(std::make_unique<SameContentStrategy>());
    QCOMPARE(groups.size(), 2);
    // Anybody clearing space wants the biggest win first.
    QCOMPARE(groups.first().reclaimable, 9000);
    QCOMPARE(groups.last().reclaimable, 1000);
}

void TestDuplicates::aTreeWithNoDuplicatesReportsNone()
{
    QVERIFY(m_tree->writeFile(QStringLiteral("a.txt"), QByteArray(100, 'a')));
    QVERIFY(m_tree->writeFile(QStringLiteral("b.txt"), QByteArray(200, 'b')));
    QVERIFY(m_tree->writeFile(QStringLiteral("c.txt"), QByteArray(300, 'c')));

    QVERIFY(find(std::make_unique<SameContentStrategy>()).isEmpty());
    QVERIFY(find(std::make_unique<SameNameStrategy>()).isEmpty());
}

namespace {

/// Counts how many files reach each stage, which is the property the staged
/// design exists for.
class CountingStrategy final : public IDuplicateStrategy
{
public:
    QString id() const override { return QStringLiteral("counting"); }
    QString label() const override { return QStringLiteral("Counting"); }
    QString description() const override { return QStringLiteral("for the test"); }
    QStringList stageNames() const override { return { QStringLiteral("size"), QStringLiteral("content") }; }
    bool stageReadsContent(int stage) const override { return stage > 0; }

    QString keyFor(int stage, const FileEntry& entry, IFileSystem*, const CancelToken&) const override
    {
        {
            // A stage that reads is run on several threads, so the tally has to
            // be taken under something. Without this the count is whatever the
            // threads left behind, and the suite is not clean under a sanitizer.
            const QMutexLocker locked(&m_guard);
            ++seen[stage];
        }
        return stage == 0 ? QString::number(entry.size) : entry.name;
    }

    int count(int stage) const
    {
        const QMutexLocker locked(&m_guard);
        return seen.value(stage);
    }

    mutable QHash<int, int> seen;
    mutable QMutex m_guard;
};

} // namespace

void TestDuplicates::theExpensiveStageOnlySeesWhatSurvivedTheCheapOnes()
{
    // Two files share a size; eight do not. Only the pair should reach the
    // second stage -- hashing all ten would be the naive approach this design
    // exists to avoid, and on a real tree it is the difference between minutes
    // and hours.
    QVERIFY(m_tree->writeFile(QStringLiteral("pairA.bin"), QByteArray(4000, 'a')));
    QVERIFY(m_tree->writeFile(QStringLiteral("pairB.bin"), QByteArray(4000, 'b')));
    for (int i = 0; i < 8; ++i) {
        QVERIFY(m_tree->writeFile(QStringLiteral("lone%1.bin").arg(i), QByteArray(1000 + i * 100, 'x')));
    }

    auto strategy = std::make_unique<CountingStrategy>();
    CountingStrategy* watched = strategy.get();

    auto* task = new FindDuplicatesTask(m_vfs.get(), { m_tree->rootUri() }, std::move(strategy));
    task->setMinimumSize(1);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QCOMPARE(watched->count(0), 10);
    QCOMPARE(watched->count(1), 2);
}

namespace {

/// SameContentStrategy, with a note of how many files reached each stage.
///
/// Wraps the real one rather than reimplementing it: what is being asserted is
/// where the real strategy stops, and a copy of its logic would assert that
/// about the copy.
class CountedContentStrategy final : public IDuplicateStrategy
{
public:
    QString id() const override { return m_inner.id(); }
    QString label() const override { return m_inner.label(); }
    QString description() const override { return m_inner.description(); }
    QStringList stageNames() const override { return m_inner.stageNames(); }
    bool stageReadsContent(int stage) const override { return m_inner.stageReadsContent(stage); }
    bool stageComparesContent(int stage) const override { return m_inner.stageComparesContent(stage); }
    QString keyFor(
        int stage, const FileEntry& entry, IFileSystem* fileSystem, const CancelToken& cancel) const override
    {
        note(stage, 1);
        return m_inner.keyFor(stage, entry, fileSystem, cancel);
    }
    QList<QList<FileEntry>> compare(int stage, const QList<FileEntry>& bucket, const DriveLookup& driveFor,
        const CancelToken& cancel) const override
    {
        // Counted in files, like the keying stages, so the numbers below are all
        // "how many files did this stage have to open".
        note(stage, static_cast<int>(bucket.size()));
        return m_inner.compare(stage, bucket, driveFor, cancel);
    }

    int count(int stage) const
    {
        const QMutexLocker locked(&m_guard);
        return seen.value(stage);
    }

private:
    void note(int stage, int files) const
    {
        const QMutexLocker locked(&m_guard);
        seen[stage] += files;
    }

    SameContentStrategy m_inner;
    mutable QHash<int, int> seen;
    mutable QMutex m_guard;
};

} // namespace

void TestDuplicates::filesSharingALongHeaderAreSeparatedWithoutReadingThemWhole()
{
    // Two files of one size that agree over a long header and differ well inside
    // it. This is the shape the files people have a lot of really take -- a video
    // container, a RAW photograph, a PDF, a disk image -- and with a 16 kB head
    // every pair of them agreed at the middle stage and went through to the
    // whole-file hash, which is the pass the middle stage exists to keep small.
    const qint64 head = SameContentStrategy::kHeadBytes;
    QVERIFY2(head >= 1024 * 1024, "the head is smaller than a megabyte");

    QByteArray shared(64 * 1024, 'h');
    QByteArray a = shared + QByteArray(head, 'a');
    QByteArray b = shared + QByteArray(head, 'b');
    QCOMPARE(a.size(), b.size());
    QVERIFY(m_tree->writeFile(QStringLiteral("a.bin"), a));
    QVERIFY(m_tree->writeFile(QStringLiteral("b.bin"), b));

    auto strategy = std::make_unique<CountedContentStrategy>();
    CountedContentStrategy* watched = strategy.get();
    auto* task = new FindDuplicatesTask(m_vfs.get(), { m_tree->rootUri() }, std::move(strategy));
    task->setMinimumSize(1);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QCOMPARE(task->groups().size(), 0);
    // Both were sized, both had their head read -- and neither reached the stage
    // that reads the file whole and compares it. That last number is the point of
    // the change: at a 16 kB head it was two.
    QCOMPARE(watched->count(0), 2);
    QCOMPARE(watched->count(1), 2);
    QCOMPARE(watched->count(2), 0);
}

// ---- the last stage compares, and what that costs ------------------------

namespace {

/// A drive that watches how the files under it are read.
///
/// Two claims need it, and both are about what a scan may never do however large
/// the files or the bucket: hold a file in memory, and hold every file of a
/// bucket open at once. Neither is visible from the outcome of a scan -- a
/// comparison that slurped both files whole would give exactly the same groups --
/// so it is measured at the only place it shows.
class WatchfulDrive final : public IFileSystem
{
public:
    explicit WatchfulDrive(FileSystemPtr inner)
        : m_inner(std::move(inner))
    {
    }

    QString scheme() const override { return m_inner->scheme(); }
    VfsCapabilities capabilities() const override { return m_inner->capabilities(); }
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
    Result<std::unique_ptr<QIODevice>> openWrite(const VfsUri& target, qint64 expectedSize = -1) override
    {
        return m_inner->openWrite(target, expectedSize);
    }

    Result<std::unique_ptr<QIODevice>> openRead(const VfsUri& target, qint64 expectedSize = -1) override
    {
        Result<std::unique_ptr<QIODevice>> inner = m_inner->openRead(target, expectedSize);
        if (!inner.ok())
            return inner;
        return Result<std::unique_ptr<QIODevice>>(
            std::unique_ptr<QIODevice>(new Watched(std::move(inner.value()), this)));
    }

    int mostOpenAtOnce() const { return m_mostOpen.loadAcquire(); }
    qint64 largestRead() const { return m_largestRead.loadAcquire(); }

private:
    class Watched final : public QIODevice
    {
    public:
        Watched(std::unique_ptr<QIODevice> inner, WatchfulDrive* drive)
            : m_inner(std::move(inner))
            , m_drive(drive)
        {
            m_drive->opened();
            open(QIODevice::ReadOnly);
        }
        ~Watched() override { m_drive->closed(); }

        bool isSequential() const override { return false; }
        qint64 size() const override { return m_inner->size(); }

    protected:
        qint64 readData(char* data, qint64 maxSize) override
        {
            m_drive->read(maxSize);
            return m_inner->read(data, maxSize);
        }
        qint64 writeData(const char*, qint64) override { return -1; }

    private:
        std::unique_ptr<QIODevice> m_inner;
        WatchfulDrive* m_drive = nullptr;
    };

    void opened()
    {
        const int now = m_open.fetchAndAddOrdered(1) + 1;
        int seen = m_mostOpen.loadAcquire();
        while (now > seen && !m_mostOpen.testAndSetOrdered(seen, now))
            seen = m_mostOpen.loadAcquire();
    }
    void closed() { m_open.fetchAndSubOrdered(1); }
    void read(qint64 bytes)
    {
        qint64 seen = m_largestRead.loadAcquire();
        while (bytes > seen && !m_largestRead.testAndSetOrdered(seen, bytes))
            seen = m_largestRead.loadAcquire();
    }

    FileSystemPtr m_inner;
    QAtomicInt m_open { 0 };
    QAtomicInt m_mostOpen { 0 };
    QAtomicInteger<qint64> m_largestRead { 0 };
};

} // namespace

void TestDuplicates::contentsAreProvedByComparisonRatherThanByADigest()
{
    // Two files of one size that agree over more than the head reads and differ
    // in the very last byte. The head lets them through -- that is what it is
    // for -- and what separates them is the files themselves being compared.
    const qint64 head = SameContentStrategy::kHeadBytes;
    QByteArray a(head + 4096, 'q');
    QByteArray b = a;
    b[b.size() - 1] = 'r';
    QVERIFY(m_tree->writeFile(QStringLiteral("last-byte-a.bin"), a));
    QVERIFY(m_tree->writeFile(QStringLiteral("last-byte-b.bin"), b));
    // And a pair that really is identical, so this is not passing by refusing
    // to group anything at all.
    QVERIFY(m_tree->writeFile(QStringLiteral("same-one.bin"), QByteArray(9000, 's')));
    QVERIFY(m_tree->writeFile(QStringLiteral("same-two.bin"), QByteArray(9000, 's')));

    const QList<DuplicateGroup> groups = find(std::make_unique<SameContentStrategy>());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().files.size(), 2);
    QVERIFY2(groups.first().files.first().name.startsWith(QStringLiteral("same-")),
        qPrintable(groups.first().files.first().name));
}

void TestDuplicates::aBucketLargerThanTheOpenLimitIsStillOneGroup()
{
    // More copies than may be held open at once, which is an ordinary shape --
    // a photograph filed in forty places. The bucket is compared in slices, and
    // the slices have to be joined back up or the answer is forty files in three
    // groups that are all the same file.
    const int copies = kMaxOpenAtOnce * 2 + 5;
    const QByteArray payload(20000, 'c');
    for (int i = 0; i < copies; ++i)
        QVERIFY(m_tree->writeFile(QStringLiteral("copy%1.bin").arg(i), payload));

    const QList<DuplicateGroup> groups = find(std::make_unique<SameContentStrategy>());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().files.size(), copies);
}

void TestDuplicates::aFileAloneInItsSliceIsNotLost()
{
    // The bucket is one file longer than the limit, so the last slice holds a
    // single file -- and a slice that threw away its lone files would lose it.
    // It is a match for every one of the others.
    const int copies = kMaxOpenAtOnce + 1;
    const QByteArray payload(12345, 'd');
    for (int i = 0; i < copies; ++i)
        QVERIFY(m_tree->writeFile(QStringLiteral("alone%1.bin").arg(i), payload));

    const QList<DuplicateGroup> groups = find(std::make_unique<SameContentStrategy>());
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().files.size(), copies);
}

void TestDuplicates::nothingIsEverHeldWholeAndNothingBeyondTheOpenLimitIsEverOpen()
{
    // The two bounds that keep a scan of a hundred-gigabyte disk image from
    // being a scan that ends in the process being killed. Neither shows in the
    // groups a scan produces, so both are measured at the drive.
    const int copies = kMaxOpenAtOnce * 3;
    const QByteArray payload(kComparisonChunkBytes * 3 + 517, 'e');

    auto memory = std::make_shared<MemoryFileSystem>();
    for (int i = 0; i < copies; ++i)
        memory->addFile(QStringLiteral("/big%1.bin").arg(i), payload);
    auto watchful = std::make_shared<WatchfulDrive>(memory);

    VfsManager vfs;
    Mount mount;
    mount.id = QStringLiteral("watched");
    mount.root = VfsUri::fromString(QStringLiteral("mem:///"));
    mount.fileSystem = watchful;
    vfs.addMount(mount);

    auto* task = new FindDuplicatesTask(
        &vfs, { VfsUri::fromString(QStringLiteral("mem:///")) }, std::make_unique<SameContentStrategy>());
    task->setMinimumSize(1);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 60000));

    QCOMPARE(task->groups().size(), 1);
    QCOMPARE(task->groups().first().files.size(), copies);

    // A file is read a chunk at a time and never asked for whole, so what a
    // comparison costs in memory does not depend on how big the files are.
    QVERIFY2(watchful->largestRead() <= kComparisonChunkBytes,
        qPrintable(QStringLiteral("largest read was %1 bytes").arg(watchful->largestRead())));

    // And a bucket of any size is compared in slices, so what it costs does not
    // depend on how many files agreed either. The limit is per comparison and
    // the scan reads on several threads, so the ceiling is that many slices.
    const int ceiling = kMaxOpenAtOnce * task->workerCount();
    QVERIFY2(watchful->mostOpenAtOnce() <= ceiling,
        qPrintable(QStringLiteral("%1 files were open at once, over the %2 this may hold")
                       .arg(watchful->mostOpenAtOnce())
                       .arg(ceiling)));
}

void TestDuplicates::theAnswerIsTheSameHoweverManyThreadsRead()
{
    // Reads are overlapped and nothing else is, so a scan on eight threads has
    // to produce the same groups in the same order as a scan on one. If it does
    // not, the ordering the results depend on is coming from the scheduler.
    for (int i = 0; i < 6; ++i) {
        const QByteArray payload(4000 + i * 100, static_cast<char>('a' + i));
        QVERIFY(m_tree->writeFile(QStringLiteral("group%1-one.bin").arg(i), payload));
        QVERIFY(m_tree->writeFile(QStringLiteral("group%1-two.bin").arg(i), payload));
        QVERIFY(m_tree->writeFile(QStringLiteral("group%1-three.bin").arg(i), payload));
    }

    const auto scan = [this](int workers) {
        auto* task = new FindDuplicatesTask(
            m_vfs.get(), { m_tree->rootUri() }, std::make_unique<SameContentStrategy>());
        task->setMinimumSize(1);
        task->setWorkerCount(workers);
        m_tasks->submit(task);
        [&] { QVERIFY(waitFor([task] { return task->isFinished(); }, 60000)); }();
        QStringList names;
        for (const DuplicateGroup& group : task->groups()) {
            QStringList inGroup;
            for (const FileEntry& file : group.files)
                inGroup.append(file.name);
            inGroup.sort();
            names.append(inGroup.join(QLatin1Char(',')));
        }
        return names;
    };

    const QStringList onOne = scan(1);
    QCOMPARE(onOne.size(), 6);
    QCOMPARE(scan(8), onOne);
}

// ---- results as they are found ------------------------------------------

namespace {

/// Two groups of three files each, at two different sizes.
///
/// Two sizes rather than one is the whole point: the cheap first stage puts
/// them in separate buckets, and separate buckets are what the last stage can
/// finish at different moments. One size would be one bucket and one answer,
/// which is the behaviour this is meant to distinguish from.
bool writeTwoGroupsOfDifferentSizes(TempTree& tree)
{
    const QByteArray small(2048, 'a');
    const QByteArray large(9000, 'b');
    return tree.writeFile(QStringLiteral("small/one.bin"), small)
        && tree.writeFile(QStringLiteral("small/two.bin"), small)
        && tree.writeFile(QStringLiteral("small/three.bin"), small)
        && tree.writeFile(QStringLiteral("large/one.bin"), large)
        && tree.writeFile(QStringLiteral("large/two.bin"), large)
        && tree.writeFile(QStringLiteral("large/three.bin"), large);
}

} // namespace

void TestDuplicates::groupsArriveAsTheyAreConfirmedRatherThanAllAtTheEnd()
{
    QVERIFY(writeTwoGroupsOfDifferentSizes(*m_tree));

    auto* task
        = new FindDuplicatesTask(m_vfs.get(), { m_tree->rootUri() }, std::make_unique<SameContentStrategy>());
    task->setMinimumSize(1);

    // The order things were *announced* in, not the order they were noticed. Both
    // signals are queued to this thread and dispatched in the order they were
    // emitted, so a "finished" sitting after two groups is proof the task emitted
    // them before it ended -- and it is proof taken from the data, with no clock
    // anywhere in it.
    QStringList announcements;
    connect(task, &FindDuplicatesTask::groupsFound, this,
        [&announcements](const QList<DuplicateGroup>& groups, const QList<int>&) {
            for (int i = 0; i < groups.size(); ++i)
                announcements.append(QStringLiteral("group"));
        });
    connect(
        task, &Task::finished, this, [&announcements] { announcements.append(QStringLiteral("finished")); });

    m_tasks->submit(task);
    // Waited on the announcement, not on the state. `isFinished()` is true as soon
    // as the task sets it, but `finished()` is emitted through a queued call -- so a
    // wait on the state can return with the third entry not yet delivered, and the
    // comparison below then sees two. It did, once, in a parallel run: the same shape
    // as MOLE-256, waiting for something adjacent to the thing being asserted.
    QVERIFY(waitFor([&announcements] { return announcements.contains(QStringLiteral("finished")); }, 30000));

    const QStringList expected { QStringLiteral("group"), QStringLiteral("group"),
        QStringLiteral("finished") };
    QCOMPARE(announcements, expected);
}

void TestDuplicates::aGroupIsNeverAnnouncedAndThenTakenBack()
{
    QVERIFY(writeTwoGroupsOfDifferentSizes(*m_tree));
    // And one file that looks like a candidate at the first stage and separates at
    // the last: it shares a size with the small group and nothing else. If a group
    // could be announced before its last stage had run, this is the file that would
    // make one wrong.
    QByteArray impostor(2048, 'a');
    impostor[2047] = 'z';
    QVERIFY(m_tree->writeFile(QStringLiteral("small/impostor.bin"), impostor));

    auto* task
        = new FindDuplicatesTask(m_vfs.get(), { m_tree->rootUri() }, std::make_unique<SameContentStrategy>());
    task->setMinimumSize(1);

    QList<DuplicateGroup> announced;
    connect(task, &FindDuplicatesTask::groupsFound, this,
        [&announced](const QList<DuplicateGroup>& groups, const QList<int>&) { announced.append(groups); });

    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    // What was announced is exactly what the scan ended up with -- nothing extra
    // that had to be withdrawn, and nothing held back.
    const QList<DuplicateGroup> settled = task->groups();
    QCOMPARE(announced.size(), settled.size());
    QCOMPARE(announced.size(), 2);
    for (const DuplicateGroup& group : std::as_const(announced)) {
        QVERIFY(group.files.size() == 3);
        bool found = false;
        for (const DuplicateGroup& settledGroup : settled) {
            if (settledGroup.files.size() == group.files.size()
                && settledGroup.reclaimable == group.reclaimable
                && settledGroup.files.first().uri == group.files.first().uri) {
                found = true;
            }
        }
        QVERIFY2(found, "a group was announced and is not in the answer");
    }
    // The impostor is in none of them, which is what proves the last stage ran
    // before anything went out.
    for (const DuplicateGroup& group : settled) {
        for (const FileEntry& entry : group.files)
            QVERIFY(entry.name != QStringLiteral("impostor.bin"));
    }
}

void TestDuplicates::theListIsInOrderAtEveryInstantAndNotOnlyAtTheEnd()
{
    QVERIFY(writeTwoGroupsOfDifferentSizes(*m_tree));

    auto* task
        = new FindDuplicatesTask(m_vfs.get(), { m_tree->rootUri() }, std::make_unique<SameContentStrategy>());
    task->setMinimumSize(1);

    // Rebuilt here from the positions the task hands out, exactly as the tab does.
    // Sorted after every insertion rather than only at the end, because a list that
    // is in arrival order for the whole of a long scan and then rearranges itself
    // is the thing this ordering exists to avoid.
    QList<qint64> mirrored;
    bool everOutOfOrder = false;
    connect(task, &FindDuplicatesTask::groupsFound, this,
        [&mirrored, &everOutOfOrder](const QList<DuplicateGroup>& groups, const QList<int>& positions) {
            for (int g = 0; g < groups.size() && g < positions.size(); ++g) {
                mirrored.insert(
                    qBound(0, positions.at(g), static_cast<int>(mirrored.size())), groups.at(g).reclaimable);
                for (int i = 1; i < mirrored.size(); ++i) {
                    if (mirrored.at(i) > mirrored.at(i - 1))
                        everOutOfOrder = true;
                }
            }
        });

    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QVERIFY2(!everOutOfOrder, "the list was out of order part-way through the scan");
    QList<qint64> settled;
    for (const DuplicateGroup& group : task->groups())
        settled.append(group.reclaimable);
    QCOMPARE(mirrored, settled);
    QVERIFY(settled.size() == 2);
    QVERIFY2(settled.first() > settled.last(), "the biggest saving is not at the top");
}

void TestDuplicates::aScanStoppedPartWayKeepsWhatItHadAlreadyConfirmed()
{
    // Three groups at three sizes, so the last stage has three buckets to get
    // through and stopping after the first leaves two.
    const QByteArray a(2048, 'a');
    const QByteArray b(5000, 'b');
    const QByteArray c(9000, 'c');
    QVERIFY(m_tree->writeFile(QStringLiteral("a/one.bin"), a));
    QVERIFY(m_tree->writeFile(QStringLiteral("a/two.bin"), a));
    QVERIFY(m_tree->writeFile(QStringLiteral("b/one.bin"), b));
    QVERIFY(m_tree->writeFile(QStringLiteral("b/two.bin"), b));
    QVERIFY(m_tree->writeFile(QStringLiteral("c/one.bin"), c));
    QVERIFY(m_tree->writeFile(QStringLiteral("c/two.bin"), c));

    auto* task
        = new FindDuplicatesTask(m_vfs.get(), { m_tree->rootUri() }, std::make_unique<SameContentStrategy>());
    task->setMinimumSize(1);

    // Stopped by the data rather than by a clock: the moment the first group is
    // confirmed, and on the task's own thread, so the stop is in place before it
    // looks at the next bucket. A test that slept for 200 ms would stop somewhere
    // different on every machine.
    //
    // Through the hook rather than the signal, because since MOLE-211 the signal
    // is drained on the drawing thread and would arrive after the scan of six
    // small files had already ended -- which is the whole point of the drain, and
    // exactly why the hook exists.
    task->setOnGroupConfirmed([task](const DuplicateGroup&, int) { task->requestCancel(); });

    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QCOMPARE(task->state(), Task::State::Cancelled);
    // What it had is kept. Every group of it agreed at every stage, and the scan
    // stopping does not make that less true -- it only means there may be more.
    QCOMPARE(task->groups().size(), 1);
    QCOMPARE(task->groups().first().files.size(), 2);
}

void TestDuplicates::aBurstOfConfirmationsIsBoundedByTheDrainAndNotByTheGroupCount()
{
    // Two hundred groups, each at a size of its own, so the last stage settles two
    // hundred buckets and confirms two hundred times as fast as it can read them.
    // Before MOLE-211 that was two hundred queued events into the window, from a
    // worker thread, with nothing bounding how many could arrive in a frame --
    // a second unthrottled channel out of a task whose own header explains why
    // there is a box.
    const int wanted = 200;
    for (int i = 0; i < wanted; ++i) {
        const QByteArray body(1024 + i * 8, static_cast<char>('a' + i % 26));
        QVERIFY(m_tree->writeFile(QStringLiteral("pile/%1/one.bin").arg(i), body));
        QVERIFY(m_tree->writeFile(QStringLiteral("pile/%1/two.bin").arg(i), body));
    }

    auto* task
        = new FindDuplicatesTask(m_vfs.get(), { m_tree->rootUri() }, std::make_unique<SameContentStrategy>());
    task->setMinimumSize(1);

    int emissions = 0;
    QList<qint64> mirrored;
    QSet<QString> arrivedTwice;
    QSet<QString> seen;
    connect(task, &FindDuplicatesTask::groupsFound, this,
        [&](const QList<DuplicateGroup>& groups, const QList<int>& positions) {
            ++emissions;
            QCOMPARE(groups.size(), positions.size());
            for (int g = 0; g < groups.size(); ++g) {
                const QString first = groups.at(g).files.first().uri.toString();
                if (seen.contains(first))
                    arrivedTwice.insert(first);
                seen.insert(first);
                // Applied in the order they were given, which is what makes a
                // position in a batch mean anything at all.
                mirrored.insert(
                    qBound(0, positions.at(g), static_cast<int>(mirrored.size())), groups.at(g).reclaimable);
            }
        });

    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 120000));
    drainEvents();

    // Everything arrived, once each, and the list built from the batches is the
    // list the scan ended up with -- ADR-0043's ordering, unchanged.
    QCOMPARE(seen.size(), wanted);
    QVERIFY2(arrivedTwice.isEmpty(), "a group was announced twice");
    QList<qint64> settled;
    for (const DuplicateGroup& group : task->groups())
        settled.append(group.reclaimable);
    QCOMPARE(mirrored, settled);

    // The bound, taken from the task's own elapsed time rather than from a clock
    // the test reads: at most one event per drain interval, plus the one
    // flushReports() hands over at the end. A slower machine gets a longer scan
    // and a proportionally larger allowance, so this cannot be made flaky by
    // load -- and it fails outright for anything that emits per group, which
    // would need two hundred.
    const qint64 allowed = task->elapsedMs() / Task::kDrainIntervalMs + 2;
    QVERIFY2(emissions >= 1, "nothing was announced at all");
    QVERIFY2(emissions <= allowed,
        qPrintable(QStringLiteral("%1 emissions for %2 groups in %3 ms allows at most %4")
                       .arg(emissions)
                       .arg(wanted)
                       .arg(task->elapsedMs())
                       .arg(allowed)));
}

void TestDuplicates::aGroupReachesTheWindowWhileTheScanIsStillStandingInConfirm()
{
    // A drain is a bound, not a delay: the one group of a quiet scan must not wait
    // for the interval to close or for the walk to end.
    //
    // Proved by the data rather than by a clock. The scan is held inside confirm()
    // -- on its own thread, through the hook that runs there -- until the group has
    // been received on this one. If the group only arrived at the end of the scan,
    // the scan could not end, and the wait times out instead.
    const QByteArray same(4096, 'a');
    QVERIFY(m_tree->writeFile(QStringLiteral("one.bin"), same));
    QVERIFY(m_tree->writeFile(QStringLiteral("deep/two.bin"), same));

    auto* task
        = new FindDuplicatesTask(m_vfs.get(), { m_tree->rootUri() }, std::make_unique<SameContentStrategy>());
    task->setMinimumSize(1);

    QSemaphore arrived;
    std::atomic<bool> arrivedBeforeTheScanMovedOn { false };
    connect(task, &FindDuplicatesTask::groupsFound, this,
        [&arrived](const QList<DuplicateGroup>&, const QList<int>&) { arrived.release(); });
    task->setOnGroupConfirmed([&arrived, &arrivedBeforeTheScanMovedOn](const DuplicateGroup&, int) {
        arrivedBeforeTheScanMovedOn = arrived.tryAcquire(1, 10000);
    });

    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QVERIFY2(arrivedBeforeTheScanMovedOn.load(),
        "the group did not reach the window until the scan had finished with it");
    QCOMPARE(task->groups().size(), 1);
}

MOLE_TEST_MAIN(TestDuplicates)
#include "tst_Duplicates.moc"
