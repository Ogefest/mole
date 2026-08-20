#include "plugins/builtin/BrowserFeature.h"
#include "support/GitFixture.h"
#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "ui/AppController.h"
#include "ui/models/FileListModel.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/vcs/Repository.h"

#include <QDir>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickStyle>
#include <QStringList>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

using namespace mole;
using namespace mole::test;

/// The band above the listing, through the real window.
///
/// The controller tests prove which branch is read; only this proves that a pane
/// showing a checkout has a strip saying so, and -- the half that is easier to get
/// wrong -- that a pane showing anything else looks exactly as it did before the
/// band existed, down to how tall the listing is.
///
/// Its own binary rather than more of the walkthrough: it builds repositories in
/// the fixture and navigates between them, which is state a narrative test would
/// have to step around.
class TestRepositoryBand : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aPaneInsideACheckoutNamesTheBranch();
    void aPaneOutsideOneHasNoBandAndTheListingTakesTheHeight();
    void aDetachedHeadSaysSo();
    void anInterruptedRebaseSaysSo();
    void movingBetweenTwoCheckoutsShowsEachBranch();
    void theBandCountsWhatHasChangedAndSaysWhenNothingHas();
    void aChangedRowIsMarkedInTheListing();
    void theBandNamesTheLastCommit();
    void aLongSubjectElidesRatherThanWrappingTheBand();
    void aRepositoryWithNoCommitsHasNoCommitLine();
    void aBranchAheadAndBehindSaysSoAndOneWithNoUpstreamSaysNothing();
    void theCountOpensAListNamingEveryPathGitReported();
    void theCountAndTheListAreTheSameNumber();
    void aDeletedFileIsNamedInTheListAndNowhereInTheListing();
    void activatingADeletedEntryLandsOnTheFolderThatHeldIt();
    void aCleanCheckoutHasNothingToOpen();

private:
    BrowserPaneController* pane() const;
    /// Navigates the active pane and waits until the band says `expectedHead` --
    /// an empty string meaning the band has gone away.
    void goTo(const QString& relativePath, const QString& expectedHead);
    /// The band belonging to the pane in view. Both panes have one; the second is
    /// not shown in a single view.
    QQuickItem* band() const;
    QString headText() const;
    /// What the band says about how much has changed, empty until the walk lands.
    QString changesText() const;
    /// Every git letter drawn in the listing right now, sorted so the assertion
    /// does not depend on what order the delegates were built in.
    QStringList markers() const;
    /// The text of the one visible label called `name`, empty when none is visible.
    QString labelText(const QString& name) const;
    /// How tall the listing is right now, which is the measurement the "no strip
    /// reserving height" claim is made of.
    qreal listingHeight() const;
    /// Builds a checkout at `relativePath` inside the harness fixture.
    std::unique_ptr<GitFixture> checkoutAt(
        const QString& relativePath, const QString& branch = QStringLiteral("main"));
    /// A checkout at `relativePath` carrying one path in each of the six states
    /// git reports -- conflicted, deleted, renamed, added, untracked, modified.
    ///
    /// All six in one work tree rather than six work trees, because the claim
    /// being made is about the *count*: the band says how many there are and the
    /// list says which, and a fixture holding one state at a time could never
    /// catch the two disagreeing.
    std::unique_ptr<GitFixture> checkoutInEveryState(const QString& relativePath);

    /// Opens the list behind the count, by clicking the count the way a reader
    /// would. Returns false when the band never offered one.
    bool openChangedPaths();
    /// What the open list holds, as "<mark> <path>", in the order it draws them.
    QStringList changedPathRows() const;
    /// The same, straight off the model, so the band and the answer it is drawing
    /// can be compared rather than assumed equal.
    QStringList changedPathEntries() const;

    std::unique_ptr<QmlAppHarness> m_harness;
};

