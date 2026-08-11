#include "plugins/builtin/SearchFeatures.h"
#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// What a tab says about where it came from, driven through the real window.
///
/// A folder opened from a search is a detour: the results are the work, and the
/// folder is one row of them looked at more closely. Three folders further in,
/// which tab that list was in is a guess — so a browser opened from another tab
/// carries the way back to it, and the model tests below it prove that label
/// stays true as tabs are renamed and closed.
class TestTabStrip : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aBrowserOpenedFromASearchOffersTheWayBack();
    void pressingItReturnsToTheSearch();
    void aBrowserOpenedFromNothingOffersNothing();
    void theWayBackIsNamedAfterTheTabItLeadsTo();
    void closingTheSearchTakesTheWayBackWithIt();

private:
    /// A finished search over the fixture, and the row its tab sits at.
    LiveSearchController* search(const QString& text, int* rowOut);
    /// Reveals `relativePath` from the search tab, leaving a browser tab open.
    void revealFromSearch(int searchRow, const QString& relativePath);
    /// The way back on the tab in front of the user.
    ///
    /// Every tab keeps its own loaded view, so every tab has one of these and
    /// all but one of them are hidden — asking for "the" item would silently
    /// answer with whichever tab was opened first.
    QQuickItem* backBar() const;

    std::unique_ptr<QmlAppHarness> m_harness;
};

void TestTabStrip::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));
    QVERIFY(m_harness->writeFile(QStringLiteral("deep/down/needle.txt")));
}

void TestTabStrip::cleanup()
{
    m_harness.reset();
}

LiveSearchController* TestTabStrip::search(const QString& text, int* rowOut)
{
    TabsModel* tabs = m_harness->app()->tabs();
    const int row = tabs->openTab(QStringLiteral("mole.livesearch"));
    if (row < 0)
        return nullptr;
    if (rowOut)
        *rowOut = row;

    auto* controller = qobject_cast<LiveSearchController*>(tabs->controllerAt(row));
    if (!controller)
        return nullptr;
    controller->setRootUri(m_harness->fixtureUri());
    controller->setQueryText(text);
    controller->start();
    if (!m_harness->until([controller] { return !controller->isRunning(); }))
        return nullptr;
    m_harness->settle(3);
    return controller;
}

QQuickItem* TestTabStrip::backBar() const
{
    for (QQuickItem* bar : m_harness->items(QStringLiteral("backToOpener"))) {
        if (bar->isVisible())
            return bar;
    }
    return nullptr;
}

void TestTabStrip::revealFromSearch(int searchRow, const QString& relativePath)
{
    m_harness->app()->tabs()->setCurrentIndex(searchRow);
    m_harness->settle(3);
    m_harness->app()->revealFile(m_harness->fixtureUri() + QLatin1Char('/') + relativePath);
    m_harness->settle(5);
}

void TestTabStrip::aBrowserOpenedFromASearchOffersTheWayBack()
{
    int searchRow = -1;
    QVERIFY(search(QStringLiteral("needle"), &searchRow));

    // Nothing before: the tab open at startup was opened from nowhere.
    QQuickItem* before = backBar();
    QVERIFY2(!before || !before->isVisible(), "a tab opened from nothing offered a way back to it");

    revealFromSearch(searchRow, QStringLiteral("deep/down/needle.txt"));

    QVERIFY(m_harness->until([this] {
        QQuickItem* bar = backBar();
        return bar != nullptr && bar->isVisible();
    }));
}

void TestTabStrip::pressingItReturnsToTheSearch()
{
    int searchRow = -1;
    LiveSearchController* found = search(QStringLiteral("needle"), &searchRow);
    QVERIFY(found);
    revealFromSearch(searchRow, QStringLiteral("deep/down/needle.txt"));

    TabsModel* tabs = m_harness->app()->tabs();
    QVERIFY2(tabs->currentIndex() != searchRow, "the reveal never left the search tab");

    QQuickItem* bar = backBar();
    QVERIFY(bar);
    auto* button = bar->findChild<QQuickItem*>(QStringLiteral("backToOpenerButton"));
    QVERIFY(button);
    QMetaObject::invokeMethod(button, "clicked");
    m_harness->settle(3);

    QCOMPARE(tabs->currentIndex(), searchRow);
    // And the results are still the results -- going back to a search that has
    // forgotten what you asked it is the same loss as losing the tab.
    QCOMPARE(tabs->controllerAt(searchRow), found);
    QCOMPARE(found->results()->rowCount(), 1);
}

