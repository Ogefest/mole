#include "ui/UpdateCheck.h"

#include "core/diagnostics/Diagnostics.h"
#include "core/settings/Preferences.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace mole {
namespace {

    /// What was read last time, so the next look is a conditional one.
    QString etagKey()
    {
        return QStringLiteral("update.etag");
    }
    /// Which version somebody has already been told about, and the day they were.
    /// Not "when we last looked": the week of silence is about the telling.
    QString announcedVersionKey()
    {
        return QStringLiteral("update.announcedVersion");
    }
    QString announcedOnKey()
    {
        return QStringLiteral("update.announcedOn");
    }

    /// Three numbers, or nothing. `release.sh` only ever cuts `x.y.z`, so anything
    /// else is a manifest that has been edited by hand into a shape this cannot
    /// compare -- and comparing it wrongly is worse than not answering.
    bool parts(const QString& version, int out[3])
    {
        const QStringList pieces = version.split(QLatin1Char('.'));
        if (pieces.size() != 3)
            return false;
        for (int i = 0; i < 3; ++i) {
            bool numeric = false;
            out[i] = pieces.at(i).toInt(&numeric);
            if (!numeric || out[i] < 0)
                return false;
        }
        return true;
    }

} // namespace

QUrl UpdateCheck::defaultManifestUrl()
{
    // Somewhere else, when somebody is working on what happens next.
    //
    // The notice, the switch and the documentation are all about a version being
    // found, and a version can only be found by asking a server that says one
    // exists. Without this the only way to see any of it is to edit the URL below,
    // which is a change nobody should have to make and remember to undo. Read the
    // way every other MOLE_* parameter is -- see README.md -- and never a
    // constant.
    const QByteArray elsewhere = qgetenv("MOLE_UPDATE_MANIFEST");
    if (!elsewhere.isEmpty())
        return QUrl(QString::fromLocal8Bit(elsewhere));

    // `raw` rather than the GitHub API, and this is the measured reason: raw
    // answers with `cache-control: max-age=300` and an `ETag`, so the ordinary
    // answer to the request below is `304` with no body. The API answers the same
    // question and carries 60 requests per IP per hour unauthenticated -- a few
    // dozen people behind one NAT starting Mole in the morning would exhaust it,
    // and the check would then quietly stop working for all of them, looking
    // exactly like being up to date. See ADR-0084.
    return QUrl(QStringLiteral("https://raw.githubusercontent.com/Ogefest/mole/main/latest.json"));
}

QString UpdateCheck::enabledKey()
{
    return QStringLiteral("update.check");
}

UpdateCheck::UpdateCheck(Preferences* preferences, QString runningVersion, QObject* parent)
    : QObject(parent)
    , m_preferences(preferences)
    , m_running(std::move(runningVersion))
    , m_manifest(defaultManifestUrl())
    , m_calendar([] { return QDate::currentDate(); })
{
}

UpdateCheck::~UpdateCheck() = default;

void UpdateCheck::setManifestUrl(QUrl url)
{
    m_manifest = std::move(url);
}

void UpdateCheck::setTimeout(int milliseconds)
{
    m_timeout = milliseconds;
}

void UpdateCheck::setCalendar(Calendar calendar)
{
    m_calendar = calendar ? std::move(calendar) : Calendar([] { return QDate::currentDate(); });
}

bool UpdateCheck::isNewer(const QString& candidate, const QString& running)
{
    int mine[3] = { 0, 0, 0 };
    int theirs[3] = { 0, 0, 0 };
    if (!parts(candidate, mine) || !parts(running, theirs))
        return false;
    for (int i = 0; i < 3; ++i) {
        if (mine[i] != theirs[i])
            return mine[i] > theirs[i];
    }
    return false;
}

bool UpdateCheck::start()
{
    if (!m_preferences)
        return false;

    // The switch, which MOLE-325 puts in the Help menu. Default on, which is the
    // author's decision of 2026-09-01 and the reason README.md has to say that this
    // request happens at all.
    if (!m_preferences->value(enabledKey(), true).toBool()) {
        qCDebug(updateLog) << "not looking: the check is switched off";
        return false;
    }

    // The week of silence, and it holds the *request* back as well as the notice:
    // somebody who was told and did nothing is not helped by being asked about
    // again, and the request is worth saving too.
    //
    // **It stops holding once the announced version is the one running.** Somebody
    // who took the notice and updated has acted on it, so there is nothing left for
    // the silence to protect -- and keeping it would mean missing the next release
    // for up to a week as a reward for doing exactly what was asked.
    const QString announced = m_preferences->value(announcedVersionKey()).toString();
    const QDate announcedOn
        = QDate::fromString(m_preferences->value(announcedOnKey()).toString(), Qt::ISODate);
    if (!announced.isEmpty() && announcedOn.isValid() && isNewer(announced, m_running)) {
        const QDate askAgain = announcedOn.addDays(silentDays);
        if (m_calendar() < askAgain) {
            qCDebug(updateLog) << "staying quiet about" << announced << "until"
                               << askAgain.toString(Qt::ISODate);
            return false;
        }
    }

    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    QNetworkRequest request(m_manifest);
    // One attempt, with a short bound on it. Nothing retries: a machine that is
    // offline now is asked again the next time Mole starts, and a check that keeps
    // trying is a check somebody eventually notices.
    request.setTransferTimeout(m_timeout);

    // **What leaves the machine, measured rather than assumed.** With nothing set
    // at all, Qt 6.4.2 sends:
    //
    //     GET /<path> HTTP/1.1
    //     Host: <host>
    //     Connection: Keep-Alive
    //     Accept-Encoding: zstd, br, gzip, deflate
    //     Accept-Language: en-US,*          <- the machine's locale
    //     User-Agent: Mozilla/5.0
    //
    // `Accept-Language` is built out of the system locale -- `pl-PL,en,*` on a
    // Polish machine -- and it is the only thing in that list that says anything
    // about whoever is asking. The manifest is not translated, so it buys nothing
    // and is replaced with a fixed `en`. Setting it to an empty value does not
    // work: Qt puts its own back.
    //
    // The user agent is replaced as well, with a bare `Mole` carrying no version
    // and no platform. The default is a browser's name, which is untrue, and a
    // version here would be a count of installs by release arriving at somebody
    // else's server -- which is exactly what must not leave.
    //
    // Nothing else is added: no install id, no counter, no platform string. What
    // the request looks like is asserted in tst_UpdateCheck, so it cannot drift.
    request.setRawHeader("Accept-Language", "en");
    request.setRawHeader("User-Agent", "Mole");

    const QString etag = m_preferences->value(etagKey()).toString();
    if (!etag.isEmpty())
        request.setRawHeader("If-None-Match", etag.toLatin1());

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handle(reply); });
    qCDebug(updateLog) << "asking" << m_manifest.toString() << "what the newest release is";
    return true;
}

