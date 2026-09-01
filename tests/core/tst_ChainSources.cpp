#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/automation/ChainSources.h"
#include "core/automation/ChainSteps.h"
#include "core/automation/ChainTask.h"
#include "core/sets/FileSetStore.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace mole;
using namespace mole::test;

namespace {

/// What the fusion is measured with, and why counting is the only way.
///
/// The fusion cannot be checked by reading the plan back -- that would assert
/// that the code says what it says. What it can be checked by is the work: a
/// filter folded into a walk answers from the listing the walk already had, and
/// one that runs as its own step has to ask the drive about every uri it was
/// handed, because a uri is not a FileEntry. FaultyFileSystem counts both, and it
/// is the wrapper every other test already uses rather than a fourth one written
/// for this.

/// A step that hands on exactly what it was given, so a filter can be put one
/// place further down without changing anything else about the chain.
class PassThrough final : public IChainStepKind
{
public:
    QString kind() const override { return QStringLiteral("passthrough"); }
    QString displayName() const override { return QStringLiteral("Pass on"); }
    StepRole role() const override { return StepRole::Transform; }
    QList<StepParameter> parameters() const override { return {}; }

    StepOutcome run(const ChainStep&, const QStringList& incoming, const StepContext&) override
    {
        return StepOutcome::produced(incoming);
    }
    StepPreview preview(const ChainStep&, const QStringList& incoming, const StepContext&) override
    {
        return StepPreview::would(incoming);
    }
};

ChainStep stepOf(const QString& kind)
{
    ChainStep step;
    step.kind = kind;
    return step;
}

} // namespace

/// Where a chain begins. See MOLE-167.
class TestChainSources : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aPlaceReturnsWhatIsUnderIt();
    void aPlaceHonoursADepthOfNoughtAndOfEverything();
    void aFileSetAsAPlaceReturnsItsMembersAcrossDrives();
    void aQueryIsStoredAndRunAgainRatherThanItsAnswer();
    void aFilterDirectlyAfterAPlaceIsFoldedIntoTheWalk();
    void theSameFilterOneStepFurtherDownIsNotFoldedInAndAnswersTheSame();
    void aFilterOverNamesAndSizesOpensNoFiles();
    void aFilterThatReachesIntoContentsAnswersTheSameWhereverItSits();
    void aFilterSaysWhetherItWillOpenTheFilesItIsGiven();
    void aChainStartedFromWhatIsSelectedUsesIt();
    void aChainStartedFromNothingSaysSoRatherThanActingOnEverything();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::shared_ptr<FaultyFileSystem> m_drive;
    std::unique_ptr<FileSetStore> m_sets;
    ChainRegistry m_registry;
    std::unique_ptr<PlaceSource> m_place;
    std::unique_ptr<QuerySource> m_query;
    std::unique_ptr<FilterStep> m_filter;
    std::unique_ptr<ListingSource> m_listing;
    std::unique_ptr<PassThrough> m_passThrough;

    DriveResolver resolver()
    {
        auto drive = m_drive;
        return [drive](const VfsUri& uri) -> FileSystemPtr {
            return uri.scheme() == QLatin1String("mem") ? drive : nullptr;
        };
    }

    /// Runs a chain and hands back the task, so a case can read what it produced.
    ChainTask* run(const QList<ChainStep>& steps, const QStringList& startedWith = {})
    {
        Chain chain;
        chain.id = QStringLiteral("c1");
        chain.steps = steps;
        auto* task = new ChainTask(chain, &m_registry);
        task->setStartingList(startedWith);
        m_tasks->submit(task);
        return waitForTask(task, 30000) ? task : nullptr;
    }

    static SearchQuery onlyText()
    {
        SearchQuery query;
        query.add(SearchPredicate::extensions({ QStringLiteral("txt") }));
        return query;
    }

    /// A criterion that cannot be answered from a listing: the file has to be
    /// opened and read. Only "three" is in a file under mem:///reports.
    static SearchQuery onlyContaining(const QString& text)
    {
        SearchQuery query;
        query.add(SearchPredicate::content(text));
        return query;
    }

    ChainStep filterOn(const SearchQuery& query) const
    {
        ChainStep filter = stepOf(FilterStep::stepKind());
        QVariantMap criteria;
        putQuery(criteria, FilterStep::queryKey(), query);
        filter.parameters = criteria;
        return filter;
    }
};

