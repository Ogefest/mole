#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "ui/AppController.h"
#include "ui/models/DriveListModel.h"

#include "core/CoreMetaTypes.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QTest>
#include <QVariantMap>

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
    void connectingAsksWhetherTheDriveIsThere();

    void aLockedStoreSaysSoInTheWindow();
    void onePassphraseConnectsTheDrivesThatWereWaiting();
    void aWrongPassphraseSaysSoAndConnectsNothing();
    void withNothingWaitingThereIsNoBand();

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
    /// Saves a drive whose password lives in the credential store, then starts
    /// the application again so the store is shut -- which is the only way to
    /// see what somebody meets on an ordinary morning.
    bool configureLockedDrive(const QString& name);

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

bool TestSidebar::configureLockedDrive(const QString& name)
{
    if (!m_harness->app()->unlockCredentials(QStringLiteral("a passphrase")))
        return false;

    // A field the SFTP backend declares as secret, so the value goes to the
    // store rather than the settings file -- which is what makes the drive
    // depend on the store being open.
    QVariantMap values;
    values.insert(QStringLiteral("host"), QStringLiteral("nas.local"));
    values.insert(QStringLiteral("password"), QStringLiteral("not-a-real-password"));
    if (!m_harness->app()->saveDrive({}, name, QStringLiteral("sftp"), QStringLiteral("sftp"), {}, values))
        return false;
    m_harness->settle(3);

    QString error;
    if (!m_harness->restart(&error))
        return false;
    return m_harness->until([this, &name] { return rowNamed(name) != nullptr; });
}

/// The worst behaviour in this area, and the reason for the band. A drive whose
/// password lives in a shut store waited at startup in complete silence -- no
/// prompt, no badge, no failure. The drive simply was not there, and the way to
/// find out why was to open a dialog and notice an amber panel.
void TestSidebar::aLockedStoreSaysSoInTheWindow()
{
    if (!m_harness->app()->credentialsAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(configureLockedDrive(QStringLiteral("Locked NAS")));

    QVERIFY2(m_harness->app()->credentialsNeeded(),
        "a drive that cannot connect without the store is what the band is for");

    QQuickItem* band = m_harness->item(QStringLiteral("sidebarUnlockBand"));
    QVERIFY2(band, "the property has to have a reader; this test is that reader");
    QVERIFY2(band->isVisible(), "said in the window, before anything is opened, and without a dialog");

    QCOMPARE(stateOfDrive(QStringLiteral("Locked NAS")), DriveListModel::State::Locked);

    // The row points at the band rather than offering a second way to do it.
    QQuickItem* row = rowNamed(QStringLiteral("Locked NAS"));
    QVERIFY(row);
    QCOMPARE(row->property("unlockable").toBool(), true);
    QCOMPARE(row->property("connectable").toBool(), false);

    press(buttonIn(row, QStringLiteral("placeRemoveButton")));
    QQuickItem* field = m_harness->item(QStringLiteral("passphraseField"));
    QVERIFY(field);
    QVERIFY2(field->hasActiveFocus(), "a locked row sends the cursor to where the passphrase is typed");
}

void TestSidebar::onePassphraseConnectsTheDrivesThatWereWaiting()
{
    if (!m_harness->app()->credentialsAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(configureLockedDrive(QStringLiteral("Locked NAS")));
    QCOMPARE(stateOfDrive(QStringLiteral("Locked NAS")), DriveListModel::State::Locked);

    QQuickItem* field = m_harness->item(QStringLiteral("passphraseField"));
    QVERIFY(field);
    field->setProperty("text", QStringLiteral("a passphrase"));
    press(m_harness->item(QStringLiteral("unlockButton")));

    // One entry, and everything that was waiting stops waiting. Where the drive
    // ends up is the connection's business -- it is pointed at a host that does
    // not exist -- but it must have left Locked.
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Locked NAS")) != DriveListModel::State::Locked; }));
    QVERIFY(!m_harness->app()->credentialsNeeded());

    QQuickItem* band = m_harness->item(QStringLiteral("sidebarUnlockBand"));
    QVERIFY(band);
    QVERIFY2(!band->isVisible(), "nothing is waiting any more, so the band has nothing to say");
}

void TestSidebar::aWrongPassphraseSaysSoAndConnectsNothing()
{
    if (!m_harness->app()->credentialsAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(configureLockedDrive(QStringLiteral("Locked NAS")));

    QQuickItem* field = m_harness->item(QStringLiteral("passphraseField"));
    QVERIFY(field);
    field->setProperty("text", QStringLiteral("not the passphrase"));
    press(m_harness->item(QStringLiteral("unlockButton")));

    QQuickItem* error = m_harness->item(QStringLiteral("unlockError"));
    QVERIFY(error);
    QVERIFY2(!error->property("text").toString().isEmpty(), "a refused passphrase has to say so");
    QCOMPARE(stateOfDrive(QStringLiteral("Locked NAS")), DriveListModel::State::Locked);
    QVERIFY(m_harness->app()->credentialsNeeded());
}

void TestSidebar::withNothingWaitingThereIsNoBand()
{
    // The fixture has local disks and no configured drive at all, which is what
    // most people have most of the time.
    QVERIFY(!m_harness->app()->credentialsNeeded());
    QQuickItem* band = m_harness->item(QStringLiteral("sidebarUnlockBand"));
    QVERIFY(band);
    QVERIFY2(!band->isVisible(), "nothing is waiting on a passphrase, so nothing asks for one");
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

/// The wiring, end to end. connectDrive() returns as soon as the backend is
/// built, and building one performs no I/O -- so the row must not say Connected
/// on the strength of that alone. Every state the row passes through is
/// recorded, because "ends Connected" would also be true of a row that never
/// asked anything.
void TestSidebar::connectingAsksWhetherTheDriveIsThere()
{
    QVERIFY(configureScratchDrive(QStringLiteral("Scratch")));

    DriveListModel* model = m_harness->app()->drives();
    QList<DriveListModel::State> seen;
    const auto record = [this, &seen] { seen.append(stateOfDrive(QStringLiteral("Scratch"))); };
    connect(model, &QAbstractItemModel::dataChanged, this, record);
    connect(model, &QAbstractItemModel::modelReset, this, record);

    press(buttonIn(rowNamed(QStringLiteral("Scratch")), QStringLiteral("placeRemoveButton")));

    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Connected; }));

    // The in-memory backend answers, so this one ends connected -- but only
    // after passing through the state that says nobody has asked yet.
    QVERIFY2(seen.contains(DriveListModel::State::Connecting),
        "a drive must be marked as unanswered-for before anything claims it is connected");

    // And the answer is on the row, with the moment it was taken.
    QQuickItem* row = rowNamed(QStringLiteral("Scratch"));
    QVERIFY(row);
    QVERIFY2(
        !row->property("checkCaption").toString().isEmpty(), "the row carries what the check found and when");
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
