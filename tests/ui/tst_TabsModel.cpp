#include "host/FeatureRegistry.h"
#include "sdk/FeatureController.h"
#include "support/FakePlugin.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/SessionStore.h"
#include "ui/models/TabsModel.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>

using namespace mole;
using namespace mole::test;

/// A controller that renames its own tab, the way the browser does when the
/// user navigates.
class RenamingController : public FeatureController
{
    Q_OBJECT
public:
    explicit RenamingController(QObject* parent)
        : FeatureController(QStringLiteral("initial"), parent)
    {
    }
    void rename(const QString& title) { setTitle(title); }
    void work(bool busy) { setBusy(busy); }
};

class RenamingFeature final : public IFeature
{
public:
    QString id() const override { return QStringLiteral("test.renaming"); }
    QString title() const override { return QStringLiteral("Renaming"); }
    QString description() const override { return {}; }
    QString iconText() const override { return QStringLiteral("R"); }
    QUrl viewSource() const override { return QUrl(QStringLiteral("qrc:/x.qml")); }
    FeatureController* createController(QObject* parent) override { return new RenamingController(parent); }
};

class TestTabsModel : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void obeysTheModelContract();
    void openTabCreatesIndependentControllers();
    void unknownFeatureIdOpensNothing();
    void openTabBecomesCurrent();
    void closingTabDestroysItsController();
    void closingBeforeCurrentShiftsSelection();
    void closingReturnsToTheTabItWasOpenedFrom();
    void closingReturnsToNeighbourWhenTheOpenerIsGone();
    void aWorkingTabSaysSo();
    void closingTheLastTabLeavesNoSelection();
    void closingOutOfRangeIsIgnored();
    void controllerRenameUpdatesTheTabLabel();
    void exposesViewSourceForTheShell();
    void rowOfFeatureFindsTheFirstTabOfAKind();
    void aRestoredTabWasNotOpenedFromItsNeighbour();
    void theRestoredSelectionSurvivesAFeatureThatIsGone();

private:
    FeatureRegistry* m_registry = nullptr;
    FakeFeature* m_fake = nullptr;
};

void TestTabsModel::init()
{
    m_registry = new FeatureRegistry(this);
    auto fake = std::make_unique<FakeFeature>(QStringLiteral("test.fake"), QStringLiteral("Fake"));
    m_fake = fake.get();
    QVERIFY(m_registry->registerFeature(std::move(fake)));
    QVERIFY(m_registry->registerFeature(std::make_unique<RenamingFeature>()));
}

void TestTabsModel::obeysTheModelContract()
{
    TabsModel tabs(m_registry);
    QAbstractItemModelTester tester(&tabs, QAbstractItemModelTester::FailureReportingMode::Warning);

    tabs.openTab(QStringLiteral("test.fake"));
    tabs.openTab(QStringLiteral("test.fake"));
    tabs.closeTab(0);
    QCOMPARE(tabs.rowCount(), 1);
}

void TestTabsModel::openTabCreatesIndependentControllers()
{
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.fake"));
    tabs.openTab(QStringLiteral("test.fake"));

    QCOMPARE(tabs.rowCount(), 2);
    QCOMPARE(m_fake->createdCount(), 2);
    // Two tabs of the same kind must not share state, or a second browser tab
    // would follow the first one around.
    QVERIFY(tabs.controllerAt(0) != tabs.controllerAt(1));
}

void TestTabsModel::unknownFeatureIdOpensNothing()
{
    TabsModel tabs(m_registry);
    QCOMPARE(tabs.openTab(QStringLiteral("nope")), -1);
    QCOMPARE(tabs.rowCount(), 0);
    QCOMPARE(tabs.currentIndex(), -1);
}

void TestTabsModel::openTabBecomesCurrent()
{
    TabsModel tabs(m_registry);
    QSignalSpy opened(&tabs, &TabsModel::tabOpened);

    QCOMPARE(tabs.openTab(QStringLiteral("test.fake")), 0);
    QCOMPARE(tabs.currentIndex(), 0);
    QCOMPARE(tabs.openTab(QStringLiteral("test.fake")), 1);
    QCOMPARE(tabs.currentIndex(), 1);
    QCOMPARE(opened.count(), 2);
    QCOMPARE(tabs.currentController(), tabs.controllerAt(1));
}

void TestTabsModel::closingTabDestroysItsController()
{
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.fake"));
    QPointer<QObject> controller = tabs.controllerAt(0);
    QVERIFY(controller);

    tabs.closeTab(0);
    drainEvents();

    QVERIFY2(controller.isNull(), "a closed tab must not leak its controller");
    QCOMPARE(tabs.rowCount(), 0);
}

