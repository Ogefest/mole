#include "support/GitFixture.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/vcs/ReadRepositoryTask.h"
#include "core/vcs/ReadStatusTask.h"
#include "core/vcs/Repository.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QThread>

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

    void theWalkNamesWhatHasChangedAndCountsIt();
    void anIgnoredFileIsNeitherMarkedNorCounted();
    void aDirectoryIsMarkedForWhatIsInsideItAtEveryLevel();
    void anUntrackedFolderIsOneChangeRatherThanItsContents();
    void aCancelledWalkAnswersWithNothingRatherThanWithHalf();
    void aBranchSaysHowFarItIsFromWhatItTracks();
    void aBranchWithNoUpstreamSaysThatRatherThanZero();
    void aBranchLevelWithItsUpstreamCountsNothing();
    void theLastCommitIsNamedAndDated();
    void aRepositoryWithNoCommitsHasNoCommitToName();

    void aStagedRenameIsNamedAsOne();
    void theWalkTaskCountsAndCaches();
    void aSecondWalkOfOneWorkTreeTakesTheAnswerAlreadyThere();
    void aCancelledWalkTaskSaysCancelledAndTellsNobody();

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
    // And the status cache holds an answer per work tree, keyed by a path the next
    // temporary directory could be given again.
    RepositoryStatusCache::shared().clear();
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

void TestRepository::theWalkNamesWhatHasChangedAndCountsIt()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("kept.txt"), "kept"));
    QVERIFY(repository.writeFile(QStringLiteral("edited.txt"), "before"));
    QVERIFY(repository.writeFile(QStringLiteral("removed.txt"), "gone soon"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());

    QVERIFY(repository.writeFile(QStringLiteral("edited.txt"), "after"));
    QVERIFY(repository.removeFile(QStringLiteral("removed.txt")));
    QVERIFY(repository.writeFile(QStringLiteral("fresh.txt"), "new"));
    QVERIFY(repository.writeFile(QStringLiteral("staged.txt"), "new and staged"));
    QVERIFY(repository.stageAll());

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryStatus status = handle->readStatus(CancelToken {});

    QVERIFY(status.complete);
    const QString root = handle->root();
    // stageAll() staged the lot, so what was an edit is a staged edit and what was
    // untracked is added. Both are folded into one answer per path on purpose --
    // Mole shows what differs from the last commit, and cannot unstage anything.
    QVERIFY((status.stateFor(root + QStringLiteral("/edited.txt")) & RepositoryModified) != 0);
    QVERIFY((status.stateFor(root + QStringLiteral("/removed.txt")) & RepositoryDeleted) != 0);
    QVERIFY((status.stateFor(root + QStringLiteral("/fresh.txt")) & RepositoryAdded) != 0);
    QVERIFY((status.stateFor(root + QStringLiteral("/staged.txt")) & RepositoryAdded) != 0);

    // The file nobody touched is absent from the map rather than present and zero,
    // which is what keeps the map the size of the changes instead of the checkout.
    QVERIFY(!status.byPath.contains(root + QStringLiteral("/kept.txt")));
    QCOMPARE(status.stateFor(root + QStringLiteral("/kept.txt")), int(RepositoryUnchanged));

    QCOMPARE(status.changedCount, 4);
}

void TestRepository::anIgnoredFileIsNeitherMarkedNorCounted()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral(".gitignore"), "*.log\nbuild/\n"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());

    QVERIFY(repository.writeFile(QStringLiteral("noise.log"), "ignored"));
    QVERIFY(repository.writeFile(QStringLiteral("build/artefact.o"), "ignored"));
    QVERIFY(repository.writeFile(QStringLiteral("real.txt"), "counted"));

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryStatus status = handle->readStatus(CancelToken {});
    const QString root = handle->root();

    QVERIFY(status.complete);
    QCOMPARE(status.changedCount, 1);
    QVERIFY((status.stateFor(root + QStringLiteral("/real.txt")) & RepositoryUntracked) != 0);
    QCOMPARE(status.stateFor(root + QStringLiteral("/noise.log")), int(RepositoryUnchanged));
    QCOMPARE(status.stateFor(root + QStringLiteral("/build")), int(RepositoryUnchanged));
    // The roll-up must not reach a directory whose only contents are ignored
    // either, or a build tree would mark every folder above it.
    QCOMPARE(status.stateFor(root + QStringLiteral("/build/artefact.o")), int(RepositoryUnchanged));
}