void TestRepositoryBand::init()
{
    if (!Repository::isSupported())
        QSKIP("built without libgit2");

    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));

    QVERIFY(m_harness->makeDirs(QStringLiteral("plain")));
    QVERIFY(m_harness->writeFile(QStringLiteral("plain/one.txt")));
    QVERIFY(m_harness->writeFile(QStringLiteral("plain/two.txt")));
}

void TestRepositoryBand::cleanup()
{
    m_harness.reset();
    // Every fixture was a directory that is being deleted now, and the cache holds
    // an open handle per repository.
    RepositoryCache::shared().clear();
    // And the walk's answer is cached per work tree, keyed by a path the next
    // fixture could be handed again.
    RepositoryStatusCache::shared().clear();
}

BrowserPaneController* TestRepositoryBand::pane() const
{
    auto* browser = qobject_cast<BrowserController*>(m_harness->app()->tabs()->currentController());
    return browser ? browser->activePane() : nullptr;
}

std::unique_ptr<GitFixture> TestRepositoryBand::checkoutAt(const QString& relativePath, const QString& branch)
{
    if (!m_harness->makeDirs(relativePath))
        return nullptr;
    auto fixture = std::make_unique<GitFixture>(QDir(m_harness->fixturePath()).filePath(relativePath));
    if (!fixture->init(branch))
        return nullptr;
    if (!fixture->writeFile(QStringLiteral("readme.md"), "hello"))
        return nullptr;
    if (fixture->commitAll(QStringLiteral("first")).isEmpty())
        return nullptr;
    return fixture;
}

std::unique_ptr<GitFixture> TestRepositoryBand::checkoutInEveryState(const QString& relativePath)
{
    if (!m_harness->makeDirs(relativePath))
        return nullptr;
    auto fixture = std::make_unique<GitFixture>(QDir(m_harness->fixturePath()).filePath(relativePath));
    if (!fixture->init(QStringLiteral("main")))
        return nullptr;

    // Everything the later states need, committed first, so that the rebase below
    // -- which checks a tree out -- has nothing of ours to trample.
    const bool seeded = fixture->writeFile(QStringLiteral("readme.md"), "base\n")
        && fixture->writeFile(QStringLiteral("edited.txt"), "before\n")
        && fixture->writeFile(QStringLiteral("gone.txt"), "here for now\n")
        && fixture->writeFile(QStringLiteral("before.txt"), "renamed shortly\n")
        && !fixture->commitAll(QStringLiteral("first")).isEmpty();
    if (!seeded)
        return nullptr;

    // `U`. A real conflict rather than an index written by hand: two branches
    // rewrite the same line, and a rebase stops on it. That is what git does, and
    // a fixture that only resembled it would be testing our idea of a conflict.
    const bool conflicted = fixture->createBranch(QStringLiteral("topic"))
        && fixture->checkoutBranch(QStringLiteral("topic"))
        && fixture->writeFile(QStringLiteral("readme.md"), "the topic line\n")
        && !fixture->commitAll(QStringLiteral("on topic")).isEmpty()
        && fixture->checkoutBranch(QStringLiteral("main"))
        && fixture->writeFile(QStringLiteral("readme.md"), "the main line\n")
        && !fixture->commitAll(QStringLiteral("on main")).isEmpty()
        && fixture->beginRebase(QStringLiteral("topic"), QStringLiteral("main"));
    if (!conflicted)
        return nullptr;

    const bool rest = fixture->writeFile(QStringLiteral("edited.txt"), "after\n") // M
        && fixture->removeFile(QStringLiteral("gone.txt")) // D
        && fixture->writeFile(QStringLiteral("stray.txt"), "nobody staged me\n") // ??
        && fixture->writeFile(QStringLiteral("added.txt"), "brand new\n") // A
        && fixture->stagePath(QStringLiteral("added.txt"))
        && fixture->renameFile(QStringLiteral("before.txt"), QStringLiteral("after.txt")); // R
    if (!rest)
        return nullptr;
    return fixture;
}

