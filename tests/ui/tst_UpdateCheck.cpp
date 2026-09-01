#include "support/MoleTestMain.h"
#include "support/ScriptedHttpServer.h"
#include "support/TestSupport.h"
#include "ui/UpdateCheck.h"

#include "core/settings/Preferences.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>

#include <mutex>
#include <vector>

using namespace mole;
using namespace mole::test;

namespace {

/// Every message this suite provoked at warning level or above, whoever emitted
/// it.
///
/// **This is an assertion and not a convenience.** "Silence on failure" is the
/// substance of the check: an offline machine, a captive portal or a timeout must
/// leave nothing in the session log, and the first version of this code broke that
/// without any case noticing -- it read the reply's body before asking whether the
/// reply had arrived, and Qt answered a read on a closed device with a warning of
/// its own. So every case ends by asserting nobody said anything, and that
/// includes Qt.
std::mutex g_saidMutex;
std::vector<QString> g_said;
QtMessageHandler g_previousHandler = nullptr;

void recordLoudMessages(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        const std::lock_guard<std::mutex> hold(g_saidMutex);
        g_said.push_back(message);
    }
    // Chained rather than swallowed, so the run still reads the way every other
    // suite's does.
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

/// A manifest in the shape `scripts/release.sh` writes, so a case can cut about
/// whichever field it is asking about. See ADR-0084.
QByteArray manifestFor(const QString& version, int format = 1,
    const QString& page = QStringLiteral("https://github.com/Ogefest/mole/releases/tag/v%1"))
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), format);
    manifest.insert(QStringLiteral("version"), version);
    manifest.insert(QStringLiteral("released"), QStringLiteral("2026-09-08"));
    manifest.insert(QStringLiteral("url"), page.contains(QLatin1String("%1")) ? page.arg(version) : page);
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

} // namespace

/// Whether Mole notices it is out of date, and everything it must not do while
/// finding out.
///
/// Nothing here reaches the internet. The manifest is served by a local scripted
/// server, which is also the only way to obtain the answers that matter most --
/// a 500, a body that stops half way, a connection that goes quiet and stays
/// open. Every one of those has to end in silence, and silence is the hardest
/// thing to test for by hand: it looks exactly like the check working.
///
/// The week of silence is checked by moving the calendar rather than by waiting,
/// and every case waits on a condition rather than on a clock. See MOLE-324.
class TestUpdateCheck : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void versionsAreComparedOnTheirNumbersAndNotAsText();
    void aNewerVersionIsAnnouncedWithTheManifestsOwnPage();
    void theSameVersionIsAnnouncedOnceAndNotAgainTheSameDay();
    void theRunningVersionIsNotNews();
    void anOlderVersionIsNotNews();
    void theSecondLookIsConditionalAndA304SaysNothing();
    void aWeekLaterItAsksAgainAndAnEvenNewerVersionIsAnnounced();
    void aVersionAlreadyAnnouncedIsNotAnnouncedAgainWhenTheWeekIsOut();
    void updatingToTheAnnouncedVersionEndsTheSilence();
    void theSwitchStopsTheRequestBeingMadeAtAll();
    void anUnreachableServerSaysNothing();
    void aServerErrorSaysNothing();
    void aBodyThatStopsHalfWaySaysNothing();
    void aConnectionThatGoesQuietSaysNothingAndDoesNotWait();
    void aManifestThatIsNotADocumentSaysNothing();
    void aFormatThisBuildDoesNotKnowSaysNothing();
    void aPageThatIsNotHttpsIsRefused();
    void nothingIdentifyingLeavesTheMachine();
    void theManifestCanBePointedSomewhereElseFromTheEnvironment();
    void readmeStatesWhatLeavesTheMachine();

private:
    /// A check pointed at `server`, with the preferences this test owns.
    std::unique_ptr<UpdateCheck> checkAgainst(
        const ScriptedHttpServer& server, const QString& running = QStringLiteral("0.1.0"));

    std::unique_ptr<QTemporaryDir> m_home;
    std::unique_ptr<Preferences> m_preferences;
    /// What the check believes today is. Moved, never waited for.
    QDate m_today { 2026, 9, 8 };
};