void TestChainSources::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_tasks = std::make_unique<TaskManager>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_drive = std::make_shared<FaultyFileSystem>(m_memory);
    m_sets = std::make_unique<FileSetStore>(QDir(m_dir->path()).filePath(QStringLiteral("sets.json")));

    m_registry = ChainRegistry {};
    m_place = std::make_unique<PlaceSource>(resolver(), m_sets.get());
    m_query = std::make_unique<QuerySource>(resolver());
    m_filter = std::make_unique<FilterStep>(resolver());
    m_listing = std::make_unique<ListingSource>();
    m_passThrough = std::make_unique<PassThrough>();
    m_registry.registerKind(m_place.get());
    m_registry.registerKind(m_query.get());
    m_registry.registerKind(m_filter.get());
    m_registry.registerKind(m_listing.get());
    m_registry.registerKind(m_passThrough.get());

    m_memory->addFile(QStringLiteral("/reports/a.txt"), QByteArray("one"));
    m_memory->addFile(QStringLiteral("/reports/b.pdf"), QByteArray("two"));
    m_memory->addFile(QStringLiteral("/reports/deep/c.txt"), QByteArray("three"));
    m_memory->addFile(QStringLiteral("/elsewhere/d.txt"), QByteArray("four"));
}

void TestChainSources::cleanup()
{
    m_tasks.reset();
    m_dir.reset();
}

void TestChainSources::aPlaceReturnsWhatIsUnderIt()
{
    ChainStep place = stepOf(PlaceSource::stepKind());
    place.parameters = { { PlaceSource::whereKey(), QStringLiteral("mem:///reports") } };

    ChainTask* task = run({ place });
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QStringList produced = task->produced();
    produced.sort();
    QCOMPARE(produced,
        QStringList({ QStringLiteral("mem:///reports/a.txt"), QStringLiteral("mem:///reports/b.pdf"),
            QStringLiteral("mem:///reports/deep"), QStringLiteral("mem:///reports/deep/c.txt") }));
}

void TestChainSources::aPlaceHonoursADepthOfNoughtAndOfEverything()
{
    // The same field maxDepth carries everywhere else: 0 is this folder and
    // nothing under it, -1 is everything below.
    ChainStep shallow = stepOf(PlaceSource::stepKind());
    shallow.parameters
        = { { PlaceSource::whereKey(), QStringLiteral("mem:///reports") }, { PlaceSource::depthKey(), 0 } };

    ChainTask* task = run({ shallow });
    QVERIFY(task);
    QStringList produced = task->produced();
    produced.sort();
    QCOMPARE(produced,
        QStringList({ QStringLiteral("mem:///reports/a.txt"), QStringLiteral("mem:///reports/b.pdf"),
            QStringLiteral("mem:///reports/deep") }));
    QVERIFY2(!produced.contains(QStringLiteral("mem:///reports/deep/c.txt")),
        "a depth of nought went into a subfolder");

    ChainStep everything = stepOf(PlaceSource::stepKind());
    everything.parameters
        = { { PlaceSource::whereKey(), QStringLiteral("mem:///reports") }, { PlaceSource::depthKey(), -1 } };
    ChainTask* deep = run({ everything });
    QVERIFY(deep);
    QVERIFY(deep->produced().contains(QStringLiteral("mem:///reports/deep/c.txt")));
}

void TestChainSources::aFileSetAsAPlaceReturnsItsMembersAcrossDrives()
{
    // A set is already a list, its members may sit on several drives, and depth
    // means nothing to it -- so there is nothing to walk.
    FileSet set;
    set.id = QStringLiteral("s1");
    set.name = QStringLiteral("Things to tidy");
    set.uris = { QStringLiteral("mem:///reports/a.txt"), QStringLiteral("file:///tmp/elsewhere/e.txt"),
        QStringLiteral("sftp://host/f.txt") };
    QVERIFY(m_sets->put(set));

    ChainStep place = stepOf(PlaceSource::stepKind());
    place.parameters = { { PlaceSource::setKey(), QStringLiteral("s1") } };

    ChainTask* task = run({ place });
    QVERIFY(task);
    QCOMPARE(task->produced(), set.uris);
    QVERIFY2(m_drive->listCount() == 0, "a file set was walked as though it were a folder");
}

