#pragma once

#include <QDate>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace mole {

class Preferences;

/// Whether a newer Mole exists, asked once when the application starts.
///
/// Mole is in no distribution's archive, so every copy of it was installed by hand
/// and nothing would ever mention that a new one had been published. This asks:
/// one conditional `GET` of the manifest `make release` writes -- see
/// [ADR-0084](../../docs/adr/0084-the-newest-release-is-stated-in-a-file-at-a-fixed-path.md)
/// -- and a signal when what it names is newer than what is running.
///
/// **Nothing downloads or installs anything.** Mole says a version exists and
/// hands over a page to open. A file manager that rewrites its own binary is a
/// different application with a different threat model.
///
/// **Nothing here is loud.** An offline machine, a captive portal, a proxy, a DNS
/// failure, a 500, a body that stops half way, a manifest in a format this build
/// has never heard of: every one of them produces no message, no warning in the
/// session log and no delay anybody notices. An update check that complains is
/// worse than no update check.
///
/// **No thread, and that is not a compromise.** `QNetworkAccessManager` is
/// asynchronous on the event loop -- the request goes out, a signal comes back --
/// so the window is never held up and there is nothing to synchronise. A thread
/// here would be more code for the same result; do not add one.
///
/// **One notice per version, then a week of silence.** Somebody told about a
/// version who does nothing does not want telling again, and there is no sense
/// spending the request either. So what is remembered is *which version was
/// announced and when*, rather than merely when we last looked. The deliberate
/// cost: a release appearing two days into that week is not found until the week
/// is out.
class UpdateCheck : public QObject
{
    Q_OBJECT

public:
    /// The one URL every binary that ships from now on asks for the rest of its
    /// life. It can never move: a copy of Mole released years ago cannot be told
    /// the file went somewhere else, since being told things is what it is for.
    ///
    /// `MOLE_UPDATE_MANIFEST` points it somewhere else, which is how anybody
    /// working on the notice sees a version being found without editing code.
    static QUrl defaultManifestUrl();

    /// How long a notice keeps this quiet afterwards -- the request included.
    static constexpr int silentDays = 7;

    /// The manifest format this build reads. Fields may be added to that file for
    /// ever; a file announcing any other format is treated exactly like an
    /// unreachable server, because a build from before a change cannot know what
    /// the change was.
    static constexpr int knownFormat = 1;

    /// Whether to look at all. The Help menu's switch is the same key, which is
    /// why it is named here rather than spelled out twice.
    static QString enabledKey();

    /// `runningVersion` is what this build is. Passed in rather than read from
    /// `MOLE_VERSION` here, so a test can be an old version without being rebuilt.
    UpdateCheck(Preferences* preferences, QString runningVersion, QObject* parent = nullptr);
    ~UpdateCheck() override;

    /// Where the manifest is. For tests; the application uses the default.
    void setManifestUrl(QUrl url);
    /// How long the one attempt gets. Short by intent, and nothing retries.
    void setTimeout(int milliseconds);

    /// What day it is. Replaceable so a test can be next week without waiting for
    /// one, which is the only way the week of silence can be checked at all.
    using Calendar = std::function<QDate()>;
    void setCalendar(Calendar calendar);

    /// Asks, if asking is due. False when nothing was sent -- the switch is off,
    /// or a notice is still inside its week -- and then no signal follows either.
    bool start();

    /// Newer on the three numbers rather than as text, so 0.10.0 is newer than
    /// 0.9.0. Anything not shaped like three numbers is never newer than anything.
    static bool isNewer(const QString& candidate, const QString& running);

signals:
    /// A version worth telling somebody about, and `page` is the manifest's own
    /// landing page -- never a URL this assembled. At most once per version.
    void newVersionFound(const QString& version, const QUrl& page);

    /// The answer has been dealt with, whatever it was. Nothing in the application
    /// acts on this; it is what lets a test wait for the condition rather than for
    /// a clock.
    void finished();

private:
    void handle(QNetworkReply* reply);
    void announce(const QString& version, const QUrl& page);
    /// The manifest, or nothing at all when it says something this cannot act on.
    bool readManifest(const QByteArray& body, QString* version, QUrl* page) const;

    Preferences* m_preferences = nullptr;
    QString m_running;
    QUrl m_manifest;
    int m_timeout = 5000;
    Calendar m_calendar;
    QNetworkAccessManager* m_network = nullptr;
};

} // namespace mole