void TestUpdateCheck::init()
{
    m_home = std::make_unique<QTemporaryDir>();
    QVERIFY(m_home->isValid());
    m_preferences = std::make_unique<Preferences>(m_home->filePath(QStringLiteral("preferences.json")));
    m_today = QDate(2026, 9, 8);

    const std::lock_guard<std::mutex> hold(g_saidMutex);
    g_said.clear();
    if (!g_previousHandler)
        g_previousHandler = qInstallMessageHandler(recordLoudMessages);
}

void TestUpdateCheck::cleanup()
{
    m_preferences.reset();
    m_home.reset();

    // Every case, not only the ones about failing. See recordLoudMessages().
    const std::lock_guard<std::mutex> hold(g_saidMutex);
    for (const QString& said : g_said)
        QFAIL(qPrintable(QStringLiteral("something was said at warning level: %1").arg(said)));
}

std::unique_ptr<UpdateCheck> TestUpdateCheck::checkAgainst(
    const ScriptedHttpServer& server, const QString& running)
{
    auto check = std::make_unique<UpdateCheck>(m_preferences.get(), running);
    check->setManifestUrl(QUrl(server.url() + QStringLiteral("/latest.json")));
    // Short, because every case that means to fail should fail inside the test's
    // own patience rather than the harness's.
    check->setTimeout(2000);
    check->setCalendar([this] { return m_today; });
    return check;
}

void TestUpdateCheck::versionsAreComparedOnTheirNumbersAndNotAsText()
{
    // The one everybody gets wrong: as text, "0.9.0" sorts after "0.10.0".
    QVERIFY(UpdateCheck::isNewer(QStringLiteral("0.10.0"), QStringLiteral("0.9.0")));
    QVERIFY(!UpdateCheck::isNewer(QStringLiteral("0.9.0"), QStringLiteral("0.10.0")));
    QVERIFY(UpdateCheck::isNewer(QStringLiteral("1.0.0"), QStringLiteral("0.99.99")));
    QVERIFY(UpdateCheck::isNewer(QStringLiteral("0.1.1"), QStringLiteral("0.1.0")));
    QVERIFY(!UpdateCheck::isNewer(QStringLiteral("0.1.0"), QStringLiteral("0.1.0")));

    // Anything not shaped like three numbers is never newer than anything. A
    // manifest edited by hand is the only way to get one, and comparing it
    // wrongly is worse than not answering.
    QVERIFY(!UpdateCheck::isNewer(QStringLiteral("0.2"), QStringLiteral("0.1.0")));
    QVERIFY(!UpdateCheck::isNewer(QStringLiteral("0.2.0-rc1"), QStringLiteral("0.1.0")));
    QVERIFY(!UpdateCheck::isNewer(QStringLiteral("tomorrow"), QStringLiteral("0.1.0")));
    QVERIFY(!UpdateCheck::isNewer(QStringLiteral("0.2.0"), QString()));
}

void TestUpdateCheck::aNewerVersionIsAnnouncedWithTheManifestsOwnPage()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.headers << "ETag: \"first\"" << "Content-Type: application/json";
        reply.body = manifestFor(QStringLiteral("0.2.0"));
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));

    QCOMPARE(found.count(), 1);
    QCOMPARE(found.at(0).at(0).toString(), QStringLiteral("0.2.0"));
    // The manifest's own field, verbatim. The application never assembles this --
    // that is the whole reason the field exists.
    QCOMPARE(found.at(0).at(1).toUrl(),
        QUrl(QStringLiteral("https://github.com/Ogefest/mole/releases/tag/v0.2.0")));

    // What it remembered, which is the version and the day rather than "we looked".
    QCOMPARE(
        m_preferences->value(QStringLiteral("update.announcedVersion")).toString(), QStringLiteral("0.2.0"));
    QCOMPARE(
        m_preferences->value(QStringLiteral("update.announcedOn")).toString(), QStringLiteral("2026-09-08"));
    QCOMPARE(m_preferences->value(QStringLiteral("update.etag")).toString(), QStringLiteral("\"first\""));
}

