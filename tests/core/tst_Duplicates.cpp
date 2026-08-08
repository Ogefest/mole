#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/CoreMetaTypes.h"
#include "core/duplicates/FindDuplicatesTask.h"
#include "core/duplicates/Strategies.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"

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
    // The same first 16 kB, then different. The cheap middle stage groups them
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

MOLE_TEST_MAIN(TestDuplicates)
#include "tst_Duplicates.moc"
