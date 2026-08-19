#include "support/MoleTestMain.h"
#include "support/QmlAppHarness.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/models/DriveListModel.h"

#include "core/CoreMetaTypes.h"
#include "core/tasks/TaskManager.h"

#include <QGuiApplication>
#include <QQuickItem>
#include <QSemaphore>
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

    void atStartupNothingAsksForThePassphrase();
    void openingALockedDriveIsWhatAsksForThePassphrase();
    void onePassphraseConnectsTheDrivesThatWereWaiting();
    void aWrongPassphraseSaysSoAndConnectsNothing();
    void theKeyOnARowAndThePaletteOpenTheSameDialog();
    void aDriveWithNoSecretConnectsWithNothingTyped();
    void withNothingWaitingNothingAsks();

    void aDriveNobodyIsUsingShowsAFilledMutedDotAndABookmarkShowsNone();
    void theTwoGreysAreToldApartByShape();
    void openingAFolderOnADriveFillsItsDotInTheAccentColour();
    void aDriveOpenInATabThatIsNotVisibleStillReadsAsOpen();
    void connectingPulsesTheRingAndUnreachableIsFilledRed();
    void workRunningOnADriveMakesItsDotPulseInTheAccentColour();

private:
    /// The passphrase dialog, which is a Popup and so never appears in the
    /// visual tree that item() walks.
    QObject* unlockDialog() const { return m_harness->object(QStringLiteral("unlockDialog")); }
    bool unlockDialogIsUp() const
    {
        QObject* dialog = unlockDialog();
        return dialog && dialog->property("visible").toBool();
    }
    /// Types a passphrase into the dialog and presses its acting button.
    void answerUnlockDialog(const QString& passphrase);

    /// The sidebar row showing this name, from either list.
    QQuickItem* rowNamed(const QString& label) const;
    QQuickItem* buttonIn(QQuickItem* row, const QString& objectName) const;
    /// Presses a button the way its own handler is bound, without depending on
    /// hover -- there is no pointer in an offscreen window to hover with.
    void press(QQuickItem* button);
    /// Saves an in-memory drive and returns once the sidebar has caught up.
    bool configureScratchDrive(const QString& name);
    DriveListModel::State stateOfDrive(const QString& name) const;
    /// The state dot on a row, which is where the six appearances are read from.
    QQuickItem* dotOn(const QString& driveName) const
    {
        return buttonIn(rowNamed(driveName), QStringLiteral("placeStateDot"));
    }
    /// The three channels of one dot, as the view actually has them.
    struct DotLook
    {
        QColor colour;
        bool filled = false;
        bool pulsing = false;
        bool visible = false;
    };
    DotLook lookOf(const QString& driveName) const
    {
        DotLook look;
        QQuickItem* dot = dotOn(driveName);
        if (!dot)
            return look;
        look.visible = dot->isVisible();
        look.filled = dot->property("filled").toBool();
        look.pulsing = dot->property("pulsing").toBool();
        // The colour the dot is actually showing: a ring paints its border and
        // leaves the middle transparent, so reading `color` alone would answer
        // "transparent" for half the states.
        look.colour = dot->property("hue").value<QColor>();
        return look;
    }
    /// Saves a drive whose password lives in the credential store, then starts
    /// the application again so the store is shut -- which is the only way to
    /// see what somebody meets on an ordinary morning.
    bool configureLockedDrive(const QString& name);
    /// Configures the in-memory drive, connects it, and answers its root uri --
    /// or an empty string when either half did not happen.
    QString connectScratch();

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
    return DriveListModel::State::Idle;
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
    // Every field the backend insists on, so building it succeeds and the drive
    // really does connect. Without the user name create() refuses before any I/O,
    // and a test meaning to watch a drive connect watches it fail instead.
    values.insert(QStringLiteral("user"), QStringLiteral("someone"));
    values.insert(QStringLiteral("password"), QStringLiteral("not-a-real-password"));
    if (!m_harness->app()->saveDrive({}, name, QStringLiteral("sftp"), QStringLiteral("sftp"), {}, values))
        return false;
    m_harness->settle(3);

    QString error;
    if (!m_harness->restart(&error))
        return false;
    return m_harness->until([this, &name] { return rowNamed(name) != nullptr; });
}