void TestUpdateCheck::theSameVersionIsAnnouncedOnceAndNotAgainTheSameDay()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(QStringLiteral("0.2.0"));
        return reply;
    });
    QVERIFY(server.start());

    auto first = checkAgainst(server);
    QSignalSpy done(first.get(), &UpdateCheck::finished);
    QVERIFY(first->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(server.received().size(), 1);

    // Starting Mole again the same day. Not a second notice, and not a second
    // request either: the week of silence saves both.
    auto again = checkAgainst(server);
    QSignalSpy foundAgain(again.get(), &UpdateCheck::newVersionFound);
    QVERIFY(!again->start());
    drainEvents();
    QCOMPARE(foundAgain.count(), 0);
    QCOMPARE(server.received().size(), 1);
}

void TestUpdateCheck::theRunningVersionIsNotNews()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.headers << "ETag: \"same\"";
        reply.body = manifestFor(QStringLiteral("0.1.0"));
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));

    QCOMPARE(found.count(), 0);
    // Nothing announced, and nothing to be quiet about tomorrow -- but the ETag is
    // kept, so tomorrow's look costs a 304 and no body.
    QVERIFY(!m_preferences->contains(QStringLiteral("update.announcedVersion")));
    QCOMPARE(m_preferences->value(QStringLiteral("update.etag")).toString(), QStringLiteral("\"same\""));
}

void TestUpdateCheck::anOlderVersionIsNotNews()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(QStringLiteral("0.0.9"));
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(found.count(), 0);
}

void TestUpdateCheck::theSecondLookIsConditionalAndA304SaysNothing()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request& asked) {
        ScriptedHttpServer::Reply reply;
        // What raw.githubusercontent.com does, and the reason the ETag is kept at
        // all: once anything has been read, the ordinary answer is no body.
        if (asked.header("If-None-Match") == "\"same\"") {
            reply.status = 304;
            reply.reason = "Not Modified";
            reply.headers << "ETag: \"same\"";
            return reply;
        }
        reply.headers << "ETag: \"same\"";
        reply.body = manifestFor(QStringLiteral("0.1.0"));
        return reply;
    });
    QVERIFY(server.start());

    auto first = checkAgainst(server);
    QSignalSpy firstDone(first.get(), &UpdateCheck::finished);
    QVERIFY(first->start());
    QVERIFY(waitFor([&firstDone] { return firstDone.count() == 1; }));
    QVERIFY(server.received().at(0).header("If-None-Match").isEmpty());

    auto second = checkAgainst(server);
    QSignalSpy found(second.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(second.get(), &UpdateCheck::finished);
    QVERIFY(second->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));

    QCOMPARE(server.received().size(), 2);
    QCOMPARE(server.received().at(1).header("If-None-Match"), QByteArray("\"same\""));
    QCOMPARE(found.count(), 0);
}

void TestUpdateCheck::aWeekLaterItAsksAgainAndAnEvenNewerVersionIsAnnounced()
{
    QString newest = QStringLiteral("0.2.0");
    ScriptedHttpServer server([&newest](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(newest);
        return reply;
    });
    QVERIFY(server.start());

    auto first = checkAgainst(server);
    QSignalSpy firstDone(first.get(), &UpdateCheck::finished);
    QVERIFY(first->start());
    QVERIFY(waitFor([&firstDone] { return firstDone.count() == 1; }));

    // Every day in between: nothing sent, nothing said. Six of them, one by one,
    // because "the day before it is due" is the boundary worth holding.
    for (int day = 1; day <= UpdateCheck::silentDays - 1; ++day) {
        m_today = QDate(2026, 9, 8).addDays(day);
        auto quiet = checkAgainst(server);
        QSignalSpy found(quiet.get(), &UpdateCheck::newVersionFound);
        QVERIFY2(!quiet->start(), qPrintable(QStringLiteral("it asked on day %1").arg(day)));
        drainEvents();
        QCOMPARE(found.count(), 0);
        QCOMPARE(server.received().size(), 1);
    }

    // Seven days after the notice, and not before.
    newest = QStringLiteral("0.3.0");
    m_today = QDate(2026, 9, 8).addDays(UpdateCheck::silentDays);
    auto later = checkAgainst(server);
    QSignalSpy found(later.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(later.get(), &UpdateCheck::finished);
    QVERIFY(later->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));

    QCOMPARE(server.received().size(), 2);
    QCOMPARE(found.count(), 1);
    QCOMPARE(found.at(0).at(0).toString(), QStringLiteral("0.3.0"));
    QCOMPARE(
        m_preferences->value(QStringLiteral("update.announcedOn")).toString(), QStringLiteral("2026-09-15"));
}

