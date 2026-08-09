#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "ui/AppController.h"
#include "ui/models/DriveListModel.h"

#include "core/CoreMetaTypes.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QTest>

using namespace mole;
using namespace mole::test;

/// The drive rows in the sidebar, driven through the real window.
///
/// Connecting a configured drive used to take F4, File, Drives..., finding it
/// in a short list, pressing a button and closing the dialog -- four of those
/// five steps being navigation, and the one that was not being in the last
/// place anybody would look, because that dialog is where you go to type a
/// hostname.
///
/// The stand-in for a remote is the in-memory backend: it is a real factory
/// behind a real configured drive, connected through the real code path, and no
/// server is involved.
class TestSidebar : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aConfiguredDriveIsListedWithSomethingToPress();
    void pressingConnectMountsItAndTheRowStaysPut();
    void ejectingLeavesTheRowBehind();
    void aLocalDiskOffersNeitherButton();

private:
    /// The sidebar row showing this name, from either list.
    QQuickItem* rowNamed(const QString& label) const;
    QQuickItem* buttonIn(QQuickItem* row, const QString& objectName) const;
    /// Presses a button the way its own handler is bound, without depending on
    /// hover -- there is no pointer in an offscreen window to hover with.
    void press(QQuickItem* button);
    /// Saves an in-memory drive and returns once the sidebar has caught up.
    bool configureScratchDrive(const QString& name);
    DriveListModel::State stateOfDrive(const QString& name) const;

    std::unique_ptr<QmlAppHarness> m_harness;
};

void TestSidebar::init()
{
    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));
}

void TestSidebar::cleanup()
{
    m_harness.reset();
}

QQuickItem* TestSidebar::rowNamed(const QString& label) const
{
    const QList<QQuickItem*> rows = m_harness->items(QStringLiteral("placeRow"));
    for (QQuickItem* row : rows) {
        if (row->property("label").toString() == label)
            return row;
    }
    return nullptr;
}

QQuickItem* TestSidebar::buttonIn(QQuickItem* row, const QString& objectName) const
{
    return row ? row->findChild<QQuickItem*>(objectName) : nullptr;
}

void TestSidebar::press(QQuickItem* button)
{
    QVERIFY(button);
    QMetaObject::invokeMethod(button, "clicked");
    m_harness->settle(3);
}

bool TestSidebar::configureScratchDrive(const QString& name)
{
    if (!m_harness->app()->saveDrive({}, name, QStringLiteral("mem"), {}, {}, {}))
        return false;
    m_harness->settle(3);
    return m_harness->until([this, &name] { return rowNamed(name) != nullptr; });
}

DriveListModel::State TestSidebar::stateOfDrive(const QString& name) const
{
    DriveListModel* model = m_harness->app()->drives();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (index.data(DriveListModel::DisplayNameRole).toString() != name)
            continue;
        return static_cast<DriveListModel::State>(index.data(DriveListModel::StateRole).toInt());
    }
    return DriveListModel::State::Local;
}

void TestSidebar::aConfiguredDriveIsListedWithSomethingToPress()
{
    QVERIFY(configureScratchDrive(QStringLiteral("Scratch")));

    QQuickItem* row = rowNamed(QStringLiteral("Scratch"));
    QVERIFY2(row, "a configured drive has to appear in the sidebar before anyone can press anything");
    QCOMPARE(stateOfDrive(QStringLiteral("Scratch")), DriveListModel::State::Disconnected);

    QQuickItem* action = buttonIn(row, QStringLiteral("placeRemoveButton"));
    QVERIFY(action);
    QVERIFY2(action->isVisible(), "the drive's own button has to be in the row, not in a dialog");

    // The check is offered on the same row, because "is it actually there" is
    // the question somebody has the moment they see a drive listed.
    QQuickItem* check = buttonIn(row, QStringLiteral("placeCheckButton"));
    QVERIFY(check);
    QVERIFY(check->isVisible());

    // Nothing is known about how full it is, and a bar drawn from no
    // measurement would be a claim.
    QCOMPARE(row->property("capacityKnown").toBool(), false);
    QVERIFY(!row->property("severity").toString().isEmpty());
}

void TestSidebar::pressingConnectMountsItAndTheRowStaysPut()
{
    QVERIFY(configureScratchDrive(QStringLiteral("Scratch")));

    QQuickItem* row = rowNamed(QStringLiteral("Scratch"));
    QVERIFY(row);
    // Measured before, so the assertion afterwards is about this row rather
    // than about whatever number happened to be typed into the test.
    const qreal heightBefore = row->height();
    const qreal yBefore = row->y();
    QVERIFY(heightBefore > 0);

    press(buttonIn(row, QStringLiteral("placeRemoveButton")));

    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Connected; }));

    // The same row, not a new one somewhere else: the list is read with a
    // pointer already on the way to it.
    QQuickItem* after = rowNamed(QStringLiteral("Scratch"));
    QVERIFY(after);
    QCOMPARE(after->height(), heightBefore);
    QCOMPARE(after->y(), yBefore);
    QCOMPARE(after->property("connectable").toBool(), false);
    QCOMPARE(after->property("ejectable").toBool(), true);
}

void TestSidebar::ejectingLeavesTheRowBehind()
{
    QVERIFY(configureScratchDrive(QStringLiteral("Scratch")));

    QQuickItem* row = rowNamed(QStringLiteral("Scratch"));
    QVERIFY(row);
    press(buttonIn(row, QStringLiteral("placeRemoveButton")));
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Connected; }));

    const qreal heightBefore = rowNamed(QStringLiteral("Scratch"))->height();
    const qreal yBefore = rowNamed(QStringLiteral("Scratch"))->y();

    press(buttonIn(rowNamed(QStringLiteral("Scratch")), QStringLiteral("placeRemoveButton")));
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Disconnected; }));

    // A drive that has been ejected is still a drive somebody configured.
    // Removing the row is what the mount list used to do, and it is the whole
    // reason the drive list replaced it.
    QQuickItem* after = rowNamed(QStringLiteral("Scratch"));
    QVERIFY2(after, "ejecting must not take the drive out of the sidebar");
    QCOMPARE(after->height(), heightBefore);
    QCOMPARE(after->y(), yBefore);
    QCOMPARE(after->property("connectable").toBool(), true);
}

void TestSidebar::aLocalDiskOffersNeitherButton()
{
    // Home is mounted at startup and nobody configured it: there is nothing to
    // connect, nothing to eject and nothing to ask a check about.
    QQuickItem* row = rowNamed(QStringLiteral("Home"));
    QVERIFY2(row, "the sidebar must still list the local disks it always did");

    QCOMPARE(row->property("actionable").toBool(), false);
    QCOMPARE(row->property("removable").toBool(), false);

    QQuickItem* action = buttonIn(row, QStringLiteral("placeRemoveButton"));
    QVERIFY(action);
    QVERIFY2(!action->isVisible(), "a local disk cannot be connected or ejected");

    QQuickItem* check = buttonIn(row, QStringLiteral("placeCheckButton"));
    QVERIFY(check);
    QVERIFY2(!check->isVisible(), "and there is nothing to ask it that being there does not answer");
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setApplicationName(QStringLiteral("mole-tests"));
    mole::registerCoreMetaTypes();

    TestSidebar testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_Sidebar.moc"
