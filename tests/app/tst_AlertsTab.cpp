#include "plugins/builtin/AlertsFeature.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/TabsModel.h"

#include "core/CoreMetaTypes.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The form that creates a watch, with the window behind it.
///
/// `tst_Alerts` covers what a rule measures and what the store keeps; this covers
/// the one step before either, which is the only place a person types a number.
/// `parseThreshold()` fell through to `QLocale::toDouble()`, so "10 GBB", "ten"
/// and an empty field all came back as 0.0 -- `addAlert()` took it, and "total
/// size above 0" fires on the first check of any folder that is not empty. The
/// user asked to be told when a folder passed ten gigabytes and was told
/// immediately, about nothing. See MOLE-378.
class TestAlertsTab : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aThresholdThatCannotBeReadKeepsAddOff();
    void aThresholdThatCannotBeReadIsRefusedWhateverPressesAdd();
    void aThresholdWithItsUnitIsTakenAndAddsTheWatch();
    void aComparisonThatNeedsNoNumberAddsWithTheFieldEmpty();

private:
    AlertsController* openAlerts();
    /// The named item, with the layout given a chance to place it.
    QQuickItem* shown(const QString& objectName) const;

    std::unique_ptr<QmlAppHarness> m_harness;
    AlertsController* m_controller = nullptr;
};

void TestAlertsTab::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));
    m_controller = openAlerts();
    QVERIFY(m_controller);
    m_harness->settle();
}

void TestAlertsTab::cleanup()
{
    m_controller = nullptr;
    m_harness.reset();
}

AlertsController* TestAlertsTab::openAlerts()
{
    const int row = m_harness->app()->openFeatureTab(QStringLiteral("core.alerts"));
    return row < 0 ? nullptr : qobject_cast<AlertsController*>(m_harness->app()->tabs()->controllerAt(row));
}

QQuickItem* TestAlertsTab::shown(const QString& objectName) const
{
    QQuickItem* found = nullptr;
    m_harness->until([this, &objectName, &found] {
        found = m_harness->item(objectName);
        return found != nullptr;
    });
    return found;
}

void TestAlertsTab::aThresholdThatCannotBeReadKeepsAddOff()
{
    QQuickItem* target = shown(QStringLiteral("alertTarget"));
    QQuickItem* threshold = shown(QStringLiteral("alertThreshold"));
    QQuickItem* add = shown(QStringLiteral("addAlertButton"));
    QVERIFY(target && threshold && add);

    target->setProperty("text", m_harness->fixtureUri());

    // The default metric is total size, whose unit is bytes, so this is the
    // shape somebody actually types wrong: a real number and one letter too many.
    for (const QString& unreadable :
        { QStringLiteral("10 GBB"), QStringLiteral("ten"), QStringLiteral("10 gigs"), QString() }) {
        threshold->setProperty("text", unreadable);
        m_harness->settle();
        QVERIFY2(!add->property("enabled").toBool(),
            qPrintable(QStringLiteral("Add was offered for \"%1\"").arg(unreadable)));
    }

    threshold->setProperty("text", QStringLiteral("10 GB"));
    m_harness->settle();
    QVERIFY(add->property("enabled").toBool());
}

void TestAlertsTab::aThresholdThatCannotBeReadIsRefusedWhateverPressesAdd()
{
    // The second line, and the one that matters: a disabled button is a
    // courtesy, and the controller is what a restored form, a plugin or a later
    // edit of this view would reach.
    const QString id = m_controller->addAlert(QStringLiteral("Watch"), m_harness->fixtureUri(),
        QStringLiteral("totalSize"), QStringLiteral("above"), QStringLiteral("10 GBB"),
        QStringLiteral("live"));

    QVERIFY(id.isEmpty());
    QCOMPARE(m_controller->alerts().size(), 0);
}

void TestAlertsTab::aThresholdWithItsUnitIsTakenAndAddsTheWatch()
{
    QQuickItem* target = shown(QStringLiteral("alertTarget"));
    QQuickItem* threshold = shown(QStringLiteral("alertThreshold"));
    QQuickItem* add = shown(QStringLiteral("addAlertButton"));
    QVERIFY(target && threshold && add);

    target->setProperty("text", m_harness->fixtureUri());
    threshold->setProperty("text", QStringLiteral("10 GB"));
    m_harness->settle();
    QVERIFY(m_harness->clickOn(add));

    QVERIFY(m_harness->until([this] { return m_controller->alerts().size() == 1; }));
    const QVariantMap watch = m_controller->alerts().first().toMap();
    // Powers of 1024, which is what a file manager showing GiB everywhere else
    // has to mean by "GB" -- and nowhere near the 0 the old form produced.
    QCOMPARE(watch.value(QStringLiteral("threshold")).toDouble(), 10.0 * 1024 * 1024 * 1024);
}

void TestAlertsTab::aComparisonThatNeedsNoNumberAddsWithTheFieldEmpty()
{
    // "changes" has nothing to compare against, so the field is disabled and an
    // empty one must not be read as a threshold that failed to parse.
    const QString id
        = m_controller->addAlert(QStringLiteral("Watch the permissions"), m_harness->fixtureUri(),
            QStringLiteral("permissions"), QStringLiteral("changed"), QString(), QStringLiteral("live"));

    QVERIFY(!id.isEmpty());
    QVERIFY(m_harness->until([this] { return m_controller->alerts().size() == 1; }));
}

int main(int argc, char** argv)
{
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication app(argc, argv);
    mole::registerCoreMetaTypes();
    TestAlertsTab test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_AlertsTab.moc"