void TestTabStrip::aBrowserOpenedFromNothingOffersNothing()
{
    // A second browser opened from the first: goTo() navigates a browser in
    // place, so this is the deliberate "new tab" route, and a tab somebody
    // asked for outright is not a detour from anything.
    TabsModel* tabs = m_harness->app()->tabs();
    tabs->setCurrentIndex(0);
    m_harness->settle(3);

    QCOMPARE(tabs->index(0, 0).data(TabsModel::OpenerTitleRole).toString(), QString());
    QQuickItem* bar = backBar();
    QVERIFY(!bar || !bar->isVisible());
}

void TestTabStrip::theWayBackIsNamedAfterTheTabItLeadsTo()
{
    int searchRow = -1;
    QVERIFY(search(QStringLiteral("needle"), &searchRow));
    revealFromSearch(searchRow, QStringLiteral("deep/down/needle.txt"));

    TabsModel* tabs = m_harness->app()->tabs();
    const int browserRow = tabs->currentIndex();
    const QString searchTitle = tabs->index(searchRow, 0).data(TabsModel::TitleRole).toString();
    QVERIFY(!searchTitle.isEmpty());
    QCOMPARE(tabs->index(browserRow, 0).data(TabsModel::OpenerTitleRole).toString(), searchTitle);

    // A tab renames itself as it works, and the way back to it has to follow --
    // a label naming a tab by a title it no longer has is a label nobody trusts.
    QSignalSpy changed(tabs, &QAbstractItemModel::dataChanged);
    auto* controller = qobject_cast<LiveSearchController*>(tabs->controllerAt(searchRow));
    QVERIFY(controller);
    controller->setQueryText(QStringLiteral("something else entirely"));
    m_harness->settle(3);

    const QString renamed = tabs->index(searchRow, 0).data(TabsModel::TitleRole).toString();
    QVERIFY2(renamed != searchTitle, "the search tab did not rename itself, so there is nothing to follow");
    QCOMPARE(tabs->index(browserRow, 0).data(TabsModel::OpenerTitleRole).toString(), renamed);
    QVERIFY2(changed.count() > 0, "the way back changed and nothing said so");
}

void TestTabStrip::closingTheSearchTakesTheWayBackWithIt()
{
    int searchRow = -1;
    QVERIFY(search(QStringLiteral("needle"), &searchRow));
    revealFromSearch(searchRow, QStringLiteral("deep/down/needle.txt"));

    TabsModel* tabs = m_harness->app()->tabs();
    const int browserRow = tabs->currentIndex();
    QVERIFY(!tabs->index(browserRow, 0).data(TabsModel::OpenerTitleRole).toString().isEmpty());

    tabs->closeTab(searchRow);
    m_harness->settle(3);

    // The browser has moved down a row, and there is nowhere left to go back to.
    const int moved = browserRow - 1;
    QCOMPARE(tabs->index(moved, 0).data(TabsModel::FeatureIdRole).toString(), QStringLiteral("mole.browser"));
    QCOMPARE(tabs->index(moved, 0).data(TabsModel::OpenerTitleRole).toString(), QString());
    QCOMPARE(tabs->openerRow(moved), -1);

    tabs->setCurrentIndex(moved);
    m_harness->settle(3);
    QQuickItem* bar = backBar();
    QVERIFY2(!bar || !bar->isVisible(), "a way back to a tab that is gone");
}

// A real window, so a real QGuiApplication rather than the guiless one every
// other suite gets from MOLE_TEST_MAIN.
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestTabStrip testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_TabStrip.moc"