bool TestRepositoryBand::openChangedPaths()
{
    // Says which step failed. "The count did not open" is a sentence with three
    // causes behind it, and this used to fail about once in fifty runs under load
    // with no way to tell them apart -- the failing run's output was filtered to
    // its summary line and the assertion was lost for a fortnight. See MOLE-256.
    QQuickItem* count = nullptr;
    for (QQuickItem* label : m_harness->items(QStringLiteral("repositoryChanges"))) {
        if (label->isVisible())
            count = label;
    }
    if (!count) {
        qWarning().noquote() << "openChangedPaths: no visible repositoryChanges label;"
                             << m_harness->items(QStringLiteral("repositoryChanges")).size()
                             << "exist, band text is" << changesText();
        return false;
    }

    // clickOn() rather than click(centreOf()): the label has only just become
    // visible, and reading its position before the layout pass places it gives the
    // top of its parent -- which is the pane's toolbar, whose back button then took
    // the click and navigated away. That is the whole of MOLE-256.
    if (!m_harness->clickOn(count)) {
        qWarning().noquote() << "openChangedPaths: the label never stopped moving";
        return false;
    }
    if (m_harness->until([this] { return !changedPathRows().isEmpty(); }))
        return true;

    qWarning().noquote() << "openChangedPaths: clicked the label at" << count->mapToScene(QPointF(0, 0))
                         << "but no row appeared;"
                         << m_harness->items(QStringLiteral("repositoryChangedPath")).size()
                         << "exist, the model has" << changedPathEntries().size() << "entries, band text is"
                         << changesText();
    return false;
}

QStringList TestRepositoryBand::changedPathRows() const
{
    QStringList out;
    for (QQuickItem* row : m_harness->items(QStringLiteral("repositoryChangedPath"))) {
        if (row->isVisible())
            out.append(row->property("entryText").toString());
    }
    return out;
}

QStringList TestRepositoryBand::changedPathEntries() const
{
    QStringList out;
    if (!pane())
        return out;
    const QVariantList entries = pane()->repository()->changedPaths();
    for (const QVariant& entry : entries) {
        const QVariantMap fields = entry.toMap();
        out.append(fields.value(QStringLiteral("mark")).toString() + QLatin1Char(' ')
            + fields.value(QStringLiteral("path")).toString());
    }
    return out;
}

void TestRepositoryBand::goTo(const QString& relativePath, const QString& expectedHead)
{
    const QString uri = m_harness->fixtureUri() + QStringLiteral("/") + relativePath;
    pane()->navigateTo(uri);
    QVERIFY(m_harness->until([this, relativePath] {
        return pane()->currentUri().endsWith(relativePath) && !pane()->isLoading();
    }));
    // The git read is a task of its own and lands after the listing, so what is
    // waited on is the band's own text. A number of rounds of the event loop would
    // be waiting on a clock: enough on this machine and not enough on a slower one,
    // and an intermittent test teaches everybody to ignore red.
    QVERIFY2(m_harness->until([this, expectedHead] { return headText() == expectedHead; }),
        qPrintable(
            QStringLiteral("the band never said \"%1\"; it says \"%2\"").arg(expectedHead, headText())));
}

QQuickItem* TestRepositoryBand::band() const
{
    for (QQuickItem* candidate : m_harness->items(QStringLiteral("repositoryBand"))) {
        // The pane that is not shown has one too; the one on screen is the one
        // whose window position is inside the window.
        if (candidate->window() && candidate->parentItem() && candidate->parentItem()->isVisible())
            return candidate;
    }
    return nullptr;
}

QString TestRepositoryBand::headText() const
{
    for (QQuickItem* label : m_harness->items(QStringLiteral("repositoryHead"))) {
        if (label->isVisible())
            return label->property("text").toString();
    }
    return {};
}

QString TestRepositoryBand::changesText() const
{
    for (QQuickItem* label : m_harness->items(QStringLiteral("repositoryChanges"))) {
        if (label->isVisible())
            return label->property("text").toString();
    }
    return {};
}

QString TestRepositoryBand::labelText(const QString& name) const
{
    for (QQuickItem* label : m_harness->items(name)) {
        if (label->isVisible())
            return label->property("text").toString();
    }
    return {};
}