void TestSidebar::answerUnlockDialog(const QString& passphrase)
{
    QQuickItem* field = m_harness->item(QStringLiteral("passphraseField"));
    QVERIFY(field);
    field->setProperty("text", passphrase);
    // The acting button, not the dialog's accept(): the point of the button is
    // that it acts without closing, so a refused passphrase has somewhere to be
    // reported.
    QQuickItem* act = m_harness->item(QStringLiteral("dialogAccept"));
    QVERIFY(act);
    QVERIFY(act->property("enabled").toBool());
    QMetaObject::invokeMethod(act, "clicked");
    m_harness->settle(4);
}

/// A drive whose password lives in a shut store used to have the whole window ask
/// about it at startup -- an amber band above the drive list and an amber dot on
/// every such row, before anybody had asked for any of them. Nothing had gone
/// wrong: the store is shut at every startup and may stay shut all session. The
/// one moment nobody has a reason to answer was the moment it got asked.
void TestSidebar::atStartupNothingAsksForThePassphrase()
{
    if (!m_harness->app()->credentialsAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(configureLockedDrive(QStringLiteral("Locked NAS")));

    QVERIFY2(!unlockDialogIsUp(), "nothing is asked for at startup: no band, no dialog");

    // The row keeps the news, which is the half of the old behaviour worth
    // keeping. It reads Locked and offers the key rather than the play triangle,
    // so a drive waiting on the store is still told apart from one that is merely
    // disconnected by anybody who looks at the row.
    QCOMPARE(stateOfDrive(QStringLiteral("Locked NAS")), DriveListModel::State::Locked);
    QQuickItem* row = rowNamed(QStringLiteral("Locked NAS"));
    QVERIFY(row);
    QCOMPARE(row->property("unlockable").toBool(), true);
    QCOMPARE(row->property("connectable").toBool(), false);
    QCOMPARE(row->property("stateCaption").toString(), QStringLiteral("Locked"));

    // And it reads grey, not amber. Amber is what a drive on its way to failing
    // wears, and a drive nobody has opened is not a problem.
    QCOMPARE(row->property("severity").toString(), QStringLiteral("idle"));
}

void TestSidebar::openingALockedDriveIsWhatAsksForThePassphrase()
{
    if (!m_harness->app()->credentialsAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(configureLockedDrive(QStringLiteral("Locked NAS")));
    QVERIFY(!unlockDialogIsUp());

    // Clicking the row is "take me there", and taking somebody there means
    // connecting the drive. Before this, the pane was pointed at a drive with no
    // mount behind it and showed a folder with nothing in it.
    QQuickItem* row = rowNamed(QStringLiteral("Locked NAS"));
    QVERIFY(row);
    QMetaObject::invokeMethod(row, "clicked");

    QVERIFY2(m_harness->until([this] { return unlockDialogIsUp(); }),
        "opening a drive that needs the store is what asks for the passphrase");

    QQuickItem* field = m_harness->item(QStringLiteral("passphraseField"));
    QVERIFY(field);
    QVERIFY2(m_harness->until([field] { return field->hasActiveFocus(); }),
        "and the keyboard is in the field, so it can be typed straight away");
}

void TestSidebar::onePassphraseConnectsTheDrivesThatWereWaiting()
{
    if (!m_harness->app()->credentialsAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(configureLockedDrive(QStringLiteral("Locked NAS")));
    QCOMPARE(stateOfDrive(QStringLiteral("Locked NAS")), DriveListModel::State::Locked);

    m_harness->app()->requestCredentials();
    QVERIFY(m_harness->until([this] { return unlockDialogIsUp(); }));
    answerUnlockDialog(QStringLiteral("a passphrase"));

    // One entry, and everything that was waiting stops waiting. Where the drive
    // ends up is the connection's business -- it is pointed at a host that does
    // not exist -- but it must have left Locked.
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Locked NAS")) != DriveListModel::State::Locked; }));
    QVERIFY(!m_harness->app()->credentialsNeeded());
    QVERIFY2(m_harness->until([this] { return !unlockDialogIsUp(); }),
        "the question is answered, so the dialog goes");
}

void TestSidebar::aWrongPassphraseSaysSoAndConnectsNothing()
{
    if (!m_harness->app()->credentialsAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(configureLockedDrive(QStringLiteral("Locked NAS")));

    m_harness->app()->requestCredentials();
    QVERIFY(m_harness->until([this] { return unlockDialogIsUp(); }));
    answerUnlockDialog(QStringLiteral("not the passphrase"));

    QQuickItem* error = m_harness->item(QStringLiteral("unlockError"));
    QVERIFY(error);
    QVERIFY2(!error->property("text").toString().isEmpty(), "a refused passphrase has to say so");
    QVERIFY2(unlockDialogIsUp(), "and it has to say so where it was asked, so the dialog stays up");
    QCOMPARE(stateOfDrive(QStringLiteral("Locked NAS")), DriveListModel::State::Locked);
    QVERIFY(m_harness->app()->credentialsNeeded());
}

void TestSidebar::theKeyOnARowAndThePaletteOpenTheSameDialog()
{
    if (!m_harness->app()->credentialsAvailable())
        QSKIP("this build cannot encrypt");
    QVERIFY(configureLockedDrive(QStringLiteral("Locked NAS")));

    // The key on the row.
    QQuickItem* row = rowNamed(QStringLiteral("Locked NAS"));
    QVERIFY(row);
    press(buttonIn(row, QStringLiteral("placeRemoveButton")));
    QVERIFY2(m_harness->until([this] { return unlockDialogIsUp(); }), "the key asks for the passphrase");
    QObject* fromTheRow = unlockDialog();

    QVERIFY(QMetaObject::invokeMethod(fromTheRow, "close"));
    QVERIFY(m_harness->until([this] { return !unlockDialogIsUp(); }));

    // And the palette's Unlock command, which is what the controller raises.
    m_harness->app()->requestCredentials();
    QVERIFY(m_harness->until([this] { return unlockDialogIsUp(); }));
    QCOMPARE(unlockDialog(), fromTheRow);
}

void TestSidebar::aDriveWithNoSecretConnectsWithNothingTyped()
{
    // The half of the old behaviour that must not change: a drive with no secret
    // fields and mountAtStartup on asks nobody anything.
    QVERIFY(configureScratchDrive(QStringLiteral("Scratch")));
    QString error;
    QVERIFY2(m_harness->restart(&error), qPrintable(error));
    QVERIFY(m_harness->until([this] { return rowNamed(QStringLiteral("Scratch")) != nullptr; }));

    QVERIFY2(!unlockDialogIsUp(), "nothing here needs the store, so nothing asks");
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Idle; }));
}

void TestSidebar::withNothingWaitingNothingAsks()
{
    // The fixture has local disks and no configured drive at all, which is what
    // most people have most of the time.
    QVERIFY(!m_harness->app()->credentialsNeeded());
    QVERIFY2(!unlockDialogIsUp(), "nothing is waiting on a passphrase, so nothing asks for one");
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
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Idle; }));

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
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Idle; }));

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
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Idle; }));

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