void TestRepository::aDirectoryIsMarkedForWhatIsInsideItAtEveryLevel()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("src/deep/down/here.txt"), "before"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(repository.writeFile(QStringLiteral("src/deep/down/here.txt"), "after"));

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryStatus status = handle->readStatus(CancelToken {});
    const QString root = handle->root();

    QVERIFY(status.complete);
    // One change, and three directories that have to say something about it --
    // otherwise a pane at the root of a checkout shows a clean-looking list of
    // folders over a tree full of edits.
    QCOMPARE(status.changedCount, 1);
    for (const QString& folder :
        { QStringLiteral("/src"), QStringLiteral("/src/deep"), QStringLiteral("/src/deep/down") }) {
        QVERIFY2((status.stateFor(root + folder) & RepositoryContainsChanges) != 0,
            qPrintable(folder + QStringLiteral(" does not say that something inside it changed")));
    }

    // A directory carries only the roll-up: which of the six states is inside is
    // not a question that aggregates into a true answer.
    QCOMPARE(status.stateFor(root + QStringLiteral("/src")), int(RepositoryContainsChanges));
    // And the work tree root is not in the map. A pane showing it is inside the
    // checkout; no row in any listing stands for it.
    QVERIFY(!status.byPath.contains(root));
}

void TestRepository::anUntrackedFolderIsOneChangeRatherThanItsContents()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("tracked.txt"), "x"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());

    for (int i = 0; i < 5; ++i)
        QVERIFY(repository.writeFile(QStringLiteral("brand-new/file%1.txt").arg(i), "x"));

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryStatus status = handle->readStatus(CancelToken {});
    const QString root = handle->root();

    QVERIFY(status.complete);
    // What `git status` itself says: one untracked directory, not five untracked
    // files. The row a listing has for it is the folder.
    QCOMPARE(status.changedCount, 1);
    QVERIFY((status.stateFor(root + QStringLiteral("/brand-new")) & RepositoryUntracked) != 0);
    QCOMPARE(status.stateFor(root + QStringLiteral("/brand-new/file0.txt")), int(RepositoryUnchanged));
}

void TestRepository::aCancelledWalkAnswersWithNothingRatherThanWithHalf()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    for (int i = 0; i < 20; ++i)
        QVERIFY(repository.writeFile(QStringLiteral("file%1.txt").arg(i), "x"));

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);

    // Cancelled before the walk begins, which is the state the callback polls for.
    // No clock anywhere: the token is already set, so the first path aborts it.
    CancelToken cancelled;
    cancelled.cancel();
    const RepositoryStatus abandoned = handle->readStatus(cancelled);

    QVERIFY(!abandoned.complete);
    // Empty rather than partial. Half a walk would mark half a listing correctly
    // and the other half as clean, and a listing that says a changed file is
    // unchanged is worse than one that says nothing.
    QVERIFY(abandoned.byPath.isEmpty());
    QCOMPARE(abandoned.changedCount, 0);

    // And the same repository still answers properly afterwards -- an abandoned
    // walk leaves nothing behind on the handle.
    const RepositoryStatus full = handle->readStatus(CancelToken {});
    QVERIFY(full.complete);
    QCOMPARE(full.changedCount, 20);
}

void TestRepository::aBranchSaysHowFarItIsFromWhatItTracks()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init(QStringLiteral("main")));
    QVERIFY(repository.writeFile(QStringLiteral("shared.txt"), "base\n"));
    QVERIFY(!repository.commitAll(QStringLiteral("shared history")).isEmpty());

    // trunk forks here, then both sides move: main gains two commits, trunk one.
    QVERIFY(repository.createBranch(QStringLiteral("trunk")));
    QVERIFY(repository.checkoutBranch(QStringLiteral("trunk")));
    QVERIFY(repository.writeFile(QStringLiteral("on-trunk.txt"), "theirs\n"));
    QVERIFY(!repository.commitAll(QStringLiteral("one on trunk")).isEmpty());

    QVERIFY(repository.checkoutBranch(QStringLiteral("main")));
    QVERIFY(repository.writeFile(QStringLiteral("one.txt"), "mine\n"));
    QVERIFY(!repository.commitAll(QStringLiteral("first on main")).isEmpty());
    QVERIFY(repository.writeFile(QStringLiteral("two.txt"), "mine again\n"));
    QVERIFY(!repository.commitAll(QStringLiteral("second on main")).isEmpty());

    QVERIFY(repository.setUpstream(QStringLiteral("main"), QStringLiteral("trunk")));

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryHead head = handle->head();

    QCOMPARE(head.branch, QStringLiteral("main"));
    QVERIFY(head.hasUpstream);
    QCOMPARE(head.ahead, 2);
    QCOMPARE(head.behind, 1);
}