QStringList TestRepositoryBand::markers() const
{
    QStringList out;
    for (QQuickItem* marker : m_harness->items(QStringLiteral("gitMarker"))) {
        if (marker->isVisible())
            out.append(marker->property("text").toString());
    }
    out.sort();
    return out;
}

qreal TestRepositoryBand::listingHeight() const
{
    QQuickItem* list = m_harness->item(QStringLiteral("fileList"));
    return list ? list->height() : -1;
}

void TestRepositoryBand::aPaneInsideACheckoutNamesTheBranch()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);

    goTo(QStringLiteral("work"), QStringLiteral("main"));

    QQuickItem* strip = band();
    QVERIFY(strip);
    QVERIFY2(strip->isVisible(), "a folder inside a checkout has a band");
    QCOMPARE(headText(), QStringLiteral("main"));
}

void TestRepositoryBand::aPaneOutsideOneHasNoBandAndTheListingTakesTheHeight()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);

    goTo(QStringLiteral("plain"), QString());
    QVERIFY(band());
    QVERIFY2(!band()->isVisible(), "a folder that is not a checkout has no band");
    const qreal withoutBand = listingHeight();
    QVERIFY(withoutBand > 0);

    // The band appearing costs the listing height, and the layout settles a frame
    // after the answer arrives -- so the height is waited for rather than read once.
    goTo(QStringLiteral("work"), QStringLiteral("main"));
    QVERIFY(band()->isVisible());
    QVERIFY2(m_harness->until([this, withoutBand] { return listingHeight() < withoutBand; }),
        "the band takes its height from the listing while it is there");

    // And gives every pixel back. An empty strip left behind would show up here as
    // a listing that never returned to the height it had.
    goTo(QStringLiteral("plain"), QString());
    QVERIFY(!band()->isVisible());
    QVERIFY(m_harness->until([this, withoutBand] { return listingHeight() == withoutBand; }));
    QCOMPARE(listingHeight(), withoutBand);
}

void TestRepositoryBand::aDetachedHeadSaysSo()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);
    QVERIFY(checkout->writeFile(QStringLiteral("second.txt"), "2"));
    const QString second = checkout->commitAll(QStringLiteral("second"));
    QVERIFY(!second.isEmpty());
    QVERIFY(checkout->detachHead());

    goTo(QStringLiteral("work"), QStringLiteral("detached at %1").arg(second));

    QVERIFY(band()->isVisible());
    QCOMPARE(headText(), QStringLiteral("detached at %1").arg(second));
}

void TestRepositoryBand::anInterruptedRebaseSaysSo()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);
    QVERIFY(checkout->createBranch(QStringLiteral("topic")));
    QVERIFY(checkout->checkoutBranch(QStringLiteral("topic")));
    QVERIFY(checkout->writeFile(QStringLiteral("readme.md"), "hello\ntopic\n"));
    QVERIFY(!checkout->commitAll(QStringLiteral("on topic")).isEmpty());
    QVERIFY(checkout->checkoutBranch(QStringLiteral("main")));
    QVERIFY(checkout->writeFile(QStringLiteral("elsewhere.txt"), "main\n"));
    QVERIFY(!checkout->commitAll(QStringLiteral("on main")).isEmpty());
    QVERIFY(checkout->beginRebase(QStringLiteral("topic"), QStringLiteral("main")));

    goTo(QStringLiteral("work"), QStringLiteral("rebasing"));

    QVERIFY(band()->isVisible());
    // The state, not the branch: during a rebase the branch name is either the old
    // one or a detached head, and both readings are wrong about what is going on.
    QCOMPARE(headText(), QStringLiteral("rebasing"));
}