void UpdateCheck::handle(QNetworkReply* reply)
{
    reply->deleteLater();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // A 304 arrives as an answer rather than as an error -- no cache is configured,
    // so Qt hands it over with this status and an empty body. It means the file has
    // not changed since it was last read, and therefore that anything worth saying
    // about it has been said.
    if (status == 304) {
        qCDebug(updateLog) << "the manifest has not changed since the last look";
        emit finished();
        return;
    }

    // Everything else that is not a whole 200: unreachable, refused, timed out,
    // 404, 500, a body that stopped half way -- one line at debug level, which is
    // silent unless somebody asked for `MOLE_LOG=update`, and nothing else.
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        qCDebug(updateLog) << "no answer worth having:" << status << reply->errorString();
        emit finished();
        return;
    }

    // Read only now that the answer is known to be a whole 200. Reading a reply
    // that timed out or was refused means reading a device Qt has already closed,
    // and Qt says so with a warning of its own -- which would put a line in the
    // session log every time somebody started Mole on a train. The check is not
    // allowed to be the loudest thing about being offline.
    const QByteArray body = reply->readAll();

    // Remembered before the body is looked at, and on purpose: the next run should
    // not fetch these bytes again whether or not this build could make sense of
    // them. The same file will not have become readable in the meantime, and a file
    // that changes brings a new ETag with it.
    const QByteArray tag = reply->rawHeader("ETag");
    if (!tag.isEmpty())
        m_preferences->setValue(etagKey(), QString::fromLatin1(tag));

    QString version;
    QUrl page;
    if (!readManifest(body, &version, &page)) {
        emit finished();
        return;
    }

    if (!isNewer(version, m_running)) {
        qCDebug(updateLog) << "the newest release is" << version << "and this is" << m_running;
        emit finished();
        return;
    }

    // Told once, and that is the whole of it. The week above decides when to ask
    // again; it is not a second telling, and a version already announced stays
    // announced however many times it comes back.
    if (version == m_preferences->value(announcedVersionKey()).toString()) {
        qCDebug(updateLog) << version << "has been announced already";
        emit finished();
        return;
    }

    announce(version, page);
    emit finished();
}

bool UpdateCheck::readManifest(const QByteArray& body, QString* version, QUrl* page) const
{
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qCDebug(updateLog) << "the manifest is not a document:" << error.errorString();
        return false;
    }
    const QJsonObject manifest = document.object();

    // A format this build has never heard of is treated exactly like an
    // unreachable server. Fields may be added to that file for ever, and a build
    // from before an addition cannot know what was added -- so the only safe answer
    // to a number it does not recognise is to say nothing at all.
    const int format = manifest.value(QStringLiteral("format")).toInt(-1);
    if (format != knownFormat) {
        qCDebug(updateLog) << "the manifest is format" << format << "and this build reads" << knownFormat;
        return false;
    }

    *version = manifest.value(QStringLiteral("version")).toString();
    int ignored[3] = { 0, 0, 0 };
    if (!parts(*version, ignored)) {
        qCDebug(updateLog) << "the manifest names no version this can compare:" << *version;
        return false;
    }

    // **The landing page is the manifest's own field and is never assembled here.**
    // That is the entire reason the field exists -- pointing releases at a real
    // page one day is an edit to one file and no change to anything installed -- so
    // a manifest with nothing usable in it is one this cannot act on, and it says
    // nothing rather than inventing a URL.
    //
    // `https` only, and not because the page is secret: this string arrives over
    // the network and is handed to whatever opens links on the machine, so the one
    // scheme it may be is the one the manifest itself came over. A `file:` or a
    // `javascript:` in that field must never reach a browser.
    *page = QUrl(manifest.value(QStringLiteral("url")).toString());
    if (!page->isValid() || page->scheme() != QLatin1String("https") || page->host().isEmpty()) {
        qCDebug(updateLog) << "the manifest names no page anything may open:" << page->toString();
        return false;
    }
    return true;
}

void UpdateCheck::announce(const QString& version, const QUrl& page)
{
    // Written down before the signal goes out, not after. Showing the notice is
    // what counts as having announced the version -- whatever the person then does
    // with it, dismissing included -- and a window that closes while the popup is
    // going up must not cause the same version to be announced again tomorrow.
    m_preferences->setValue(announcedVersionKey(), version);
    m_preferences->setValue(announcedOnKey(), m_calendar().toString(Qt::ISODate));

    qCInfo(updateLog).noquote() << "Mole" << version << "has been released:" << page.toString();
    emit newVersionFound(version, page);
}

} // namespace mole