void TestTabsModel::closingBeforeCurrentShiftsSelection()
{
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.fake"));
    tabs.openTab(QStringLiteral("test.fake"));
    tabs.openTab(QStringLiteral("test.fake"));
    tabs.setCurrentIndex(2);

    QObject* stillWanted = tabs.controllerAt(2);
    tabs.closeTab(0);

    // The user was looking at the third tab; it must stay selected.
    QCOMPARE(tabs.rowCount(), 2);
    QCOMPARE(tabs.currentIndex(), 1);
    QCOMPARE(tabs.currentController(), stillWanted);
}

void TestTabsModel::closingReturnsToTheTabItWasOpenedFrom()
{
    // Two tabs already open; the user is on the first and opens a third from
    // it. It lands at the end, so position alone would send them to the second
    // -- a tab they were never working in.
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.fake"));
    tabs.openTab(QStringLiteral("test.fake"));
    tabs.setCurrentIndex(0);
    QObject* opener = tabs.controllerAt(0);

    const int third = tabs.openTab(QStringLiteral("test.fake"));
    QCOMPARE(third, 2);

    QSignalSpy currentSpy(&tabs, &TabsModel::currentIndexChanged);
    tabs.closeTab(third);

    QCOMPARE(tabs.currentIndex(), 0);
    QCOMPARE(tabs.currentController(), opener);
    QCOMPARE(currentSpy.count(), 1);
}

void TestTabsModel::closingReturnsToNeighbourWhenTheOpenerIsGone()
{
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.fake")); // the opener
    tabs.setCurrentIndex(0);
    tabs.openTab(QStringLiteral("test.fake")); // opened from it

    tabs.closeTab(0); // the opener is closed first
    QCOMPARE(tabs.rowCount(), 1);

    // Nothing to return to, so the ordinary neighbour rule applies rather than
    // a dangling reference to a tab that no longer exists.
    tabs.closeTab(0);
    QCOMPARE(tabs.rowCount(), 0);
    QCOMPARE(tabs.currentIndex(), -1);
}

void TestTabsModel::aWorkingTabSaysSo()
{
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.renaming"));

    const QModelIndex index = tabs.index(0, 0);
    QCOMPARE(index.data(TabsModel::BusyRole).toBool(), false);

    QSignalSpy changed(&tabs, &TabsModel::dataChanged);
    auto* controller = qobject_cast<RenamingController*>(tabs.controllerAt(0));
    QVERIFY(controller);
    controller->work(true);

    // A long report must be visible from the strip, not only from inside the
    // tab the user has navigated away from.
    QCOMPARE(index.data(TabsModel::BusyRole).toBool(), true);
    QCOMPARE(changed.count(), 1);
    const QList<int> roles = changed.first().at(2).value<QList<int>>();
    QVERIFY2(roles.contains(TabsModel::BusyRole), "the strip must be told what changed");
}

void TestTabsModel::closingTheLastTabLeavesNoSelection()
{
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.fake"));
    tabs.closeTab(0);

    QCOMPARE(tabs.currentIndex(), -1);
    QVERIFY(tabs.currentController() == nullptr);
}

void TestTabsModel::closingOutOfRangeIsIgnored()
{
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.fake"));

    tabs.closeTab(-1);
    tabs.closeTab(42);
    QCOMPARE(tabs.rowCount(), 1);
}

void TestTabsModel::controllerRenameUpdatesTheTabLabel()
{
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.renaming"));

    QSignalSpy changed(&tabs, &TabsModel::dataChanged);
    QCOMPARE(tabs.index(0, 0).data(TabsModel::TitleRole).toString(), QStringLiteral("initial"));

    auto* controller = qobject_cast<RenamingController*>(tabs.controllerAt(0));
    QVERIFY(controller);
    controller->rename(QStringLiteral("Documents"));

    // The shell must follow the controller without knowing what it does.
    QCOMPARE(tabs.index(0, 0).data(TabsModel::TitleRole).toString(), QStringLiteral("Documents"));
    QCOMPARE(changed.count(), 1);
}

void TestTabsModel::exposesViewSourceForTheShell()
{
    TabsModel tabs(m_registry);
    tabs.openTab(QStringLiteral("test.fake"));

    const QModelIndex index = tabs.index(0, 0);
    QCOMPARE(index.data(TabsModel::ViewSourceRole).toUrl(), QUrl(QStringLiteral("qrc:/fake/View.qml")));
    QCOMPARE(index.data(TabsModel::FeatureIdRole).toString(), QStringLiteral("test.fake"));
    QVERIFY(index.data(TabsModel::ControllerRole).value<QObject*>() != nullptr);
}