void TestRepository::aBranchWithNoUpstreamSaysThatRatherThanZero()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init(QStringLiteral("main")));
    QVERIFY(repository.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(!repository.commitAll(QStringLiteral("only commit")).isEmpty());

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryHead head = handle->head();

    // The counts are nought either way, so the flag is the whole difference between
    // "level with the remote" and "there is no remote to be level with".
    QVERIFY(!head.hasUpstream);
    QCOMPARE(head.ahead, 0);
    QCOMPARE(head.behind, 0);
}

void TestRepository::aBranchLevelWithItsUpstreamCountsNothing()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init(QStringLiteral("main")));
    QVERIFY(repository.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(!repository.commitAll(QStringLiteral("only commit")).isEmpty());
    QVERIFY(repository.createBranch(QStringLiteral("trunk")));
    QVERIFY(repository.setUpstream(QStringLiteral("main"), QStringLiteral("trunk")));

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryHead head = handle->head();

    QVERIFY(head.hasUpstream);
    QCOMPARE(head.ahead, 0);
    QCOMPARE(head.behind, 0);
}

void TestRepository::theLastCommitIsNamedAndDated()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init(QStringLiteral("main")));
    QVERIFY(repository.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(!repository.commitAll(QStringLiteral("the first thing that happened")).isEmpty());
    QVERIFY(repository.writeFile(QStringLiteral("b.txt"), "b"));
    const QString second = repository.commitAll(QStringLiteral("what I was doing last"));
    QVERIFY(!second.isEmpty());

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryHead head = handle->head();

    // The commit HEAD is on, not the first one: this is how somebody recognises
    // where they left off.
    QCOMPARE(head.shortId, second);
    QCOMPARE(head.subject, QStringLiteral("what I was doing last"));
    QVERIFY(head.committedAt.isValid());
    // The exact instant the fixture stamps, rather than a window around "now": a date
    // read with the wrong offset, or in the wrong units, lands somewhere else entirely
    // and a comparison against a constant says so.
    QCOMPARE(head.committedAt.toSecsSinceEpoch(), GitFixture::kCommitTime);
}

void TestRepository::aRepositoryWithNoCommitsHasNoCommitToName()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init(QStringLiteral("main")));

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryHead head = handle->head();

    // The branch exists as a name with nothing to point at. An invalid date is what
    // tells the band to draw no commit line rather than an empty one.
    QVERIFY(head.unborn);
    QCOMPARE(head.branch, QStringLiteral("main"));
    QVERIFY(head.subject.isEmpty());
    QVERIFY(!head.committedAt.isValid());
    QVERIFY(!head.hasUpstream);
}

void TestRepository::aStagedRenameIsNamedAsOne()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("moved.txt"), "content that stays the same\n"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());

    QVERIFY(repository.removeFile(QStringLiteral("moved.txt")));
    QVERIFY(repository.writeFile(QStringLiteral("elsewhere.txt"), "content that stays the same\n"));
    QVERIFY(repository.stageAll());

    const std::shared_ptr<Repository> handle = RepositoryCache::shared().forPath(tree.path());
    QVERIFY(handle);
    const RepositoryStatus status = handle->readStatus(CancelToken {});
    const QString root = handle->root();

    QVERIFY(status.complete);
    // One change, not two: git works a rename out by matching content, and a move is
    // one thing that happened rather than a deletion and an addition.
    QCOMPARE(status.changedCount, 1);

    // On the destination, which is the file that is on disk and therefore the only
    // one with a row in a listing. libgit2's per-path callback hands over the source
    // instead, which is why this walk reads the deltas rather than using it.
    QCOMPARE(
        repositoryStateMark(status.stateFor(root + QStringLiteral("/elsewhere.txt"))), QStringLiteral("R"));
    QVERIFY(!status.byPath.contains(root + QStringLiteral("/moved.txt")));
}

