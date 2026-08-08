#include "host/FeatureRegistry.h"
#include "sdk/FeatureController.h"
#include "support/FakePlugin.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
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

MOLE_TEST_MAIN(TestTabsModel)
#include "tst_TabsModel.moc"
