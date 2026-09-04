#include "host/ActionRegistry.h"
#include "support/QmlAppHarness.h"
#include "support/ScriptedHttpServer.h"
#include "support/TestSupport.h"
#include "ui/AppController.h"
#include "ui/UpdateCheck.h"

#include "core/CoreMetaTypes.h"
#include "core/settings/Preferences.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QTest>

#include <mutex>
#include <vector>

using namespace mole;
using namespace mole::test;

namespace {

/// A version newer than the one this build is, derived rather than typed.
///
/// `"9.9.9"` would work today and would silently stop testing anything the year
/// Mole reaches it. The running version comes from `MOLE_VERSION`, which is
/// `project(VERSION)` -- see MOLE-117 -- so the newer one is that with the major
/// part bumped.
QString oneVersionNewer()
{
    const QStringList parts = QStringLiteral(MOLE_VERSION).split(QLatin1Char('.'));
    return QStringLiteral("%1.0.0").arg(parts.value(0).toInt() + 1);
}

/// The manifest `scripts/release.sh` writes, for a given version. See ADR-0084.
QByteArray manifestFor(const QString& version)
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), UpdateCheck::knownFormat);
    manifest.insert(QStringLiteral("version"), version);
    manifest.insert(QStringLiteral("released"), QStringLiteral("2026-09-08"));
    manifest.insert(QStringLiteral("url"),
        QStringLiteral("https://github.com/Ogefest/mole/releases/tag/v%1").arg(version));
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

/// Anything said at warning level or above while a case ran.
///
/// One of the requirements is that a start where nothing is new leaves nothing
/// behind: no popup, and nothing in the session log above debug. The second half
/// is only assertable by watching the log.
std::mutex g_saidMutex;
std::vector<QString> g_said;
QtMessageHandler g_previousHandler = nullptr;

void recordLoudMessages(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        const std::lock_guard<std::mutex> hold(g_saidMutex);
        g_said.push_back(message);
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

} // namespace

/// The notice a newer version gets, and the switch that stops it being looked for.
///
/// `tst_UpdateCheck` covers what the check decides; this covers what a person
/// sees, with a window behind it. The two halves are one ticket because a notice
/// nobody can switch off is not a notice anybody should ship. See MOLE-325.
///
/// Nothing here reaches the internet: the manifest is served by a scripted local
/// server, and the release page is never opened for real -- the step that hands a
/// URL to the desktop is a hook, and this replaces it with a recorder.
class TestUpdateNotice : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void nothingIsAskedUntilTheWindowIsUp();
    void aNewerVersionShowsOneToastNamingItAndOffersThePage();
    void theToastWithAButtonDoesNotCountDown();
    void thePageOpenedIsTheOneTheManifestNamed();
    void startingAgainTheSameDayShowsNothing();
    void theRunningVersionShowsNothingAndSaysNothingAboveDebug();
    void theHelpEntryIsATickBoxOverTheStoredPreference();
    void theTickSurvivesARestartAndStillReflectsWhatWasStored();
    void switchedOffNothingIsAsked();
    void switchingOffLeavesWhatWasAnnouncedAlone();
    void theDocumentationNamesTheSwitchTheWayTheMenuDoes();

private:
    /// Points the application's own check at `m_server` and starts it.
    void look();
    /// The entry the Help menu draws for the switch, as the menu reads it.
    QVariantMap helpSwitch() const;
    bool toastIsOpen() const;
    QString toastText() const;

    std::unique_ptr<QmlAppHarness> m_harness;
    std::unique_ptr<ScriptedHttpServer> m_server;
    QString m_served;
    /// What would have been opened, instead of a browser appearing.
    QList<QUrl> m_opened;
};