/// What the callers who may only ever have one tab of something ask. Named
/// rather than written out where it was needed: the preview had its own copy of
/// this loop and Add to set was about to want a second. See MOLE-206.
void TestTabsModel::rowOfFeatureFindsTheFirstTabOfAKind()
{
    TabsModel tabs(m_registry);
    QCOMPARE(tabs.rowOfFeature(QStringLiteral("test.fake")), -1);
    QCOMPARE(tabs.rowOfFeature(QStringLiteral("nope")), -1);

    tabs.openTab(QStringLiteral("test.renaming"));
    tabs.openTab(QStringLiteral("test.fake"));
    tabs.openTab(QStringLiteral("test.fake"));

    QCOMPARE(tabs.rowOfFeature(QStringLiteral("test.renaming")), 0);
    // The first, not the last: whoever asks is about to show it, and the one
    // that has been open longest is the one anything else already points at.
    QCOMPARE(tabs.rowOfFeature(QStringLiteral("test.fake")), 1);

    tabs.closeTab(1);
    QCOMPARE(tabs.rowOfFeature(QStringLiteral("test.fake")), 1);
    tabs.closeTab(1);
    QCOMPARE(tabs.rowOfFeature(QStringLiteral("test.fake")), -1);
}

/// A lineage that never existed.
///
/// openTab() records the current tab as the opener and then selects the row it
/// made, and restoreSession() called it once per saved tab in file order -- so
/// tab N came back recorded as opened from tab N-1. Everything built on that
/// relation then answered wrongly: rowOpenedFromCurrent() said "the tab next to
/// this one", so AppController::browserTabForCurrent() navigated a browser the
/// user had left somewhere instead of opening one, and closing a tab handed them
/// back to a tab they had never come from. ARCHITECTURE.md's "Tabs" section
/// describes the mechanism as leaving one browser behind and never touching the
/// search tab, which is what this restores. See MOLE-393.
void TestTabsModel::aRestoredTabWasNotOpenedFromItsNeighbour()
{
    TabsModel tabs(m_registry);
    Session session;
    session.tabs.append(TabSession { QStringLiteral("test.fake"), {} });
    session.tabs.append(TabSession { QStringLiteral("test.renaming"), {} });
    session.tabs.append(TabSession { QStringLiteral("test.fake"), {} });
    session.currentIndex = 1;

    QCOMPARE(tabs.restoreSession(session), 3);
    QCOMPARE(tabs.rowCount(), 3);

    // From every one of them, nothing was opened from here.
    for (int row = 0; row < tabs.rowCount(); ++row) {
        tabs.setCurrentIndex(row);
        QCOMPARE(tabs.rowOpenedFromCurrent(QStringLiteral("test.fake")), -1);
        QCOMPARE(tabs.rowOpenedFromCurrent(QStringLiteral("test.renaming")), -1);
    }

    // And a tab opened by hand afterwards does have a lineage, because that is
    // the relation this is about.
    tabs.setCurrentIndex(0);
    const int opened = tabs.openTab(QStringLiteral("test.renaming"));
    QVERIFY(opened > 0);
    tabs.setCurrentIndex(0);
    QCOMPARE(tabs.rowOpenedFromCurrent(QStringLiteral("test.renaming")), opened);
}

/// The saved index counts tabs that did not come back.
///
/// It indexes the *saved* list, and a tab whose feature nobody provides is
/// skipped -- so [gone, A, B] with the index on B selected position 2, which was
/// clamped to the last row and handed back A. See MOLE-393.
void TestTabsModel::theRestoredSelectionSurvivesAFeatureThatIsGone()
{
    TabsModel tabs(m_registry);
    Session session;
    session.tabs.append(TabSession { QStringLiteral("test.uninstalled"), {} });
    session.tabs.append(TabSession { QStringLiteral("test.fake"), {} });
    session.tabs.append(TabSession { QStringLiteral("test.renaming"), {} });
    session.currentIndex = 2;

    QCOMPARE(tabs.restoreSession(session), 2);
    QCOMPARE(tabs.rowCount(), 2);
    QCOMPARE(tabs.index(tabs.currentIndex(), 0).data(TabsModel::FeatureIdRole).toString(),
        QStringLiteral("test.renaming"));

    // A saved index naming the tab that is gone lands on the nearest one before
    // it rather than on whatever the clamp produced.
    Session onTheMissingOne;
    onTheMissingOne.tabs.append(TabSession { QStringLiteral("test.fake"), {} });
    onTheMissingOne.tabs.append(TabSession { QStringLiteral("test.uninstalled"), {} });
    onTheMissingOne.tabs.append(TabSession { QStringLiteral("test.renaming"), {} });
    onTheMissingOne.currentIndex = 1;

    TabsModel second(m_registry);
    QCOMPARE(second.restoreSession(onTheMissingOne), 2);
    QCOMPARE(second.index(second.currentIndex(), 0).data(TabsModel::FeatureIdRole).toString(),
        QStringLiteral("test.fake"));
}

MOLE_TEST_MAIN(TestTabsModel)
#include "tst_TabsModel.moc"