void TestUpdateCheck::aVersionAlreadyAnnouncedIsNotAnnouncedAgainWhenTheWeekIsOut()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(QStringLiteral("0.2.0"));
        return reply;
    });
    QVERIFY(server.start());

    auto first = checkAgainst(server);
    QSignalSpy firstDone(first.get(), &UpdateCheck::finished);
    QVERIFY(first->start());
    QVERIFY(waitFor([&firstDone] { return firstDone.count() == 1; }));

    // One notice per version, and that is the whole of it: the week decides when to
    // ask again, and it is not a licence to say the same thing every seven days.
    m_today = QDate(2026, 9, 8).addDays(UpdateCheck::silentDays * 3);
    auto later = checkAgainst(server);
    QSignalSpy found(later.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(later.get(), &UpdateCheck::finished);
    QVERIFY(later->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));

    QCOMPARE(server.received().size(), 2);
    QCOMPARE(found.count(), 0);
}

void TestUpdateCheck::updatingToTheAnnouncedVersionEndsTheSilence()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(QStringLiteral("0.3.0"));
        return reply;
    });
    QVERIFY(server.start());

    // Told about 0.2.0 yesterday, and 0.2.0 is what is running now: somebody did
    // exactly what the notice asked. The silence has nothing left to protect, and
    // keeping it would hide 0.3.0 for the rest of the week as a reward.
    m_preferences->setValue(QStringLiteral("update.announcedVersion"), QStringLiteral("0.2.0"));
    m_preferences->setValue(QStringLiteral("update.announcedOn"), QStringLiteral("2026-09-07"));

    auto check = checkAgainst(server, QStringLiteral("0.2.0"));
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));

    QCOMPARE(found.count(), 1);
    QCOMPARE(found.at(0).at(0).toString(), QStringLiteral("0.3.0"));
}

void TestUpdateCheck::theSwitchStopsTheRequestBeingMadeAtAll()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(QStringLiteral("0.2.0"));
        return reply;
    });
    QVERIFY(server.start());

    m_preferences->setValue(UpdateCheck::enabledKey(), false);

    auto check = checkAgainst(server);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QVERIFY(!check->start());
    drainEvents();

    // Asserted on the absence of the *request*, not of the popup: switching the
    // check off has to stop Mole talking to anybody, which is the promise README
    // makes about it.
    QVERIFY(server.received().isEmpty());
    QCOMPARE(found.count(), 0);

    // And on again resumes, without needing anything else cleared.
    m_preferences->setValue(UpdateCheck::enabledKey(), true);
    auto resumed = checkAgainst(server);
    QSignalSpy done(resumed.get(), &UpdateCheck::finished);
    QVERIFY(resumed->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(server.received().size(), 1);
}

void TestUpdateCheck::anUnreachableServerSaysNothing()
{
    // A port nobody is listening on: the offline machine, the captive portal and
    // the wrong proxy all arrive here in the end.
    ScriptedHttpServer server(
        [](const ScriptedHttpServer::Request&) { return ScriptedHttpServer::Reply {}; });
    QVERIFY(server.start());
    const QString address = server.url();
    server.stop();

    auto check = std::make_unique<UpdateCheck>(m_preferences.get(), QStringLiteral("0.1.0"));
    check->setManifestUrl(QUrl(address + QStringLiteral("/latest.json")));
    check->setTimeout(2000);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(found.count(), 0);
}