void TestUpdateNotice::init()
{
    m_opened.clear();
    m_served = oneVersionNewer();
    m_server = std::make_unique<ScriptedHttpServer>([this](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(m_served);
        return reply;
    });
    QVERIFY(m_server->start());

    // **Before the application is built, and that is the point.** With the manifest
    // pointed at this server from the environment, a request made while the
    // application was starting up would be seen -- which is what
    // nothingIsAskedUntilTheWindowIsUp asserts. Setting the URL on the check
    // afterwards could not: the window is already up by then, and a check started
    // too early would have gone to the real address and reached the internet.
    qputenv("MOLE_UPDATE_MANIFEST", (m_server->url() + QStringLiteral("/latest.json")).toLocal8Bit());

    m_harness = std::make_unique<QmlAppHarness>();
    QString error;
    QVERIFY2(m_harness->start({}, &error), qPrintable(error));
    m_harness->app()->setLinkHook([this](const QUrl& url) {
        m_opened.append(url);
        return true;
    });

    const std::lock_guard<std::mutex> hold(g_saidMutex);
    g_said.clear();
    if (!g_previousHandler)
        g_previousHandler = qInstallMessageHandler(recordLoudMessages);
}

void TestUpdateNotice::cleanup()
{
    m_harness.reset();
    m_server.reset();
    qunsetenv("MOLE_UPDATE_MANIFEST");
}

void TestUpdateNotice::look()
{
    UpdateCheck* check = m_harness->app()->updateCheck();
    QVERIFY(check);
    // Where it asks is already this test's server -- see init(). Only the patience
    // is set here, so a case that means to fail fails inside the harness's.
    check->setTimeout(2000);
    m_harness->app()->startUpdateCheck();
}

QVariantMap TestUpdateNotice::helpSwitch() const
{
    // buildModel() is what the menu reads when it opens, and it re-evaluates every
    // predicate as it goes -- so asking it is asking the menu. See ActionRegistry.
    for (const QVariant& section : m_harness->app()->actions()->buildModel()) {
        const QVariantMap asMap = section.toMap();
        for (const QVariant& entry : asMap.value(QStringLiteral("actions")).toList()) {
            const QVariantMap action = entry.toMap();
            if (action.value(QStringLiteral("id")).toString() == QStringLiteral("mole.help.updates")) {
                QVariantMap found = action;
                found.insert(QStringLiteral("section"), asMap.value(QStringLiteral("title")));
                return found;
            }
        }
    }
    return {};
}

bool TestUpdateNotice::toastIsOpen() const
{
    QObject* toast = m_harness->object(QStringLiteral("notificationPopup"));
    return toast && toast->property("opened").toBool();
}

QString TestUpdateNotice::toastText() const
{
    // Asked of the toast itself rather than of the label inside it: the toast is
    // a shared component now, so two of them exist and their innards carry the
    // same objectNames. See ui/Toast.qml and MOLE-398.
    QObject* toast = m_harness->object(QStringLiteral("notificationPopup"));
    return toast ? toast->property("text").toString() : QString();
}

void TestUpdateNotice::nothingIsAskedUntilTheWindowIsUp()
{
    // The whole application is built and Main.qml is loaded by now, and nothing has
    // been asked: building an AppController does not start the check. That is what
    // keeps a notice from arriving over a half-drawn window -- main.cpp starts it
    // once there is a window -- and it is also what keeps every other suite that
    // builds an AppController off the network.
    QVERIFY2(m_server->received().isEmpty(), "the application asked before anybody started the check");
    QVERIFY(!toastIsOpen());

    // And the check is there to be started, so this is not passing because nothing
    // exists.
    QVERIFY(m_harness->app()->updateCheck());
}

void TestUpdateNotice::aNewerVersionShowsOneToastNamingItAndOffersThePage()
{
    QVERIFY(!toastIsOpen());
    look();
    QVERIFY(m_harness->until([this] { return toastIsOpen(); }));

    // The version, and nothing else. The release page carries the notes; a notice
    // that carries them is a notice that needs scrolling.
    QVERIFY2(toastText().contains(m_served), qPrintable(toastText()));
    QVERIFY(!toastText().contains(QLatin1String("http")));

    QQuickItem* button = m_harness->item(QStringLiteral("openReleasePageButton"));
    QVERIFY(button);
    QVERIFY(button->property("visible").toBool());

    // One, and only one: the popup is the announcement, so a second start must not
    // produce a second.
    QSignalSpy announced(m_harness->app(), &AppController::versionAvailable);
    m_harness->settle(4);
    QCOMPARE(announced.count(), 0);
}

