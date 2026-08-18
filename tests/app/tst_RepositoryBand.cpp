#include "plugins/builtin/BrowserFeature.h"
#include "support/GitFixture.h"
#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"
#include "core/vcs/Repository.h"

#include <QDir>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTest>

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
    /// How tall the listing is right now, which is the measurement the "no strip
    /// reserving height" claim is made of.
    qreal listingHeight() const;
    /// Builds a checkout at `relativePath` inside the harness fixture.
    std::unique_ptr<GitFixture> checkoutAt(
        const QString& relativePath, const QString& branch = QStringLiteral("main"));

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
