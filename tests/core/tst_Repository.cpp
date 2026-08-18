#include "support/GitFixture.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/vcs/ReadRepositoryTask.h"
#include "core/vcs/Repository.h"

#include <QDir>
#include <QFileInfo>

using namespace mole;
using namespace mole::test;

/// Every case below needs a real repository, which a build without libgit2 cannot
/// make. Skipped rather than passed, so the suite reports that it did not run.
#define MOLE_REQUIRE_GIT()                                                                                   \
    do {                                                                                                     \
        if (!Repository::isSupported())                                                                      \
            QSKIP("built without libgit2");                                                                  \
    } while (false)

/// What git says about a folder, before anything of it is on screen.
///
/// Every fixture here is a repository built through libgit2 in a temporary
/// directory, so the suite needs no installed git, no network and nothing from
/// whoever is running it.
class TestRepository : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();

    void aPathInsideACheckoutAnswersWithItsRoot();
    void aPathOutsideAnyWorkTreeAnswersWithNothing();
    void theBranchIsNamed();
    void aRepositoryWithNoCommitsStillNamesItsBranch();
    void aDetachedHeadSaysSoRatherThanNamingNoBranch();
    void anInterruptedRebaseIsNamed();
    void oneHandleServesEveryFolderInOneWorkTree();
    void twoCheckoutsAreTwoRepositories();

    void theTaskAnswersWithTheRootAndTheBranch();
    void theTaskAnswersThatThereIsNoRepositoryRatherThanFailing();

    void withoutGitSupportNothingIsClaimed();

private:
    /// Both sides of a path comparison go through this: a temporary directory can
    /// sit under a symlink, and libgit2 answers with the path it resolved.
    static QString canonical(const QString& path) { return QFileInfo(path).canonicalFilePath(); }
};

void TestRepository::cleanup()
{
    // The cache holds an open handle per repository, and every fixture here is a
    // temporary directory that is about to be deleted.
    RepositoryCache::shared().clear();
}

void TestRepository::aPathInsideACheckoutAnswersWithItsRoot()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());

    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("readme.md"), "hello"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(repository.makeDirs(QStringLiteral("src/core/vcs")));

    const QString expected = canonical(tree.path());
    QCOMPARE(canonical(Repository::workTreeFor(tree.path())), expected);
    QCOMPARE(
        canonical(Repository::workTreeFor(repository.absolute(QStringLiteral("src/core/vcs")))), expected);
}

void TestRepository::aPathOutsideAnyWorkTreeAnswersWithNothing()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.makeDirs(QStringLiteral("just/folders")));

    QVERIFY(Repository::workTreeFor(tree.absolute(QStringLiteral("just/folders"))).isEmpty());
    QVERIFY(!RepositoryCache::shared().forPath(tree.path()));
}

void TestRepository::theBranchIsNamed()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());

    GitFixture repository(tree.path());
    QVERIFY(repository.init(QStringLiteral("main")));
    QVERIFY(repository.writeFile(QStringLiteral("readme.md"), "hello"));
    const QString committed = repository.commitAll(QStringLiteral("first"));
    QVERIFY(!committed.isEmpty());

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);

    const RepositoryHead head = handle->head();
    QVERIFY(head.isValid());
    QCOMPARE(head.branch, QStringLiteral("main"));
    QCOMPARE(head.shortId, committed);
    QVERIFY(!head.detached);
    QVERIFY(!head.unborn);
    QCOMPARE(head.state, RepositoryState::None);
    QVERIFY(head.stateText().isEmpty());
}

void TestRepository::aRepositoryWithNoCommitsStillNamesItsBranch()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());

    GitFixture repository(tree.path());
    QVERIFY(repository.init(QStringLiteral("trunk")));

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);

    const RepositoryHead head = handle->head();
    // Valid, and says which branch: `git init` and nothing else is a checkout
    // somebody is working in, not a folder with nothing to report.
    QVERIFY(head.isValid());
    QVERIFY(head.unborn);
    QCOMPARE(head.branch, QStringLiteral("trunk"));
    QVERIFY(head.shortId.isEmpty());
    QVERIFY(!head.detached);
}

void TestRepository::aDetachedHeadSaysSoRatherThanNamingNoBranch()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());

    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("one.txt"), "1"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(repository.writeFile(QStringLiteral("two.txt"), "2"));
    const QString second = repository.commitAll(QStringLiteral("second"));
    QVERIFY(!second.isEmpty());
    QVERIFY(repository.detachHead());

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);

    const RepositoryHead head = handle->head();
    QVERIFY(head.detached);
    // Not "on a branch called nothing", which is how an empty name reads.
    QVERIFY(head.branch.isEmpty());
    QCOMPARE(head.shortId, second);
    QVERIFY(head.isValid());
}

void TestRepository::anInterruptedRebaseIsNamed()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());

    GitFixture repository(tree.path());
    QVERIFY(repository.init(QStringLiteral("main")));
    QVERIFY(repository.writeFile(QStringLiteral("shared.txt"), "base\n"));
    QVERIFY(!repository.commitAll(QStringLiteral("base")).isEmpty());

    // Two branches that have both moved, which is the only situation a rebase has
    // anything to do.
    QVERIFY(repository.createBranch(QStringLiteral("topic")));
    QVERIFY(repository.checkoutBranch(QStringLiteral("topic")));
    QVERIFY(repository.writeFile(QStringLiteral("shared.txt"), "base\ntopic\n"));
    QVERIFY(!repository.commitAll(QStringLiteral("on topic")).isEmpty());

    QVERIFY(repository.checkoutBranch(QStringLiteral("main")));
    QVERIFY(repository.writeFile(QStringLiteral("elsewhere.txt"), "main\n"));
    QVERIFY(!repository.commitAll(QStringLiteral("on main")).isEmpty());

    QVERIFY(repository.beginRebase(QStringLiteral("topic"), QStringLiteral("main")));

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);

    const RepositoryHead head = handle->head();
    QCOMPARE(head.state, RepositoryState::Rebase);
    QCOMPARE(head.stateText(), QStringLiteral("rebasing"));
}