void TestRepositoryBand::movingBetweenTwoCheckoutsShowsEachBranch()
{
    const std::unique_ptr<GitFixture> one = checkoutAt(QStringLiteral("one"), QStringLiteral("main"));
    const std::unique_ptr<GitFixture> two = checkoutAt(QStringLiteral("two"), QStringLiteral("release"));
    QVERIFY(one);
    QVERIFY(two);

    goTo(QStringLiteral("one"), QStringLiteral("main"));
    QCOMPARE(headText(), QStringLiteral("main"));

    goTo(QStringLiteral("two"), QStringLiteral("release"));
    QCOMPARE(headText(), QStringLiteral("release"));

    // Back to the first, because a band that only ever changed once would pass the
    // two navigations above.
    goTo(QStringLiteral("one"), QStringLiteral("main"));
    QCOMPARE(headText(), QStringLiteral("main"));
}

void TestRepositoryBand::theBandCountsWhatHasChangedAndSaysWhenNothingHas()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);

    goTo(QStringLiteral("work"), QStringLiteral("main"));
    // Nothing has been touched since the fixture committed, so the band says so in
    // a word rather than counting to nought.
    QVERIFY2(m_harness->until([this] { return changesText() == QStringLiteral("clean"); }),
        qPrintable(QStringLiteral("the band says \"%1\" about a clean checkout").arg(changesText())));

    QVERIFY(checkout->writeFile(QStringLiteral("readme.md"), "hello, edited\n"));
    QVERIFY(checkout->writeFile(QStringLiteral("extra.txt"), "new\n"));
    // The walk's answer is cached per work tree, and nothing has told it that the
    // tree moved on -- that is MOLE-105's job, not this one's. Forgetting it by
    // hand is what makes this a test of the count rather than of the refresh.
    RepositoryStatusCache::shared().forget(QDir(m_harness->fixturePath()).filePath(QStringLiteral("work")));

    goTo(QStringLiteral("plain"), QString());
    goTo(QStringLiteral("work"), QStringLiteral("main"));
    QVERIFY2(m_harness->until([this] { return changesText() == QStringLiteral("2 changed"); }),
        qPrintable(QStringLiteral("the band says \"%1\" about two changes").arg(changesText())));

    // And a folder in no checkout has no band at all, so nothing to say about it.
    goTo(QStringLiteral("plain"), QString());
    QVERIFY(!band()->isVisible());
    QVERIFY(changesText().isEmpty());
}

void TestRepositoryBand::aChangedRowIsMarkedInTheListing()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);
    QVERIFY(checkout->writeFile(QStringLiteral("readme.md"), "hello, edited\n"));
    QVERIFY(checkout->writeFile(QStringLiteral("fresh.txt"), "new\n"));

    goTo(QStringLiteral("work"), QStringLiteral("main"));

    // The model tests prove which letter each state gets; this proves a letter ever
    // reaches the screen -- the delegate is where a role that nothing binds to looks
    // exactly like a role that works.
    QVERIFY2(m_harness->until(
                 [this] { return markers() == QStringList { QStringLiteral("??"), QStringLiteral("M") }; }),
        qPrintable(QStringLiteral("the listing draws %1").arg(markers().join(QLatin1Char(' ')))));

    // And a folder in no checkout draws none at all, so there is no column there.
    goTo(QStringLiteral("plain"), QString());
    QVERIFY(m_harness->until([this] { return markers().isEmpty(); }));
}

void TestRepositoryBand::theBandNamesTheLastCommit()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);
    QVERIFY(checkout->writeFile(QStringLiteral("second.txt"), "2"));
    const QString second = checkout->commitAll(QStringLiteral("the thing I did last"));
    QVERIFY(!second.isEmpty());

    goTo(QStringLiteral("work"), QStringLiteral("main"));

    QVERIFY(m_harness->until(
        [this, second] { return labelText(QStringLiteral("repositoryCommitId")) == second; }));
    QCOMPARE(labelText(QStringLiteral("repositoryCommitSubject")), QStringLiteral("the thing I did last"));
    // The fixture stamps a fixed instant well in the past, so the age is a count of
    // days. What matters is that it is there and reads as an age.
    QVERIFY2(labelText(QStringLiteral("repositoryCommitAge")).endsWith(QStringLiteral("ago")),
        qPrintable(
            QStringLiteral("the age reads \"%1\"").arg(labelText(QStringLiteral("repositoryCommitAge")))));
}