// ---- what the dot says ----------------------------------------------------
//
// It used to say whether a drive was plugged in, which is not what anybody wants
// to know -- and on a local disk it said nothing at all, because State::Local fell
// through to the same grey a drive nobody has connected wears. Now it says what a
// drive is *doing*, which is one question for every kind of drive. The appearances
// are read here through the one objectName, because a colour is only a third of
// what the dot is saying. See MOLE-161.

void TestSidebar::aDriveNobodyIsUsingShowsAFilledMutedDotAndABookmarkShowsNone()
{
    // A drive the window is not on. The fixture has more than one local disk, and
    // one of them holds the folder the browser opened on -- which is exactly the
    // distinction being drawn, so the quiet one is found by asking rather than by
    // knowing the fixture's names.
    DriveListModel* drives = m_harness->app()->drives();
    QString quiet;
    QString inUse;
    for (int row = 0; row < drives->rowCount(); ++row) {
        const QModelIndex at = drives->index(row, 0);
        const auto state = static_cast<DriveListModel::State>(at.data(DriveListModel::StateRole).toInt());
        const QString name = at.data(DriveListModel::DisplayNameRole).toString();
        if (state == DriveListModel::State::Idle && quiet.isEmpty())
            quiet = name;
        if (state == DriveListModel::State::Open && inUse.isEmpty())
            inUse = name;
    }
    QVERIFY2(!quiet.isEmpty(), "the fixture has no drive that nobody is using");
    m_harness->settle(3);

    const DotLook idle = lookOf(quiet);
    QVERIFY2(idle.visible, "every drive keeps a dot");
    QVERIFY2(idle.filled, "a drive that is here shows a solid dot");
    QVERIFY2(!idle.pulsing, "and nothing is happening to it");
    QCOMPARE(idle.colour, QColor(QStringLiteral("#8b93a7")));

    // The window opened on a folder, so one of the disks is in use -- and that is
    // the whole point of the dot: two local disks, one of them being read.
    QVERIFY2(!inUse.isEmpty(), "the window is not looking at anything, so this proved nothing");
    QCOMPARE(lookOf(inUse).colour, QColor(QStringLiteral("#4c9aff")));

    // A bookmark row is not a drive, and absence is what says so. Giving an idle
    // drive an empty slot would make the two kinds of row identical -- the fault
    // this scheme exists to fix, inverted.
    QVERIFY(m_harness->app()->bookmarks()->add(m_harness->fixtureUri(), QStringLiteral("Fixture")));
    QVERIFY(m_harness->until([this] { return rowNamed(QStringLiteral("Fixture")) != nullptr; }));
    QQuickItem* bookmarkDot = dotOn(QStringLiteral("Fixture"));
    QVERIFY2(!bookmarkDot || !bookmarkDot->isVisible(), "a bookmark row must have no dot at all");
}