void TestChainSources::aQueryIsStoredAndRunAgainRatherThanItsAnswer()
{
    // The difference between a chain that processes today's photographs and one
    // that processes the photographs that happened to be there when it was
    // written. This is what MOLE-163 was for.
    ChainStep search = stepOf(QuerySource::stepKind());
    QVariantMap parameters { { QuerySource::whereKey(), QStringLiteral("mem:///reports") } };
    putQuery(parameters, QuerySource::queryKey(), onlyText());
    search.parameters = parameters;

    ChainTask* first = run({ search });
    QVERIFY(first);
    QStringList before = first->produced();
    before.sort();
    QCOMPARE(before,
        QStringList({ QStringLiteral("mem:///reports/a.txt"), QStringLiteral("mem:///reports/deep/c.txt") }));

    // A file that did not exist when the chain was saved.
    m_memory->addFile(QStringLiteral("/reports/new.txt"), QByteArray("later"));

    ChainTask* second = run({ search });
    QVERIFY(second);
    QVERIFY2(second->produced().contains(QStringLiteral("mem:///reports/new.txt")),
        "the search handed back the answer it gave last time");
    QCOMPARE(second->produced().size(), 3);
}

void TestChainSources::aFilterDirectlyAfterAPlaceIsFoldedIntoTheWalk()
{
    ChainStep place = stepOf(PlaceSource::stepKind());
    place.parameters = { { PlaceSource::whereKey(), QStringLiteral("mem:///reports") } };
    ChainStep filter = stepOf(FilterStep::stepKind());
    QVariantMap criteria;
    putQuery(criteria, FilterStep::queryKey(), onlyText());
    filter.parameters = criteria;

    m_drive->forgetCounts();
    ChainTask* task = run({ place, filter });
    QVERIFY(task);
    QStringList produced = task->produced();
    produced.sort();
    QCOMPARE(produced,
        QStringList({ QStringLiteral("mem:///reports/a.txt"), QStringLiteral("mem:///reports/deep/c.txt") }));

    // Folded in: the criteria were answered from the listing the walk already
    // had, so nothing had to be asked about a second time.
    QCOMPARE(m_drive->statCount(), 0);
    // And the chain that ran is one step, which is what the fusion means.
    QCOMPARE(task->stepsRun(), 1);
}

void TestChainSources::theSameFilterOneStepFurtherDownIsNotFoldedInAndAnswersTheSame()
{
    ChainStep place = stepOf(PlaceSource::stepKind());
    place.parameters = { { PlaceSource::whereKey(), QStringLiteral("mem:///reports") } };
    ChainStep filter = stepOf(FilterStep::stepKind());
    QVariantMap criteria;
    putQuery(criteria, FilterStep::queryKey(), onlyText());
    filter.parameters = criteria;

    m_drive->forgetCounts();
    ChainTask* task = run({ place, stepOf(QStringLiteral("passthrough")), filter });
    QVERIFY(task);
    QStringList produced = task->produced();
    produced.sort();
    // The same answer, which is the half that matters: the fusion is an
    // optimisation and must not be a difference in meaning.
    QCOMPARE(produced,
        QStringList({ QStringLiteral("mem:///reports/a.txt"), QStringLiteral("mem:///reports/deep/c.txt") }));

    // And the cost the fusion exists to avoid: a filter handed uris has to ask
    // the drive about every one of them, because a uri is not a FileEntry.
    QVERIFY2(m_drive->statCount() > 0, "the filter answered without asking the drive anything");
    QCOMPARE(task->stepsRun(), 3);
}

void TestChainSources::aFilterOverNamesAndSizesOpensNoFiles()
{
    // The claim ARCHITECTURE.md makes about the bar over a listing, held for the
    // step version of it: a filter over what a listing already says is free. Not
    // "cheap" -- free, and the only way to hold that is to count the opens on a
    // drive that would report any.
    ChainStep place = stepOf(PlaceSource::stepKind());
    place.parameters = { { PlaceSource::whereKey(), QStringLiteral("mem:///reports") } };

    m_drive->forgetCounts();
    ChainTask* task = run({ place, stepOf(QStringLiteral("passthrough")), filterOn(onlyText()) });
    QVERIFY(task);
    QStringList produced = task->produced();
    produced.sort();
    QCOMPARE(produced,
        QStringList({ QStringLiteral("mem:///reports/a.txt"), QStringLiteral("mem:///reports/deep/c.txt") }));

    // One stat per entry it was handed, and not one byte read.
    QVERIFY2(m_drive->statCount() > 0, "the filter answered without asking the drive anything");
    QCOMPARE(m_drive->openReadCount(), 0);
    QCOMPARE(m_drive->bytesRead(), 0);
}