void TestRepositoryBand::aLongSubjectElidesRatherThanWrappingTheBand()
{
    const std::unique_ptr<GitFixture> shortSubject = checkoutAt(QStringLiteral("terse"));
    QVERIFY(shortSubject);
    goTo(QStringLiteral("terse"), QStringLiteral("main"));
    QVERIFY(m_harness->until(
        [this] { return labelText(QStringLiteral("repositoryCommitSubject")) == QStringLiteral("first"); }));
    const qreal tidyHeight = band()->height();
    QVERIFY(tidyHeight > 0);

    // A subject far wider than any pane. It has to give way by eliding: a band that
    // grew a second line would take that height off the listing for good.
    const std::unique_ptr<GitFixture> wordy = checkoutAt(QStringLiteral("wordy"));
    QVERIFY(wordy);
    QVERIFY(wordy->writeFile(QStringLiteral("more.txt"), "x"));
    const QString subject
        = QStringLiteral("a commit message subject line written by somebody who had a great deal to say "
                         "about a very small change and no intention of stopping before the end of the pane");
    QVERIFY(!wordy->commitAll(subject).isEmpty());

    goTo(QStringLiteral("wordy"), QStringLiteral("main"));
    QVERIFY(m_harness->until([this] {
        return labelText(QStringLiteral("repositoryCommitSubject"))
            .startsWith(QStringLiteral("a commit message"));
    }));

    QCOMPARE(band()->height(), tidyHeight);
    // And the label really is narrower than the text it holds, which is what eliding
    // means -- otherwise this would pass on a window wide enough to fit the lot.
    QQuickItem* label = nullptr;
    for (QQuickItem* candidate : m_harness->items(QStringLiteral("repositoryCommitSubject"))) {
        if (candidate->isVisible())
            label = candidate;
    }
    QVERIFY(label);
    QVERIFY2(label->implicitWidth() > label->width(), "the subject fitted, so nothing was elided");
}

void TestRepositoryBand::aRepositoryWithNoCommitsHasNoCommitLine()
{
    // `git init` and nothing else: a branch that exists as a name with nothing to
    // point at.
    QVERIFY(m_harness->makeDirs(QStringLiteral("fresh")));
    GitFixture fresh(QDir(m_harness->fixturePath()).filePath(QStringLiteral("fresh")));
    QVERIFY(fresh.init(QStringLiteral("main")));

    goTo(QStringLiteral("fresh"), QStringLiteral("main"));

    // The band is there and names the branch, because that is true.
    QVERIFY(band()->isVisible());
    QCOMPARE(headText(), QStringLiteral("main"));
    // The commit line is not, because there is no commit -- an empty one would read as
    // a commit whose message nobody wrote.
    QVERIFY(labelText(QStringLiteral("repositoryCommitId")).isEmpty());
    QVERIFY(labelText(QStringLiteral("repositoryCommitSubject")).isEmpty());
    QVERIFY(labelText(QStringLiteral("repositoryCommitAge")).isEmpty());
}