void TestUpdateNotice::theToastWithAButtonDoesNotCountDown()
{
    look();
    QVERIFY(m_harness->until([this] { return toastIsOpen(); }));

    // Five seconds is right for something to read and wrong for something to
    // press. Asserted on the timer rather than by waiting five seconds to see
    // whether it goes: a test that waits for a clock is a test that fails on a
    // slow machine.
    QObject* toast = m_harness->object(QStringLiteral("notificationPopup"));
    QVERIFY(toast);
    QVERIFY2(!toast->property("counting").toBool(), "the notice is counting down with a button on it");

    // And an ordinary notification still does, so this did not switch it off for
    // everybody.
    emit m_harness->app()->notification(0, QStringLiteral("Something happened"), QString());
    QVERIFY(m_harness->until([this] { return toastText() == QStringLiteral("Something happened"); }));
    QVERIFY(toast->property("counting").toBool());
}

void TestUpdateNotice::thePageOpenedIsTheOneTheManifestNamed()
{
    look();
    QVERIFY(m_harness->until([this] { return toastIsOpen(); }));

    QQuickItem* button = m_harness->item(QStringLiteral("openReleasePageButton"));
    QVERIFY(button);
    QVERIFY(m_harness->clickOn(button));
    QVERIFY(m_harness->until([this] { return !m_opened.isEmpty(); }));

    // Verbatim, and not a URL the application built out of the version: that is
    // the whole reason the manifest carries the field. See ADR-0084.
    QCOMPARE(m_opened.size(), 1);
    QCOMPARE(m_opened.at(0),
        QUrl(QStringLiteral("https://github.com/Ogefest/mole/releases/tag/v%1").arg(m_served)));
    // Pressing it takes the notice away, so there is nothing left to press twice.
    QVERIFY(m_harness->until([this] { return !toastIsOpen(); }));
}

void TestUpdateNotice::startingAgainTheSameDayShowsNothing()
{
    look();
    QVERIFY(m_harness->until([this] { return toastIsOpen(); }));

    // The same profile, built again: what a restart looks like to whatever was
    // written to disk.
    QString error;
    QVERIFY2(m_harness->restart(&error), qPrintable(error));
    m_harness->app()->setLinkHook([this](const QUrl& url) {
        m_opened.append(url);
        return true;
    });
    const int askedBefore = m_server->received().size();

    QSignalSpy announced(m_harness->app(), &AppController::versionAvailable);
    look();
    m_harness->settle(6);

    QCOMPARE(announced.count(), 0);
    QVERIFY(!toastIsOpen());
    // And nothing was even asked: the week of silence saves the request too.
    QCOMPARE(m_server->received().size(), askedBefore);
}

void TestUpdateNotice::theRunningVersionShowsNothingAndSaysNothingAboveDebug()
{
    m_served = QStringLiteral(MOLE_VERSION);

    QSignalSpy announced(m_harness->app(), &AppController::versionAvailable);
    look();
    QVERIFY(m_harness->until([this] { return !m_server->received().isEmpty(); }));
    m_harness->settle(6);

    // The overwhelmingly common case, and it has to be silence with no flicker and
    // no "you are up to date".
    QCOMPARE(announced.count(), 0);
    QVERIFY(!toastIsOpen());

    const std::lock_guard<std::mutex> hold(g_saidMutex);
    for (const QString& said : g_said)
        QFAIL(qPrintable(QStringLiteral("something was said at warning level: %1").arg(said)));
}