void TestRepository::theWalkTaskCountsAndCaches()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("src/a.txt"), "a"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(repository.writeFile(QStringLiteral("src/a.txt"), "changed"));

    TaskManager tasks;
    auto* task = new ReadStatusTask(repository.absolute(QStringLiteral("src")));
    // Housekeeping nobody asked for, marked the same way as QuerySpaceTask so it
    // cannot scroll the user's own copy off the task strip.
    QVERIFY(task->isBackground());

    QString seenRoot;
    RepositoryStatus seen;
    connect(
        task, &ReadStatusTask::statusRead, this, [&](const QString& root, const RepositoryStatus& status) {
            seenRoot = root;
            seen = status;
        });

    // A direct connection runs on whichever thread emitted, so this records the
    // thread the walk itself ran on. That is the rule the whole task framework
    // exists for -- the UI thread never waits on a disk -- and a stat of every file
    // in a checkout is exactly the work that would break it.
    std::atomic<QThread*> walkedOn { nullptr };
    connect(
        task, &ReadStatusTask::statusRead, this,
        [&walkedOn](const QString&, const RepositoryStatus&) { walkedOn = QThread::currentThread(); },
        Qt::DirectConnection);

    tasks.submit(task);
    QVERIFY(waitForTask(task));
    QCOMPARE(task->state(), Task::State::Succeeded);

    QVERIFY2(walkedOn.load() != nullptr && walkedOn.load() != QThread::currentThread(),
        "the work tree walk ran on the thread that draws the window");

    QCOMPARE(canonical(seenRoot), canonical(tree.path()));
    QCOMPARE(seen.changedCount, 1);
    QVERIFY(!task->wasCached());
    QVERIFY((seen.stateFor(task->root() + QStringLiteral("/src/a.txt")) & RepositoryModified) != 0);

    // The walk covers the work tree rather than the folder it was handed, which is
    // what git status means and what lets one walk serve every folder.
    QVERIFY((seen.stateFor(task->root() + QStringLiteral("/src")) & RepositoryContainsChanges) != 0);
    // And it is in the cache, which is where the next pane finds it.
    QVERIFY(RepositoryStatusCache::shared().forRoot(task->root()).complete);
}

void TestRepository::aSecondWalkOfOneWorkTreeTakesTheAnswerAlreadyThere()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("src/a.txt"), "a"));
    QVERIFY(repository.writeFile(QStringLiteral("tests/b.txt"), "b"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(repository.writeFile(QStringLiteral("src/a.txt"), "changed"));

    TaskManager tasks;
    auto* first = new ReadStatusTask(repository.absolute(QStringLiteral("src")));
    tasks.submit(first);
    QVERIFY(waitForTask(first));
    QVERIFY(!first->wasCached());

    // The same checkout, reached from a different folder in it. Nothing walks
    // twice: navigating between folders inside one repository is the commonest
    // thing anybody does, and a stat of the whole tree per folder would make the
    // feature the most expensive thing in the window.
    auto* second = new ReadStatusTask(repository.absolute(QStringLiteral("tests")));
    tasks.submit(second);
    QVERIFY(waitForTask(second));
    QVERIFY(second->wasCached());
    QCOMPARE(second->status().changedCount, first->status().changedCount);
    QCOMPARE(canonical(second->root()), canonical(first->root()));
}

void TestRepository::aCancelledWalkTaskSaysCancelledAndTellsNobody()
{
    MOLE_REQUIRE_GIT();

    TempTree tree;
    QVERIFY(tree.isValid());
    GitFixture repository(tree.path());
    QVERIFY(repository.init());
    QVERIFY(repository.writeFile(QStringLiteral("a.txt"), "a"));
    QVERIFY(!repository.commitAll(QStringLiteral("first")).isEmpty());
    QVERIFY(repository.writeFile(QStringLiteral("a.txt"), "changed"));

    TaskManager tasks;
    auto* task = new ReadStatusTask(tree.path());

    int answers = 0;
    connect(
        task, &ReadStatusTask::statusRead, this, [&](const QString&, const RepositoryStatus&) { ++answers; });

    // Cancelled before it is submitted, so there is no window to race with and no
    // clock to wait on. What is asserted is the end of the task, which is what
    // navigating away has to leave behind.
    task->requestCancel();
    tasks.submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Cancelled);
    // Nobody is told, and nothing is cached: an abandoned walk must not leave a
    // half-answer behind for the next pane to find and believe.
    QCOMPARE(answers, 0);
    QVERIFY(!RepositoryStatusCache::shared().forRoot(canonical(tree.path())).complete);
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