void TestUpdateCheck::aServerErrorSaysNothing()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.status = 500;
        reply.reason = "Internal Server Error";
        reply.body = "<html>sorry</html>";
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(found.count(), 0);
    // Nothing remembered from an answer that was not one.
    QVERIFY(!m_preferences->contains(QStringLiteral("update.etag")));
}

void TestUpdateCheck::aBodyThatStopsHalfWaySaysNothing()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.headers << "ETag: \"cut\"";
        reply.body = manifestFor(QStringLiteral("0.2.0"));
        // Promised whole, delivered half. The failure that looks like success --
        // and half a JSON document would not parse anyway, which is the second
        // belt.
        reply.hangUpAfter = 20;
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(found.count(), 0);
    QVERIFY(!m_preferences->contains(QStringLiteral("update.etag")));
}

void TestUpdateCheck::aConnectionThatGoesQuietSaysNothingAndDoesNotWait()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(QStringLiteral("0.2.0"));
        // Open, and sending nothing. Nothing arrives and nothing closes, so the
        // timeout is the only thing that can end it -- which is what a short one
        // is for.
        reply.goQuietAfter = 10;
        reply.stayQuietMs = 5000;
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    check->setTimeout(300);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    // Well inside the five seconds the server means to hold on for: the point is
    // that the check gives up on its own rather than waiting for the far end.
    QVERIFY(waitFor([&done] { return done.count() == 1; }, 3000));
    QCOMPARE(found.count(), 0);
}

void TestUpdateCheck::aManifestThatIsNotADocumentSaysNothing()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = "<!DOCTYPE html><title>404 not found</title>";
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(found.count(), 0);
}

void TestUpdateCheck::aFormatThisBuildDoesNotKnowSaysNothing()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        // A manifest from the future. Fields may be added to that file for ever,
        // and a build from before a change cannot know what changed -- so the only
        // safe answer is the one an unreachable server gets.
        reply.body = manifestFor(QStringLiteral("0.2.0"), UpdateCheck::knownFormat + 1);
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(found.count(), 0);
}

void TestUpdateCheck::aPageThatIsNotHttpsIsRefused()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        // This string arrives over the network and would be handed to whatever
        // opens links on the machine. Nothing but https may reach that.
        reply.body = manifestFor(QStringLiteral("0.2.0"), 1, QStringLiteral("file:///etc/passwd"));
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    QSignalSpy found(check.get(), &UpdateCheck::newVersionFound);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(found.count(), 0);
    // And nothing was announced, so tomorrow's start looks again rather than
    // staying quiet about a version nobody was told about.
    QVERIFY(!m_preferences->contains(QStringLiteral("update.announcedVersion")));
}

void TestUpdateCheck::nothingIdentifyingLeavesTheMachine()
{
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(QStringLiteral("0.2.0"));
        return reply;
    });
    QVERIFY(server.start());

    auto check = checkAgainst(server);
    QSignalSpy done(check.get(), &UpdateCheck::finished);
    QVERIFY(check->start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));
    QCOMPARE(server.received().size(), 1);

    const ScriptedHttpServer::Request asked = server.received().at(0);
    QCOMPARE(asked.method, QByteArray("GET"));
    QCOMPARE(asked.path, QByteArray("/latest.json"));
    // No query string: a version, a platform or a counter smuggled into the URL
    // would be the easiest way to break this promise and the hardest to spot.
    QVERIFY(!asked.path.contains('?'));
    QVERIFY(asked.body.isEmpty());

    // **The one Qt puts there by itself.** With nothing set, Qt builds
    // `Accept-Language` out of the system locale -- `pl-PL,en,*` on a Polish
    // machine -- which is the only thing in a default request that says anything
    // about whoever is asking. The manifest is not translated, so it is replaced
    // with a fixed `en`. This is the assertion that keeps it replaced.
    QCOMPARE(asked.header("Accept-Language"), QByteArray("en"));

    // And no version, platform or install id anywhere in what was sent. The user
    // agent names the application and nothing else: a version here would be a
    // count of installs by release arriving at somebody else's server.
    QCOMPARE(asked.header("User-Agent"), QByteArray("Mole"));
    for (const QByteArray& line : asked.headers) {
        QVERIFY2(
            !line.contains("0.1.0"), qPrintable(QStringLiteral("sent: %1").arg(QString::fromUtf8(line))));
        QVERIFY2(!line.contains(QSysInfo::machineHostName().toUtf8()),
            qPrintable(QStringLiteral("sent: %1").arg(QString::fromUtf8(line))));
        QVERIFY2(!line.contains("Qt/"), qPrintable(QStringLiteral("sent: %1").arg(QString::fromUtf8(line))));
    }
    QVERIFY(asked.header("Cookie").isEmpty());
    QVERIFY(asked.header("Authorization").isEmpty());
}