void TestUpdateNotice::theHelpEntryIsATickBoxOverTheStoredPreference()
{
    QVariantMap entry = helpSwitch();
    QVERIFY2(!entry.isEmpty(), "the Help menu has no entry for the update check");
    QCOMPARE(entry.value(QStringLiteral("section")).toString(), QStringLiteral("Help"));
    QVERIFY(entry.value(QStringLiteral("checkable")).toBool());
    // Default on, which is the author's decision and the reason README has to say
    // that the request happens at all.
    QVERIFY(entry.value(QStringLiteral("checked")).toBool());

    QVERIFY(m_harness->app()->actions()->trigger(QStringLiteral("mole.help.updates")));
    entry = helpSwitch();
    QVERIFY(!entry.value(QStringLiteral("checked")).toBool());

    // Ticking it again resumes, without anything else being cleared.
    QVERIFY(m_harness->app()->actions()->trigger(QStringLiteral("mole.help.updates")));
    QVERIFY(helpSwitch().value(QStringLiteral("checked")).toBool());
}

void TestUpdateNotice::theTickSurvivesARestartAndStillReflectsWhatWasStored()
{
    QVERIFY(m_harness->app()->actions()->trigger(QStringLiteral("mole.help.updates")));
    QVERIFY(!helpSwitch().value(QStringLiteral("checked")).toBool());

    QString error;
    QVERIFY2(m_harness->restart(&error), qPrintable(error));
    QVERIFY2(!helpSwitch().value(QStringLiteral("checked")).toBool(),
        "the tick came back on, so the choice was not remembered");
}

void TestUpdateNotice::switchedOffNothingIsAsked()
{
    QVERIFY(m_harness->app()->actions()->trigger(QStringLiteral("mole.help.updates")));
    QVERIFY(!helpSwitch().value(QStringLiteral("checked")).toBool());

    QSignalSpy announced(m_harness->app(), &AppController::versionAvailable);
    look();
    m_harness->settle(6);

    // On the absence of the request, not of the popup. Switching the check off has
    // to stop Mole talking to anybody, which is the promise README makes about it.
    QVERIFY2(m_server->received().isEmpty(), "a request went out with the check switched off");
    QCOMPARE(announced.count(), 0);
    QVERIFY(!toastIsOpen());
}

void TestUpdateNotice::switchingOffLeavesWhatWasAnnouncedAlone()
{
    look();
    QVERIFY(m_harness->until([this] { return toastIsOpen(); }));

    QVERIFY(m_harness->app()->actions()->trigger(QStringLiteral("mole.help.updates")));

    // It stops the check and does nothing else. A switch that cleared the state
    // would be a switch people use to force a check, which is not what it is --
    // and it does not take the notice off the screen either.
    Preferences* remembered = m_harness->app()->preferences();
    QVERIFY(remembered);
    QCOMPARE(remembered->value(QStringLiteral("update.announcedVersion")).toString(), m_served);
    QVERIFY(remembered->contains(QStringLiteral("update.announcedOn")));
    QVERIFY(toastIsOpen());
}

void TestUpdateNotice::theDocumentationNamesTheSwitchTheWayTheMenuDoes()
{
    // **"Turn it off in Help" is only useful if the name matches.** Both places that
    // tell somebody how to stop the check name the entry, and the entry's title
    // lives in C++ -- so the two are held against each other rather than against a
    // memory of what it used to be called. See MOLE-326.
    const QString title = helpSwitch().value(QStringLiteral("title")).toString();
    QVERIFY(!title.isEmpty());

    for (const QString& page : { QStringLiteral(MOLE_SOURCE_ROOT "/README.md"),
             QStringLiteral(MOLE_SOURCE_ROOT "/docs/guide/README.md") }) {
        QFile file(page);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(page));
        // Whitespace collapsed, because prose wraps: the first version of this
        // failed on a README that names the entry across a line break.
        const QString said = QString::fromUtf8(file.readAll()).simplified();
        QVERIFY2(said.contains(title),
            qPrintable(QStringLiteral("%1 does not name the menu entry, which is \"%2\"").arg(page, title)));
    }
}

int main(int argc, char** argv)
{
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication app(argc, argv);
    mole::registerCoreMetaTypes();
    TestUpdateNotice test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_UpdateNotice.moc"
