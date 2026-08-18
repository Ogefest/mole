#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/CoreMetaTypes.h"
#include "core/duplicates/FindDuplicatesTask.h"
#include "core/duplicates/Strategies.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

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

    void groupsArriveAsTheyAreConfirmedRatherThanAllAtTheEnd();
    void aGroupIsNeverAnnouncedAndThenTakenBack();
    void theListIsInOrderAtEveryInstantAndNotOnlyAtTheEnd();
    void aScanStoppedPartWayKeepsWhatItHadAlreadyConfirmed();

private:
    QList<DuplicateGroup> find(std::unique_ptr<IDuplicateStrategy> strategy, qint64 minimumSize = 1);

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
            Result<std::unique_ptr<QIODevice>> reader = m_inner->openRead(target, expectedSize);
            // Rewritten after the reader has its copy, so what was hashed really
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
        ++seen[stage];
        return stage == 0 ? QString::number(entry.size) : entry.name;
    }

    mutable QHash<int, int> seen;
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

    QCOMPARE(watched->seen.value(0), 10);
    QCOMPARE(watched->seen.value(1), 2);
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
    QString keyFor(
        int stage, const FileEntry& entry, IFileSystem* fileSystem, const CancelToken& cancel) const override
    {
        ++seen[stage];
        return m_inner.keyFor(stage, entry, fileSystem, cancel);
    }

    mutable QHash<int, int> seen;

private:
    SameContentStrategy m_inner;
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
    // that reads the file whole. That last number is the point of the change: at
    // 16 kB it was two.
    QCOMPARE(watched->seen.value(0), 2);
    QCOMPARE(watched->seen.value(1), 2);
    QCOMPARE(watched->seen.value(2), 0);
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
    connect(task, &FindDuplicatesTask::groupFound, this,
        [&announcements](const DuplicateGroup&, int) { announcements.append(QStringLiteral("group")); });
    connect(
        task, &Task::finished, this, [&announcements] { announcements.append(QStringLiteral("finished")); });

    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

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
    connect(task, &FindDuplicatesTask::groupFound, this,
        [&announced](const DuplicateGroup& group, int) { announced.append(group); });

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
    connect(task, &FindDuplicatesTask::groupFound, this,
        [&mirrored, &everOutOfOrder](const DuplicateGroup& group, int position) {
            mirrored.insert(qBound(0, position, static_cast<int>(mirrored.size())), group.reclaimable);
            for (int i = 1; i < mirrored.size(); ++i) {
                if (mirrored.at(i) > mirrored.at(i - 1))
                    everOutOfOrder = true;
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
    connect(
        task, &FindDuplicatesTask::groupFound, task,
        [task](const DuplicateGroup&, int) { task->requestCancel(); }, Qt::DirectConnection);

    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->isFinished(); }, 30000));

    QCOMPARE(task->state(), Task::State::Cancelled);
    // What it had is kept. Every group of it agreed at every stage, and the scan
    // stopping does not make that less true -- it only means there may be more.
    QCOMPARE(task->groups().size(), 1);
    QCOMPARE(task->groups().first().files.size(), 2);
}

MOLE_TEST_MAIN(TestDuplicates)
#include "tst_Duplicates.moc"