void TestChainSources::aFilterThatReachesIntoContentsAnswersTheSameWhereverItSits()
{
    // **The fault this case exists for.** A content criterion cannot be answered
    // without a reader, and a reader is something the step has to build -- so the
    // filter fused into the place answered correctly, from the reader PlaceSource
    // builds, while the same criteria one step further down answered "nothing
    // matches" for a query with a match. Two positions, two answers, and the
    // position is not supposed to be visible at all. See MOLE-168.
    ChainStep place = stepOf(PlaceSource::stepKind());
    place.parameters = { { PlaceSource::whereKey(), QStringLiteral("mem:///reports") } };
    const ChainStep filter = filterOn(onlyContaining(QStringLiteral("three")));

    ChainTask* fused = run({ place, filter });
    QVERIFY(fused);
    QStringList fromFirst = fused->produced();
    fromFirst.sort();

    ChainTask* apart = run({ place, stepOf(QStringLiteral("passthrough")), filter });
    QVERIFY(apart);
    QStringList fromFourth = apart->produced();
    fromFourth.sort();

    // "three" is the contents of deep/c.txt and of nothing else under /reports.
    QCOMPARE(fromFirst, QStringList({ QStringLiteral("mem:///reports/deep/c.txt") }));
    QCOMPARE(fromFourth, fromFirst);
}

void TestChainSources::aFilterSaysWhetherItWillOpenTheFilesItIsGiven()
{
    // What a person has to be told before leaving a chain on a clock: reading every
    // candidate is a different proposition from reading a listing, and on a remote
    // drive reading is downloading. Derived from the plan rather than from the text
    // of the criteria, so it is the same fact the search view shows.
    QVERIFY(!m_filter->readsFileContents(filterOn(onlyText())));
    QVERIFY(m_filter->readsFileContents(filterOn(onlyContaining(QStringLiteral("three")))));

    // A filter with no criteria at all keeps everything and reads nothing, which is
    // not the same as a filter whose criteria would not load -- that one refuses,
    // and refusing is not a cost worth announcing either.
    QVERIFY(!m_filter->readsFileContents(filterOn(SearchQuery())));
    QVERIFY(!m_filter->readsFileContents(stepOf(FilterStep::stepKind())));

    // And every other kind answers false without having to think about it, which is
    // what the default on IChainStepKind is for.
    QVERIFY(!m_place->readsFileContents(stepOf(PlaceSource::stepKind())));
    QVERIFY(!m_passThrough->readsFileContents(stepOf(QStringLiteral("passthrough"))));
}

void TestChainSources::aChainStartedFromWhatIsSelectedUsesIt()
{
    const QStringList selected { QStringLiteral("mem:///reports/a.txt"),
        QStringLiteral("mem:///reports/b.pdf") };
    ChainTask* task = run({ stepOf(ListingSource::stepKind()) }, selected);
    QVERIFY(task);
    QCOMPARE(task->produced(), selected);
    QVERIFY2(m_drive->listCount() == 0, "a list already in hand was walked for again");
}

void TestChainSources::aChainStartedFromNothingSaysSoRatherThanActingOnEverything()
{
    // What a scheduled run of this chain looks like: nothing handed it a list. The
    // honest answer is to stop and say why -- a chain whose first step is "what I
    // was looking at" has no meaning at three in the morning.
    ChainTask* task = run({ stepOf(ListingSource::stepKind()), stepOf(QStringLiteral("passthrough")) });
    QVERIFY(task);
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->ending() == ChainTask::Ending::StoppedEmpty);
    QVERIFY2(task->endedBecause().contains(QStringLiteral("Nothing was selected")),
        qPrintable(task->endedBecause()));
}

MOLE_TEST_MAIN(TestChainSources)

#include "tst_ChainSources.moc"