void TestRepositoryBand::aBranchAheadAndBehindSaysSoAndOneWithNoUpstreamSaysNothing()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);

    // No upstream yet, so no counter at all -- not "0 ahead, 0 behind", which reads as
    // up to date when the truth is that there is nothing to compare against.
    goTo(QStringLiteral("work"), QStringLiteral("main"));
    QVERIFY(m_harness->until([this] { return !headText().isEmpty(); }));
    QVERIFY(labelText(QStringLiteral("repositoryTracking")).isEmpty());

    // Fork, move both sides, and make one track the other.
    QVERIFY(checkout->createBranch(QStringLiteral("trunk")));
    QVERIFY(checkout->checkoutBranch(QStringLiteral("trunk")));
    QVERIFY(checkout->writeFile(QStringLiteral("theirs.txt"), "t"));
    QVERIFY(!checkout->commitAll(QStringLiteral("one on trunk")).isEmpty());
    QVERIFY(checkout->checkoutBranch(QStringLiteral("main")));
    QVERIFY(checkout->writeFile(QStringLiteral("mine.txt"), "m"));
    QVERIFY(!checkout->commitAll(QStringLiteral("one on main")).isEmpty());
    QVERIFY(checkout->writeFile(QStringLiteral("mine-again.txt"), "m"));
    QVERIFY(!checkout->commitAll(QStringLiteral("two on main")).isEmpty());
    QVERIFY(checkout->setUpstream(QStringLiteral("main"), QStringLiteral("trunk")));

    goTo(QStringLiteral("plain"), QString());
    goTo(QStringLiteral("work"), QStringLiteral("main"));
    QVERIFY2(m_harness->until([this] {
        return labelText(QStringLiteral("repositoryTracking")) == QStringLiteral("2 ahead, 1 behind");
    }),
        qPrintable(
            QStringLiteral("the band says \"%1\"").arg(labelText(QStringLiteral("repositoryTracking")))));
}

void TestRepositoryBand::theCountOpensAListNamingEveryPathGitReported()
{
    const std::unique_ptr<GitFixture> checkout = checkoutInEveryState(QStringLiteral("work"));
    QVERIFY(checkout);

    // Part-way through a rebase, so the band names the state rather than the
    // branch -- which is a fact about this fixture and not what is being tested.
    goTo(QStringLiteral("work"), QStringLiteral("rebasing"));
    QVERIFY2(m_harness->until([this] { return changesText() == QStringLiteral("6 changed"); }),
        qPrintable(QStringLiteral("the band says \"%1\"").arg(changesText())));

    // Sorted by path, so the list reads the same twice, and relative to the work
    // tree root because a reader looking at one checkout already knows where it is.
    const QStringList expected {
        QStringLiteral("A added.txt"),
        QStringLiteral("R after.txt"),
        QStringLiteral("M edited.txt"),
        QStringLiteral("D gone.txt"),
        QStringLiteral("U readme.md"),
        QStringLiteral("?? stray.txt"),
    };
    QCOMPARE(changedPathEntries(), expected);

    // And it reaches the screen. A model nothing draws looks exactly like a model
    // that works, which is the half of this only the window can prove.
    QVERIFY2(openChangedPaths(), "the count did not open");
    QCOMPARE(changedPathRows(), expected);

    // The directories this walk rolled up are not in it: a folder marked because
    // something below it changed is Mole's own arithmetic, not git's report.
    QVERIFY(!changedPathEntries().contains(QStringLiteral("\u2022 ")));
}

void TestRepositoryBand::theCountAndTheListAreTheSameNumber()
{
    const std::unique_ptr<GitFixture> checkout = checkoutInEveryState(QStringLiteral("work"));
    QVERIFY(checkout);

    goTo(QStringLiteral("work"), QStringLiteral("rebasing"));
    QVERIFY(m_harness->until([this] { return !changesText().isEmpty(); }));

    // The band is the only door to these paths, so if the two ever disagree it is
    // lying about one of them -- either counting something nobody can reach, or
    // offering something it did not count.
    const int counted = pane()->repository()->changedCount();
    QCOMPARE(changedPathEntries().size(), counted);
    QCOMPARE(changesText(), QStringLiteral("%1 changed").arg(counted));
    QCOMPARE(counted, 6);
}

void TestRepositoryBand::aDeletedFileIsNamedInTheListAndNowhereInTheListing()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);
    QVERIFY(checkout->writeFile(QStringLiteral("gone.txt"), "here for now\n"));
    QVERIFY(!checkout->commitAll(QStringLiteral("second")).isEmpty());
    QVERIFY(checkout->removeFile(QStringLiteral("gone.txt")));

    goTo(QStringLiteral("work"), QStringLiteral("main"));
    QVERIFY(m_harness->until([this] { return changesText() == QStringLiteral("1 changed"); }));

    // The listing goes on showing what is on disk: no row, and so no `D` on one.
    // That is the decision ADR-0042 records, and this is what would catch it being
    // quietly reversed.
    QVERIFY(!markers().contains(QStringLiteral("D")));

    // And the name is reachable without leaving Mole, which is the whole point.
    QVERIFY2(openChangedPaths(), "the count did not open");
    QCOMPARE(changedPathRows(), QStringList { QStringLiteral("D gone.txt") });
}