void TestUpdateCheck::readmeStatesWhatLeavesTheMachine()
{
    // **A README that is wrong about this is worse than one that says nothing.** It
    // is where somebody decides whether they are comfortable with an application
    // that makes a request on their behalf, so the URL and the headers it promises
    // are held against the ones this file actually sends. See MOLE-326.
    QFile readme(QStringLiteral(MOLE_SOURCE_ROOT "/README.md"));
    QVERIFY2(readme.open(QIODevice::ReadOnly), qPrintable(readme.fileName()));
    const QString said = QString::fromUtf8(readme.readAll()).simplified();

    const QString url = UpdateCheck::defaultManifestUrl().toString();
    QVERIFY2(said.contains(url), qPrintable(QStringLiteral("README.md does not state %1").arg(url)));

    // The two headers Mole sets rather than leaving to Qt, both of them because of
    // what a default request would otherwise have said about the machine.
    QVERIFY2(said.contains(QLatin1String("User-Agent: Mole")),
        "README.md does not say what user agent the request carries");
    QVERIFY2(said.contains(QLatin1String("Accept-Language: en")),
        "README.md does not say that the request does not carry the machine's locale");
    QVERIFY2(said.contains(QLatin1String("If-None-Match")),
        "README.md does not say that the request is a conditional one");
}

void TestUpdateCheck::theManifestCanBePointedSomewhereElseFromTheEnvironment()
{
    // Not for this suite, which sets the URL directly. It is for whoever works on
    // the notice, the switch or the documentation: all three are about a version
    // being found, and finding one needs a server that says one exists. The
    // alternative is editing the URL in the source and remembering to put it back.
    ScriptedHttpServer server([](const ScriptedHttpServer::Request&) {
        ScriptedHttpServer::Reply reply;
        reply.body = manifestFor(QStringLiteral("0.2.0"));
        return reply;
    });
    QVERIFY(server.start());

    const QString elsewhere = server.url() + QStringLiteral("/somewhere/else.json");
    qputenv("MOLE_UPDATE_MANIFEST", elsewhere.toLocal8Bit());
    QCOMPARE(UpdateCheck::defaultManifestUrl(), QUrl(elsewhere));

    UpdateCheck check(m_preferences.get(), QStringLiteral("0.1.0"));
    check.setTimeout(2000);
    QSignalSpy found(&check, &UpdateCheck::newVersionFound);
    QSignalSpy done(&check, &UpdateCheck::finished);
    QVERIFY(check.start());
    QVERIFY(waitFor([&done] { return done.count() == 1; }));

    QCOMPARE(server.received().size(), 1);
    QCOMPARE(server.received().at(0).path, QByteArray("/somewhere/else.json"));
    QCOMPARE(found.count(), 1);

    qunsetenv("MOLE_UPDATE_MANIFEST");
    // And with it unset the published manifest is what is asked, which is the half
    // that matters in a shipped build.
    QCOMPARE(UpdateCheck::defaultManifestUrl().host(), QStringLiteral("raw.githubusercontent.com"));
}

MOLE_TEST_MAIN(TestUpdateCheck)
#include "tst_UpdateCheck.moc"