void TestSidebar::theTwoGreysAreToldApartByShape()
{
    // Configured and not connected: the grey that means *could be connected*.
    QVERIFY(configureScratchDrive(QStringLiteral("Scratch")));
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Disconnected; }));
    m_harness->settle(3);

    const DotLook notConnected = lookOf(QStringLiteral("Scratch"));
    QVERIFY(notConnected.visible);
    QVERIFY2(!notConnected.filled, "a drive that could be connected and is not shows a hollow ring");
    QVERIFY(!notConnected.pulsing);

    // Connected and unused: the same grey, a different shape. That is the pair a
    // shade of grey could not express at eight pixels, and the reason the answer
    // is a ring rather than a second colour.
    press(buttonIn(rowNamed(QStringLiteral("Scratch")), QStringLiteral("placeRemoveButton")));
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Idle; }));
    m_harness->settle(3);

    const DotLook idle = lookOf(QStringLiteral("Scratch"));
    QVERIFY2(idle.filled, "a connected drive nobody is using is here, and a solid dot says so");
    QVERIFY(!idle.pulsing);
    QCOMPARE(idle.colour, notConnected.colour);
    QCOMPARE(idle.colour, QColor(QStringLiteral("#8b93a7")));
}

QString TestSidebar::connectScratch()
{
    if (!configureScratchDrive(QStringLiteral("Scratch")))
        return {};
    press(buttonIn(rowNamed(QStringLiteral("Scratch")), QStringLiteral("placeRemoveButton")));
    if (!m_harness->until(
            [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Idle; }))
        return {};

    DriveListModel* drives = m_harness->app()->drives();
    for (int row = 0; row < drives->rowCount(); ++row) {
        if (drives->index(row, 0).data(DriveListModel::DisplayNameRole).toString()
            == QStringLiteral("Scratch")) {
            return drives->rootUriAt(row);
        }
    }
    return {};
}

void TestSidebar::openingAFolderOnADriveFillsItsDotInTheAccentColour()
{
    const QString root = connectScratch();
    QVERIFY2(!root.isEmpty(), "the scratch drive would not connect");

    QVERIFY(m_harness->app()->goTo(root));
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Open; }));
    m_harness->settle(3);

    const DotLook open = lookOf(QStringLiteral("Scratch"));
    QVERIFY(open.filled);
    QVERIFY2(!open.pulsing, "open is a state, not something happening");
    QCOMPARE(open.colour, QColor(QStringLiteral("#4c9aff")));

    // And leaving turns it off. Nothing polled to find that out: a pane navigated
    // and said so.
    QVERIFY(m_harness->app()->goTo(m_harness->fixtureUri()));
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Idle; }));
    m_harness->settle(3);
    QCOMPARE(lookOf(QStringLiteral("Scratch")).colour, QColor(QStringLiteral("#8b93a7")));
}