void TestRepositoryBand::activatingADeletedEntryLandsOnTheFolderThatHeldIt()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);
    QVERIFY(checkout->makeDirs(QStringLiteral("notes")));
    QVERIFY(checkout->writeFile(QStringLiteral("notes/gone.txt"), "here for now\n"));
    // A second file in the same folder, so "the cursor is on nothing" is a claim
    // about the cursor rather than about an empty listing.
    QVERIFY(checkout->writeFile(QStringLiteral("notes/kept.txt"), "staying\n"));
    QVERIFY(!checkout->commitAll(QStringLiteral("second")).isEmpty());
    QVERIFY(checkout->removeFile(QStringLiteral("notes/gone.txt")));

    goTo(QStringLiteral("work"), QStringLiteral("main"));
    QVERIFY(m_harness->until([this] { return changesText() == QStringLiteral("1 changed"); }));
    QVERIFY2(openChangedPaths(), "the count did not open");
    QCOMPARE(changedPathRows(), QStringList { QStringLiteral("D notes/gone.txt") });

    QQuickItem* row = nullptr;
    for (QQuickItem* candidate : m_harness->items(QStringLiteral("repositoryChangedPath"))) {
        if (candidate->isVisible())
            row = candidate;
    }
    QVERIFY(row);
    // clickOn(), for the same reason as in openChangedPaths(): this row appeared a
    // moment ago inside a popup, and reading where it is before the layout has put
    // it there is what MOLE-256 was.
    QVERIFY(m_harness->clickOn(row));

    // A deleted file has nowhere to go, so it goes to the folder that held it --
    // which is the folder already carrying the roll-up dot.
    QVERIFY2(m_harness->until([this] {
        return pane()->currentUri().endsWith(QStringLiteral("work/notes")) && !pane()->isLoading();
    }),
        qPrintable(QStringLiteral("the pane is at \"%1\"").arg(pane()->currentUri())));

    // And on nothing, rather than on whatever happens to sort first there: a
    // cursor on notes/kept.txt would be pointing at a different file while looking
    // exactly like success.
    QVERIFY(m_harness->until([this] { return pane()->currentIndex() == -1; }));
    QVERIFY2(pane()->files()->rowCount() > 0, "the folder was empty, so this proved nothing");
}

void TestRepositoryBand::aCleanCheckoutHasNothingToOpen()
{
    const std::unique_ptr<GitFixture> checkout = checkoutAt(QStringLiteral("work"));
    QVERIFY(checkout);

    goTo(QStringLiteral("work"), QStringLiteral("main"));
    QVERIFY(m_harness->until([this] { return changesText() == QStringLiteral("clean"); }));

    // "clean" is a fact and not a door. A control that opens an empty list is
    // worse than no control, because pressing it teaches nothing.
    QVERIFY(pane()->repository()->changedPaths().isEmpty());
    QQuickItem* count = nullptr;
    for (QQuickItem* label : m_harness->items(QStringLiteral("repositoryChanges"))) {
        if (label->isVisible())
            count = label;
    }
    QVERIFY(count);
    QVERIFY(m_harness->clickOn(count));
    m_harness->settle();
    QVERIFY(changedPathRows().isEmpty());
}

// A real window, so a real QGuiApplication rather than the guiless one every
// other suite gets from MOLE_TEST_MAIN. Material as well, because how tall the
// listing is -- which is the measurement the "no strip reserving height" claim is
// made of -- depends on the style the application really runs under.
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();
    QQuickStyle::setStyle(QStringLiteral("Material"));

    TestRepositoryBand testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}
#include "tst_RepositoryBand.moc"