void TestRepository::oneHandleServesEveryFolderInOneWorkTree()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());

    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("src/a.txt"), "a"));
    QVERIFY(repository.writeFile(QStringLiteral("tests/b.txt"), "b"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());

    RepositoryCache& cache = RepositoryCache::shared();
    const std::shared_ptr<Repository> fromRoot = cache.forPath(tree.path());
    const std::shared_ptr<Repository> fromSrc = cache.forPath(repository.absolute(QStringLiteral("src")));
    const std::shared_ptr<Repository> fromTests = cache.forPath(repository.absolute(QStringLiteral("tests")));

    QVERIFY(fromRoot);
    // The same handle, not merely the same answer: opening is the expensive part,
    // and a status walk shared between two folders depends on this.
    QCOMPARE(fromSrc.get(), fromRoot.get());
    QCOMPARE(fromTests.get(), fromRoot.get());
}

void TestRepository::twoCheckoutsAreTwoRepositories()
{
    MOLE_REQUIRE_GIT();

    TempTree first;
    TempTree second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());

    GitFixture one(first.path());
    GitFixture two(second.path());
    QVERIFY(one.init(QStringLiteral("main")));
    QVERIFY(two.init(QStringLiteral("release")));
    QVERIFY(one.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(two.writeFile(QStringLiteral("b.txt"), "b"));
    QVERIFY(!one.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(!two.commitAll(QStringLiteral("first")).isEmpty());

    RepositoryCache& cache = RepositoryCache::shared();
    const std::shared_ptr<Repository> handleOne = cache.forPath(first.path());
    const std::shared_ptr<Repository> handleTwo = cache.forPath(second.path());
    QVERIFY(handleOne);
    QVERIFY(handleTwo);
    QVERIFY(handleOne.get() != handleTwo.get());
    QCOMPARE(handleOne->head().branch, QStringLiteral("main"));
    QCOMPARE(handleTwo->head().branch, QStringLiteral("release"));
}

void TestRepository::theTaskAnswersWithTheRootAndTheBranch()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());

    GitFixture repository(tree.path());
    QVERIFY(repository.init(QStringLiteral("main")));
    QVERIFY(repository.writeFile(QStringLiteral("src/a.txt"), "a"));
    const QString committed = repository.commitAll(QStringLiteral("first"));
    QVERIFY(!committed.isEmpty());

    TaskManager tasks;
    auto* task = new ReadRepositoryTask(repository.absolute(QStringLiteral("src")));
    // Housekeeping nobody asked for. QuerySpaceTask is marked the same way and for
    // the same reason: it must not scroll the user's own copy off the task strip.
    QVERIFY(task->isBackground());

    QString seenRoot;
    RepositoryHead seenHead;
    connect(task, &ReadRepositoryTask::repositoryRead, this,
        [&](const QString&, const QString& root, const RepositoryHead& head) {
            seenRoot = root;
            seenHead = head;
        });

    tasks.submit(task);
    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Succeeded);

    QCOMPARE(canonical(seenRoot), canonical(tree.path()));
    QCOMPARE(seenHead.branch, QStringLiteral("main"));
    QCOMPARE(seenHead.shortId, committed);
    // The same answer the task kept, so a caller that missed the signal is not
    // left guessing.
    QCOMPARE(canonical(task->root()), canonical(tree.path()));
    QCOMPARE(task->head().branch, QStringLiteral("main"));
}

void TestRepository::theTaskAnswersThatThereIsNoRepositoryRatherThanFailing()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.makeDirs(QStringLiteral("plain")));

    TaskManager tasks;
    auto* task = new ReadRepositoryTask(tree.absolute(QStringLiteral("plain")));

    int answers = 0;
    QString seenRoot = QStringLiteral("not touched");
    connect(task, &ReadRepositoryTask::repositoryRead, this,
        [&](const QString&, const QString& root, const RepositoryHead&) {
            ++answers;
            seenRoot = root;
        });

    tasks.submit(task);
    QVERIFY(waitForTask(task));

    // Succeeded, and it did answer: a folder in no work tree is the ordinary case,
    // and something has to hear about it or the band would go on showing the
    // branch of the checkout somebody has just navigated out of.
    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(answers, 1);
    QVERIFY(seenRoot.isEmpty());
    QVERIFY(!task->head().isValid());
}

void TestRepository::withoutGitSupportNothingIsClaimed()
{
#ifdef MOLE_HAVE_GIT2
    QSKIP("built with libgit2; this is the other build's case");
#else
    // Not merely absent: absent and saying so, without touching a disk. A build
    // with no libgit2 has to behave exactly as Mole did before it existed.
    QVERIFY(!Repository::isSupported());
    QVERIFY(Repository::workTreeFor(QDir::homePath()).isEmpty());
    QVERIFY(!RepositoryCache::shared().forPath(QDir::homePath()));
    QVERIFY(!Repository::open(QDir::homePath()));
#endif
}

MOLE_TEST_MAIN(TestRepository)
#include "tst_Repository.moc"