void TestSidebar::aDriveOpenInATabThatIsNotVisibleStillReadsAsOpen()
{
    const QString root = connectScratch();
    QVERIFY2(!root.isEmpty(), "the scratch drive would not connect");

    QVERIFY(m_harness->app()->goTo(root));
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Open; }));
    const int wasOnIt = m_harness->app()->tabs()->currentIndex();

    // Another tab, on top of it. The question is about the drive and not about the
    // window, so the drive is still open although nobody can see it.
    const int other = m_harness->app()->tabs()->openTab(QStringLiteral("mole.browser"));
    QVERIFY(other >= 0);
    m_harness->app()->tabs()->setCurrentIndex(other);
    m_harness->settle(3);
    QVERIFY(m_harness->app()->tabs()->currentIndex() != wasOnIt);

    QCOMPARE(stateOfDrive(QStringLiteral("Scratch")), DriveListModel::State::Open);
    QVERIFY(lookOf(QStringLiteral("Scratch")).filled);
    QCOMPARE(lookOf(QStringLiteral("Scratch")).colour, QColor(QStringLiteral("#4c9aff")));

    // Closing the tab that was on it is what turns the dot off.
    m_harness->app()->tabs()->closeTab(wasOnIt);
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) != DriveListModel::State::Open; }));
}

void TestSidebar::connectingPulsesTheRingAndUnreachableIsFilledRed()
{
    QVERIFY2(!connectScratch().isEmpty(), "the scratch drive would not connect");

    DriveListModel* drives = m_harness->app()->drives();
    QString driveId;
    for (int row = 0; row < drives->rowCount(); ++row) {
        const QModelIndex at = drives->index(row, 0);
        if (at.data(DriveListModel::DisplayNameRole).toString() == QStringLiteral("Scratch"))
            driveId = at.data(DriveListModel::ConfiguredIdRole).toString();
    }
    QVERIFY(!driveId.isEmpty());

    // A check is out and has not answered: something is happening, and it is not
    // yet known whether the far end is there. A hollow ring, pulsing.
    drives->noteCheckStarted(driveId);
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Connecting; }));
    m_harness->settle(3);
    const DotLook connecting = lookOf(QStringLiteral("Scratch"));
    QVERIFY2(!connecting.filled, "connecting has not arrived anywhere yet");
    QVERIFY2(connecting.pulsing, "and something is happening, which is what motion means");
    QCOMPARE(connecting.colour, QColor(QStringLiteral("#8b93a7")));

    // And the answer was no.
    drives->noteCheckResult(driveId, false, QStringLiteral("No route to the server"));
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Unreachable; }));
    m_harness->settle(3);
    const DotLook unreachable = lookOf(QStringLiteral("Scratch"));
    QVERIFY2(unreachable.filled, "unreachable is a state it has arrived at, not one it is heading for");
    QVERIFY(!unreachable.pulsing);
    QCOMPARE(unreachable.colour, QColor(QStringLiteral("#e5534b")));
}

void TestSidebar::workRunningOnADriveMakesItsDotPulseInTheAccentColour()
{
    // The sixth appearance, and the state that makes the whole scheme worth
    // having: "which of my drives is this transfer actually touching" is a
    // question people ask out loud, and the task strip answers it only by naming a
    // task whose title contains a path somebody has to read. See MOLE-162.
    const QString root = connectScratch();
    QVERIFY2(!root.isEmpty(), "the scratch drive would not connect");
    QCOMPARE(stateOfDrive(QStringLiteral("Scratch")), DriveListModel::State::Idle);

    // Held still on the pool, because busy is a state a task is *in*.
    auto gate = std::make_shared<QSemaphore>();
    auto* work
        = new ScriptedTask(QStringLiteral("Copy to Scratch"), [gate](ScriptedTask&) { gate->acquire(); });
    work->noteTouching(VfsUri::fromString(root).child(QStringLiteral("holiday.mov")));
    m_harness->app()->services().tasks->submit(work);

    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Busy; }));
    m_harness->settle(3);

    const DotLook busy = lookOf(QStringLiteral("Scratch"));
    QVERIFY2(busy.filled, "the drive is here and being worked on");
    QVERIFY2(busy.pulsing, "and something is happening to it, which is what motion means");
    QCOMPARE(busy.colour, QColor(QStringLiteral("#4c9aff")));

    // And it stops the moment the work does.
    gate->release();
    QVERIFY(m_harness->until(
        [this] { return stateOfDrive(QStringLiteral("Scratch")) == DriveListModel::State::Idle; }));
    m_harness->settle(3);
    const DotLook after = lookOf(QStringLiteral("Scratch"));
    QVERIFY(after.filled);
    QVERIFY2(!after.pulsing, "a finished task must not leave a drive pulsing");
    QCOMPARE(after.colour, QColor(QStringLiteral("#8b93a7")));
    gate->release(8);
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
