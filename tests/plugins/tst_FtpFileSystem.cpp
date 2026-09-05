#include "plugins/network/FtpFileSystem.h"
#include "plugins/network/TransferStreams.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"

#include <QHash>
#include <QUrl>

#include <algorithm>
#include <cstring>
#include <curl/curl.h>

using namespace mole;
using namespace mole::test;

namespace {

struct Account
{
    QString host;
    int port = 21;
    QString user;
    QString password;
    QString base;
    bool requireTls = true;
    /// The server's certificate is honestly self-signed, so check that it is
    /// there rather than that somebody vouched for it.
    ///
    /// TLS stays required either way -- this changes the trust anchor, not
    /// whether the connection is encrypted. A disposable test server cannot
    /// have a certificate anybody would vouch for, and the alternative is
    /// teaching every machine that runs this suite to trust an authority
    /// invented for the occasion.
    bool ignoreSelfSignedCert = false;

    bool isConfigured() const { return !host.isEmpty() && !user.isEmpty(); }
};

Account accountFromEnvironment()
{
    const auto value = [](const char* name) { return QString::fromLocal8Bit(qgetenv(name)); };

    Account account;
    account.host = value("MOLE_TEST_FTP_HOST");
    account.user = value("MOLE_TEST_FTP_USER");
    account.password = value("MOLE_TEST_FTP_PASS");
    account.base = value("MOLE_TEST_FTP_BASE");
    if (account.base.isEmpty())
        account.base = QStringLiteral("/Shared");

    const int port = value("MOLE_TEST_FTP_PORT").toInt();
    account.port = port > 0 ? port : 21;
    account.requireTls = value("MOLE_TEST_FTP_TLS") != QLatin1String("none");
    account.ignoreSelfSignedCert = !value("MOLE_TEST_IGNORE_SELF_SIGNED_CERT").isEmpty();
    return account;
}

/// What one raw request did, in libcurl's own words.
///
/// **A bool said "it failed" and every assertion guessed why.** The three cases
/// that make a working directory said "could not create the working directory on
/// the server" whatever went wrong -- and what actually goes wrong is a login the
/// server refused: this client verifies the certificate unless
/// MOLE_TEST_IGNORE_SELF_SIGNED_CERT is set, the live service presents a
/// self-signed one, and test-live.sh sets that where a run by hand does not. So a
/// handshake that never completed was reported as a permission on a directory,
/// and was carded as one. See MOLE-422, and MOLE-327 and MOLE-328 for the same
/// shape in the product.
struct RawOutcome
{
    CURLcode code = CURLE_OK;
    /// The last FTP response code, where there was one. Zero when the exchange
    /// never got that far.
    long status = 0;

    explicit operator bool() const { return code == CURLE_OK; }

    QString reason() const
    {
        if (code == CURLE_OK)
            return {};
        QString said = QString::fromLatin1(curl_easy_strerror(code));
        if (status > 0)
            said += QStringLiteral(" (the server last answered %1)").arg(status);
        return said;
    }
};

/// Seeds through plain libcurl, so the fixtures do not come from the code under
/// test -- the same reasoning as the SFTP and S3 suites.
class RawFtp
{
public:
    explicit RawFtp(const Account& account)
        : m_account(account)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    QByteArray urlFor(const QString& path, bool asDirectory) const
    {
        QByteArray url = "ftp://" + m_account.host.toUtf8() + ':' + QByteArray::number(m_account.port);
        url += QUrl::toPercentEncoding(path, "/");
        if (asDirectory && !url.endsWith('/'))
            url += '/';
        return url;
    }

    RawOutcome command(const QByteArray& text, const QString& contextDir) const
    {
        CURL* handle = prepare();
        if (!handle)
            return { CURLE_FAILED_INIT, 0 };
        const QByteArray url = urlFor(contextDir, true);
        curl_slist* commands = curl_slist_append(nullptr, text.constData());
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_QUOTE, commands);
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
        const CURLcode code = curl_easy_perform(handle);
        const RawOutcome outcome { code, lastStatus(handle) };
        curl_slist_free_all(commands);
        curl_easy_cleanup(handle);
        return outcome;
    }

    RawOutcome putFile(const QString& path, const QByteArray& contents) const
    {
        CURL* handle = prepare();
        if (!handle)
            return { CURLE_FAILED_INIT, 0 };
        Payload payload { contents, 0 };
        const QByteArray url = urlFor(path, false);
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(handle, CURLOPT_READFUNCTION, readPayload);
        curl_easy_setopt(handle, CURLOPT_READDATA, &payload);
        curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(contents.size()));
        const CURLcode code = curl_easy_perform(handle);
        const RawOutcome outcome { code, lastStatus(handle) };
        curl_easy_cleanup(handle);
        return outcome;
    }

    /// One ranged retrieve, exactly as a streamed read issues it: both ends of
    /// CURLOPT_RANGE set, which for FTP becomes REST plus RETR. Returns what
    /// came back, so a caller can weigh it as well as read it.
    QByteArray getRange(const QString& path, qint64 from, qint64 to) const
    {
        QByteArray body;
        CURL* handle = prepare();
        if (!handle)
            return body;
        const QByteArray url = urlFor(path, false);
        const QByteArray range = QByteArray::number(from) + '-' + QByteArray::number(to);
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_RANGE, range.constData());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
        const CURLcode code = curl_easy_perform(handle);
        curl_easy_cleanup(handle);
        return code == CURLE_OK ? body : QByteArray();
    }

    void removeTree(const QString& path) const
    {
        QByteArray listing;
        CURL* handle = prepare();
        if (!handle)
            return;
        const QByteArray url = urlFor(path, true);
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &listing);
        const bool listed = curl_easy_perform(handle) == CURLE_OK;
        curl_easy_cleanup(handle);
        if (!listed)
            return;

        for (const QString& line : QString::fromUtf8(listing).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const int space = line.lastIndexOf(QLatin1Char(' '));
            if (space < 0)
                continue;
            const QString name = line.mid(space + 1).trimmed();
            if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
                continue;
            const QString child = path + QLatin1Char('/') + name;
            if (line.startsWith(QLatin1Char('d')))
                removeTree(child);
            else
                command("DELE " + child.toUtf8(), path);
        }
        command("RMD " + path.toUtf8(), parentOf(path));
    }

    static QString parentOf(const QString& path)
    {
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        return slash > 0 ? path.left(slash) : QStringLiteral("/");
    }

private:
    struct Payload
    {
        QByteArray data;
        qint64 offset = 0;
    };

    static size_t readPayload(char* buffer, size_t size, size_t count, void* userData)
    {
        auto* payload = static_cast<Payload*>(userData);
        const qint64 remaining = payload->data.size() - payload->offset;
        const qint64 wanted = std::min<qint64>(static_cast<qint64>(size * count), remaining);
        if (wanted <= 0)
            return 0;
        memcpy(buffer, payload->data.constData() + payload->offset, static_cast<size_t>(wanted));
        payload->offset += wanted;
        return static_cast<size_t>(wanted);
    }

    static size_t collect(char* data, size_t size, size_t count, void* userData)
    {
        static_cast<QByteArray*>(userData)->append(data, static_cast<int>(size * count));
        return size * count;
    }

    /// The last response the server gave, or zero. Read from the handle rather
    /// than parsed out of anything: libcurl keeps it for exactly this.
    static long lastStatus(CURL* handle)
    {
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        return status;
    }

    CURL* prepare() const
    {
        CURL* handle = curl_easy_init();
        if (!handle)
            return nullptr;
        curl_easy_setopt(handle, CURLOPT_USERNAME, m_account.user.toUtf8().constData());
        curl_easy_setopt(handle, CURLOPT_PASSWORD, m_account.password.toUtf8().constData());
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        if (m_account.requireTls)
            curl_easy_setopt(handle, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        if (m_account.ignoreSelfSignedCert) {
            curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        return handle;
    }

    Account m_account;
};

} // namespace

/// What every case that needs the server says first.
///
/// **A macro rather than a helper, because QSKIP and QFAIL return from the
/// function they are written in.** In a helper they mark the result and then
/// return from the helper, and the case carries on to fail again in its own
/// words -- which is the fault this card is about, in a new place.
///
/// Two situations that look alike and are not. No `MOLE_TEST_FTP_*` at all is a
/// skip: nothing was asked of a server and nothing is claimed about one.
/// Configured and refused is a failure naming what libcurl said, and the case
/// does not run -- a suite that cannot log in has nothing to say about FTP, and
/// saying it once per case in the wrong words is what cost a day. That is
/// MOLE-328's distinction and its inverse. See MOLE-422.
#define MOLE_FTP_SERVER_OR_STOP()                                                                            \
    do {                                                                                                     \
        if (!loginRefused().isEmpty())                                                                       \
            QFAIL(qPrintable(loginRefused()));                                                               \
        if (!accountFromEnvironment().isConfigured()) {                                                      \
            QSKIP("No FTP account in the environment; set MOLE_TEST_FTP_HOST, MOLE_TEST_FTP_USER "           \
                  "and MOLE_TEST_FTP_PASS to run this against a real server.");                              \
        }                                                                                                    \
    } while (false)

class TestFtpFileSystem : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void aLoginThatIsRefusedSaysWhatTheServerSaidRatherThanNamingADirectory();
    void aFormWithoutAHostIsRefused();
    void anEmptyUserBecomesAnonymous();
    void encryptionDefaultsToTryingTls();
    void theFormAsksOnlyWhatFtpNeeds();
    void everyUrlNamesAnAbsolutePathTheWayFtpRequires();
    void aListingNobodyCanParseIsAnErrorRatherThanAnEmptyFolder();
    void aMachineReadableListingIsReadWithoutGuessing();
    void aNameWithALineBreakCannotCarryASecondCommand();
    void aWriteStreamsRatherThanStagingTheWholeFile();
    void aLargeReadStreamsRatherThanStagingTheWholeFile();
    void rangedFetchDeliversExactlyTheSpanItAsksFor();
    void aFileOverTheThresholdReadsBackByteForByte();
    void itSatisfiesTheConformanceSuite();

private:
    /// Whether this run may talk to a server, decided once.
    ///
    /// **Two situations that look alike and are not.** No `MOLE_TEST_FTP_*` at
    /// all is a skip: nothing was asked of a server and nothing is claimed about
    /// one. Configured and refused is a failure naming what libcurl said, and the
    /// cases do not run -- a suite that cannot log in has nothing to say about
    /// FTP, and running twelve cases to say it twelve times in the wrong words is
    /// what cost a day. That is MOLE-328's distinction and its inverse: a tier
    /// that ran and failed must not read as a machine that cannot run one, and an
    /// unconfigured machine must not read as a fault in the code. See MOLE-422.
    ///
    /// Empty unless the login was tried and refused. Read by
    /// MOLE_FTP_SERVER_OR_STOP(), which is how every live case asks.
    QString loginRefused() const { return m_loginRefused; }

    /// Empty when the login worked or was never tried. See initTestCase().
    QString m_loginRefused;
};

void TestFtpFileSystem::initTestCase()
{
    const Account account = accountFromEnvironment();
    if (account.isConfigured()) {
        // One exchange, before any case: a PWD in the base the cases work in.
        // Cheap, and it fails for every reason a case would have failed for -- a
        // handshake that cannot complete, a login the server rejects, a base
        // that is not there.
        const RawFtp raw(account);
        const RawOutcome reached = raw.command("PWD", account.base);
        if (!reached) {
            m_loginRefused = QStringLiteral("cannot log in to the FTP server this suite is pointed at: %1")
                                 .arg(reached.reason());
        }
    }
}

/// What a refused login reads as, which is what this whole card is about.
///
/// Pointed at a port nothing answers on, so it needs no server and runs
/// everywhere. The old message would have called this a directory that could not
/// be created; what it has to say is what libcurl said. See MOLE-422.
void TestFtpFileSystem::aLoginThatIsRefusedSaysWhatTheServerSaidRatherThanNamingADirectory()
{
    Account nowhere;
    nowhere.host = QStringLiteral("127.0.0.1");
    // A port in the range nothing is allowed to listen on by convention, and one
    // this machine is not using: a connection here is refused at once.
    nowhere.port = 9;
    nowhere.user = QStringLiteral("nobody");
    nowhere.password = QStringLiteral("nothing");
    nowhere.base = QStringLiteral("/Shared");

    const RawFtp raw(nowhere);
    const RawOutcome outcome = raw.command("MKD /Shared/anything", nowhere.base);
    QVERIFY2(!outcome, "a port nothing answers on cannot have made a directory");

    const QString said = outcome.reason();
    QVERIFY2(!said.isEmpty(), "a refusal with no words is what this case exists to stop");
    // libcurl's own sentence, whatever this build words it as -- what is refused
    // is a message that names something the exchange never reached.
    QVERIFY2(!said.contains(QStringLiteral("directory"), Qt::CaseInsensitive), qPrintable(said));
    QVERIFY2(said.contains(QStringLiteral("connect"), Qt::CaseInsensitive)
            || said.contains(QStringLiteral("refused"), Qt::CaseInsensitive)
            || said.contains(QStringLiteral("timed out"), Qt::CaseInsensitive),
        qPrintable(QStringLiteral("this does not read as a connection that failed: %1").arg(said)));
}

void TestFtpFileSystem::aFormWithoutAHostIsRefused()
{
    FtpFileSystemFactory factory;
    QString error;
    QVERIFY(factory.create(QVariantMap {}, &error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("host")), qPrintable(error));
}

void TestFtpFileSystem::anEmptyUserBecomesAnonymous()
{
    // Public FTP servers want "anonymous", and making someone type it would be
    // asking a question whose answer is always the same.
    FtpFileSystemFactory factory;
    QString error;
    const QVariantMap config { { QStringLiteral("host"), QStringLiteral("ftp.example.org") } };
    const FileSystemPtr fs = factory.create(config, &error);
    QVERIFY2(fs != nullptr, qPrintable(error));
}

void TestFtpFileSystem::encryptionDefaultsToTryingTls()
{
    const FtpSettings byDefault = FtpFileSystemFactory::settingsFrom(QVariantMap {});
    QVERIFY(byDefault.security == FtpSettings::Security::Try);
    QVERIFY(byDefault.passive);
    QCOMPARE(byDefault.port, 21);

    const FtpSettings required = FtpFileSystemFactory::settingsFrom(
        QVariantMap { { QStringLiteral("security"), QStringLiteral("require") } });
    QVERIFY(required.security == FtpSettings::Security::Require);

    const FtpSettings none = FtpFileSystemFactory::settingsFrom(
        QVariantMap { { QStringLiteral("security"), QStringLiteral("none") } });
    QVERIFY(none.security == FtpSettings::Security::None);
}

void TestFtpFileSystem::theFormAsksOnlyWhatFtpNeeds()
{
    const FtpFileSystemFactory factory;
    const QList<ConnectionField> fields = factory.connectionFields();
    QVERIFY2(fields.size() <= 8, "the FTP form should stay short enough to fill in");

    bool hasSecurity = false;
    for (const ConnectionField& field : fields)
        hasSecurity = hasSecurity || field.key == QLatin1String("security");
    // FTP logs in with a plain-text password, so the encryption choice belongs in
    // front of the user rather than hidden behind "advanced".
    QVERIFY(hasSecurity);

    // And the setting that decides whether "Require TLS" can be met at all.
    // FtpSettings::verifyTls was read by settingsFrom() and documented at length
    // with no field to set it from, so a self-signed FTPS server was unreachable
    // and nobody could say so. WebDAV and S3 both offer it. See MOLE-349.
    bool hasVerify = false;
    for (const ConnectionField& field : fields)
        hasVerify = hasVerify || field.key == QLatin1String("verifyTls");
    QVERIFY2(hasVerify, "the certificate check has to be settable from the form");

    QVERIFY(
        FtpFileSystemFactory::settingsFrom({ { QStringLiteral("host"), QStringLiteral("h") } }).verifyTls);
    QVERIFY(!FtpFileSystemFactory::settingsFrom(
        { { QStringLiteral("host"), QStringLiteral("h") }, { QStringLiteral("verifyTls"), false } })
                 .verifyTls);
}

/// Two addressing schemes, one drive.
///
/// libcurl implements RFC 1738 for FTP: a path in the url is a sequence of CWDs
/// *relative to the login directory*, and an absolute path is spelled with an
/// escaped slash. The QUOTE commands beside these urls -- MKD, DELE, RMD,
/// RNFR/RNTO -- carry the absolute path, so the two agreed only while the login
/// directory happened to be "/". That is true of the chrooted account the live
/// suite uses, which is why it never showed there, and false of an ordinary user
/// whose home is /home/alice: the listing showed /home/alice/notes.txt while
/// `DELE /notes.txt` went for the root's, and every upload's closing rename
/// failed -- so every upload was removed as litter. See MOLE-349.
void TestFtpFileSystem::everyUrlNamesAnAbsolutePathTheWayFtpRequires()
{
    FtpSettings settings;
    settings.host = QStringLiteral("ftp.example.com");
    settings.port = 21;
    FtpFileSystem drive(QStringLiteral("ftp"), settings);

    const VfsUri file(QStringLiteral("ftp"), QString(), QStringLiteral("/x"));
    QCOMPARE(drive.urlFor(file, false), QByteArray("ftp://ftp.example.com:21/%2Fx"));

    const VfsUri deeper(QStringLiteral("ftp"), QString(), QStringLiteral("/home/alice/notes.txt"));
    QCOMPARE(drive.urlFor(deeper, false), QByteArray("ftp://ftp.example.com:21/%2Fhome/alice/notes.txt"));

    // A directory ends in a slash, which is what tells curl to CWD into it
    // rather than to fetch a file of that name.
    const VfsUri folder(QStringLiteral("ftp"), QString(), QStringLiteral("/home/alice"));
    QCOMPARE(drive.urlFor(folder, true), QByteArray("ftp://ftp.example.com:21/%2Fhome/alice/"));

    // And the drive root, which is the case where there is nothing after the
    // escaped slash at all.
    const VfsUri root(QStringLiteral("ftp"), QString(), QStringLiteral("/"));
    QCOMPARE(drive.urlFor(root, true), QByteArray("ftp://ftp.example.com:21/%2F/"));

    // A configured root is part of the absolute path and not a second scheme.
    settings.remoteRoot = QStringLiteral("/srv/share");
    FtpFileSystem rooted(QStringLiteral("ftp"), settings);
    QCOMPARE(rooted.urlFor(file, false), QByteArray("ftp://ftp.example.com:21/%2Fsrv/share/x"));
}

/// "This directory is empty" is an instruction to a mirror.
///
/// The listing parser drops every row that does not look like a Unix `ls -l`
/// line, and the survivors were the directory. IIS in its default DOS format, a
/// server configured in another locale, or any appliance printing its own thing
/// therefore parsed to zero rows from a body full of files -- and a sync saw
/// nothing to copy, a delete found nothing, and a mirror had a list of things to
/// remove. A body that said something and yielded nothing is a format this build
/// does not understand, which is a different sentence. See MOLE-349.
void TestFtpFileSystem::aListingNobodyCanParseIsAnErrorRatherThanAnEmptyFolder()
{
    const QDateTime now = QDateTime::currentDateTime();

    // What IIS prints unless it is told otherwise.
    const QByteArray dos = "01-23-24  10:15AM       <DIR>          reports\r\n"
                           "01-23-24  10:16AM               1024 notes.txt\r\n";
    const Result<QList<net::ListingRow>> refused = FtpFileSystem::rowsFrom(dos, false, now);
    QVERIFY2(!refused.ok(), "a listing in a format this build cannot read is not an empty directory");
    QCOMPARE(refused.error().code, VfsError::IoError);

    // An empty body, on the other hand, really is an empty directory.
    const Result<QList<net::ListingRow>> empty = FtpFileSystem::rowsFrom(QByteArray(), false, now);
    QVERIFY2(empty.ok(), "a server that listed nothing listed nothing");
    QVERIFY(empty.value().isEmpty());
    QVERIFY(FtpFileSystem::rowsFrom(QByteArray("\r\n"), false, now).value().isEmpty());

    // And the format that does work still works.
    const QByteArray unix = "total 8\r\n"
                            "drwxr-xr-x 2 alice alice 4096 Jan 23 10:15 reports\r\n"
                            "-rw-r--r-- 1 alice alice 1024 Jan 23 10:16 notes.txt\r\n";
    const Result<QList<net::ListingRow>> read = FtpFileSystem::rowsFrom(unix, false, now);
    QVERIFY2(read.ok(), qPrintable(read.error().message));
    QCOMPARE(read.value().size(), 2);
}

/// The format that needs no guessing at all.
///
/// MLSD is RFC 3659: one fact list and a name, in UTC and in no locale. It is
/// asked for before LIST now, because every fault above is a property of a format
/// meant for a person to read. See MOLE-349.
void TestFtpFileSystem::aMachineReadableListingIsReadWithoutGuessing()
{
    const QDateTime now = QDateTime::currentDateTime();
    const QByteArray mlsd
        = "type=cdir;modify=20240123101500; /home/alice\r\n"
          "type=pdir;modify=20240123101500; /home\r\n"
          "type=dir;sizd=4096;modify=20240123101500;UNIX.mode=0755; reports\r\n"
          "type=file;size=1024;modify=20240123101600;UNIX.mode=0644; notes and drafts.txt\r\n";

    const Result<QList<net::ListingRow>> parsed = FtpFileSystem::rowsFrom(mlsd, true, now);
    QVERIFY2(parsed.ok(), qPrintable(parsed.error().message));

    QHash<QString, net::ListingRow> byName;
    for (const net::ListingRow& row : parsed.value())
        byName.insert(row.name, row);

    // The two rows that describe the folder and its parent come back as the dot
    // entries every caller already skips, whatever pathname the server put on
    // them.
    QVERIFY(byName.contains(QStringLiteral(".")));
    QVERIFY(byName.contains(QStringLiteral("..")));

    const net::ListingRow folder = byName.value(QStringLiteral("reports"));
    QVERIFY(folder.isDir);
    QCOMPARE(folder.permissions, QStringLiteral("rwxr-xr-x"));

    // A name with a space in it is one name: the split is at the first space
    // after the facts and nowhere else.
    const net::ListingRow file = byName.value(QStringLiteral("notes and drafts.txt"));
    QVERIFY2(file.valid, "a name with spaces in it is still one name");
    QVERIFY(!file.isDir);
    QCOMPARE(file.size, qint64(1024));
    // In UTC, which is the whole reason this format is worth asking for.
    QCOMPARE(file.modified.toUTC(), QDateTime(QDate(2024, 1, 23), QTime(10, 16), Qt::UTC));
}

/// A file name that is also a second command.
///
/// The quote commands carry names as they are, and a control channel is
/// line-based: `DELE a\r\nRMD /` is one perfectly legal file name to a
/// filesystem -- ADR-0070 is about taking the names one is given -- and two
/// commands to a server, the second of them being "remove the root". There is no
/// escape for it in the protocol, so it is refused. See MOLE-349.
void TestFtpFileSystem::aNameWithALineBreakCannotCarryASecondCommand()
{
    QVERIFY(FtpFileSystem::commandIsSendable("DELE /srv/ordinary name.txt"));
    QVERIFY2(!FtpFileSystem::commandIsSendable("DELE /srv/a\r\nRMD /"),
        "a name carrying CRLF would inject a second command");
    QVERIFY(!FtpFileSystem::commandIsSendable("DELE /srv/a\nRMD /"));
    QVERIFY(!FtpFileSystem::commandIsSendable("RNTO /srv/b\r"));
}

void TestFtpFileSystem::aWriteStreamsRatherThanStagingTheWholeFile()
{
    // The whole of MOLE-34 in one assertion, and it needs no server: a staged
    // write collects the entire payload into a local temporary file before
    // sending any of it, so a file larger than the local disk cannot be written
    // at all. Which class comes back is the difference, so that is what is held
    // -- the behaviour it stands for is what a real server proves, in the
    // conformance suite below and in the heavy tier's peak-scratch assertion.
    FtpFileSystemFactory factory;
    QString error;
    // Nothing listens on port 1, so the stat() openWrite() makes on the way past
    // is refused at once rather than waiting on a name that does not resolve.
    const QVariantMap config { { QStringLiteral("host"), QStringLiteral("127.0.0.1") },
        { QStringLiteral("port"), 1 }, { QStringLiteral("security"), QStringLiteral("none") } };
    const FileSystemPtr fs = factory.create(config, &error);
    QVERIFY2(fs != nullptr, qPrintable(error));

    Result<std::unique_ptr<QIODevice>> opened
        = fs->openWrite(VfsUri::fromString(QStringLiteral("ftp://server/big.iso")));
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));

    // Destroyed without being closed and without a byte written, so nothing is
    // ever sent and no thread is started -- the stream begins sending on the
    // first write.
    QVERIFY2(dynamic_cast<net::StreamingUpload*>(opened.value().get()) != nullptr,
        "an FTP write has to stream; staging is what made a file bigger than the disk unwritable");
    QVERIFY2(dynamic_cast<net::BufferedUpload*>(opened.value().get()) == nullptr,
        "and specifically not the staged kind");
}

void TestFtpFileSystem::aLargeReadStreamsRatherThanStagingTheWholeFile()
{
    // The mirror of the write-side assertion above, and the structural half of
    // MOLE-127: a staged read downloads the entire file into a local temporary
    // before handing any of it over, so a file larger than the scratch space
    // could not be read at all. Which class comes back is the difference.
    //
    // No server, and none needed: the size is handed in, so nothing has to be
    // asked of anybody, and a download stream fetches nothing until it is read.
    FtpFileSystemFactory factory;
    QString error;
    // Nothing listens on port 1, so anything that did reach for the network
    // would be refused at once rather than waiting on a name that does not
    // resolve. Nothing should.
    const QVariantMap config { { QStringLiteral("host"), QStringLiteral("127.0.0.1") },
        { QStringLiteral("port"), 1 }, { QStringLiteral("security"), QStringLiteral("none") } };
    const FileSystemPtr fs = factory.create(config, &error);
    QVERIFY2(fs != nullptr, qPrintable(error));

    Result<std::unique_ptr<QIODevice>> opened
        = fs->openRead(VfsUri::fromString(QStringLiteral("ftp://server/big.iso")), 8LL * 1024 * 1024 * 1024);
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));
    QVERIFY2(dynamic_cast<net::StreamingDownload*>(opened.value().get()) != nullptr,
        "an FTP read of a large file has to stream; staging is what made a file bigger than the "
        "scratch space unreadable");
}

void TestFtpFileSystem::rangedFetchDeliversExactlyTheSpanItAsksFor()
{
    // The measurement MOLE-127 was written around, kept as a test.
    //
    // A streamed read asks for one span at a time by setting both ends of
    // CURLOPT_RANGE. For FTP that becomes REST plus RETR, and REST has no end:
    // if the server ignores the end of the range, one span keeps delivering
    // until the file runs out -- past what the stream asked for -- and the next
    // span re-fetches bytes already handed over. A read that silently duplicates
    // a span is worse than one that needs scratch space, which is why this was
    // settled against a server rather than out of the documentation.
    //
    // Through plain libcurl rather than through the backend, deliberately: this
    // is a claim about what servers do, and the backend is what depends on it.
    // A server that stopped honouring the end of a range would break streamed
    // reads, and this is the line that would say so.
    MOLE_FTP_SERVER_OR_STOP();
    const Account account = accountFromEnvironment();

    const RawFtp raw(account);
    const QString base
        = account.base + QStringLiteral("/mole-ftp-range-%1").arg(QCoreApplication::applicationPid());
    raw.removeTree(base);
    const RawOutcome made = raw.command("MKD " + base.toUtf8(), account.base);
    QVERIFY2(made, qPrintable(QStringLiteral("could not make %1: %2").arg(base, made.reason())));

    // Position-dependent contents, so a span that came back from the wrong
    // offset is a different failure from one that came back the wrong length.
    QByteArray payload;
    payload.reserve(300000);
    for (int block = 0; block < 300; ++block)
        payload += QByteArray(1000, static_cast<char>(block % 251));
    const QString path = base + QStringLiteral("/ranged.bin");
    const RawOutcome sent = raw.putFile(path, payload);
    QVERIFY2(sent, qPrintable(QStringLiteral("could not upload %1: %2").arg(path, sent.reason())));

    // The whole file, as the control: whatever follows is measured against this.
    QCOMPARE(raw.getRange(path, 0, payload.size() - 1).size(), payload.size());

    // A hundred bytes from the front. The number that matters is the size: a
    // server honouring only REST would answer with the remaining 299 900.
    const QByteArray head = raw.getRange(path, 100, 199);
    QCOMPARE(head.size(), 100);
    QCOMPARE(head, QByteArray(100, static_cast<char>(0)));

    // A thousand from the middle, which is where a wrong offset shows.
    const QByteArray middle = raw.getRange(path, 150000, 150999);
    QCOMPARE(middle.size(), 1000);
    QCOMPARE(middle, QByteArray(1000, static_cast<char>(150 % 251)));

    // And a span whose end is the end of the file, which is what the last span
    // of every streamed read looks like.
    const QByteArray tail = raw.getRange(path, 299000, payload.size() - 1);
    QCOMPARE(tail.size(), 1000);
    QCOMPARE(tail, QByteArray(1000, static_cast<char>(299 % 251)));

    raw.removeTree(base);
}

void TestFtpFileSystem::aFileOverTheThresholdReadsBackByteForByte()
{
    // The behavioural half: a file over the threshold really is read through the
    // stream, and what comes back is what was put there. Larger than the
    // threshold rather than larger than the disk -- the claim is the same one and
    // the heavy tier is where sizes that need a disk of their own belong.
    MOLE_FTP_SERVER_OR_STOP();
    const Account account = accountFromEnvironment();

    const RawFtp raw(account);
    const QString base
        = account.base + QStringLiteral("/mole-ftp-big-%1").arg(QCoreApplication::applicationPid());
    raw.removeTree(base);
    const RawOutcome made = raw.command("MKD " + base.toUtf8(), account.base);
    QVERIFY2(made, qPrintable(QStringLiteral("could not make %1: %2").arg(base, made.reason())));

    FtpSettings settings;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = base;
    settings.security = account.requireTls ? FtpSettings::Security::Require : FtpSettings::Security::None;
    settings.verifyTls = !account.ignoreSelfSignedCert;
    auto fs = std::make_shared<FtpFileSystem>(QStringLiteral("ftp"), settings);

    // Just over the figure that decides between staging and streaming, so the
    // test costs one file of that size rather than a disk of them.
    const qint64 size = 64LL * 1024 * 1024 + 4096;
    QByteArray payload;
    payload.resize(size);
    for (qint64 i = 0; i < size; ++i)
        payload[i] = static_cast<char>((i * 31 + i / 4096) & 0xff);
    const RawOutcome sent = raw.putFile(base + QStringLiteral("/big.bin"), payload);
    QVERIFY2(sent, qPrintable(QStringLiteral("could not upload the fixture: %1").arg(sent.reason())));

    Result<std::unique_ptr<QIODevice>> opened
        = fs->openRead(VfsUri::fromString(QStringLiteral("ftp://server/big.bin")));
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));
    std::unique_ptr<QIODevice> device = std::move(opened.value());
    QVERIFY2(dynamic_cast<net::StreamingDownload*>(device.get()) != nullptr,
        "a file over the threshold has to come back as a stream, not as a staged copy");
    QCOMPARE(device->size(), size);

    // Read whole, in the ordinary way, and compared: a stream that dropped or
    // repeated a span would still be the right length and the wrong file.
    QByteArray readBack;
    readBack.reserve(size);
    while (readBack.size() < size) {
        const QByteArray chunk = device->read(4LL * 1024 * 1024);
        if (chunk.isEmpty())
            break;
        readBack += chunk;
    }
    QCOMPARE(readBack.size(), size);
    QVERIFY2(readBack == payload, "the bytes that came back are not the bytes that were put there");

    device.reset();
    raw.removeTree(base);
}

void TestFtpFileSystem::itSatisfiesTheConformanceSuite()
{
    MOLE_FTP_SERVER_OR_STOP();
    const Account account = accountFromEnvironment();

    const RawFtp raw(account);
    const QString base
        = account.base + QStringLiteral("/mole-ftp-%1").arg(QCoreApplication::applicationPid());

    raw.removeTree(base);
    const RawOutcome made = raw.command("MKD " + base.toUtf8(), account.base);
    QVERIFY2(made, qPrintable(QStringLiteral("could not make %1: %2").arg(base, made.reason())));

    FtpSettings settings;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = base;
    settings.security = account.requireTls ? FtpSettings::Security::Require : FtpSettings::Security::None;
    // The backend under test gets told the same thing the fixture was, or the
    // two would be talking to what looks like two different servers.
    settings.verifyTls = !account.ignoreSelfSignedCert;

    ConformanceContext context;
    context.fileSystem = std::make_shared<FtpFileSystem>(QStringLiteral("ftp"), settings);
    context.root = VfsUri(QStringLiteral("ftp"), QString(), QStringLiteral("/"));
    context.seedFile = [&raw, &base](const QString& relative, const QByteArray& contents) {
        return static_cast<bool>(raw.putFile(base + QLatin1Char('/') + relative, contents));
    };
    context.seedDir = [&raw, &base](const QString& relative) {
        const QString path = base + QLatin1Char('/') + relative;
        return static_cast<bool>(raw.command("MKD " + path.toUtf8(), RawFtp::parentOf(path)));
    };

    runFileSystemConformance(context);

    raw.removeTree(base);
}

MOLE_TEST_MAIN(TestFtpFileSystem)

#include "tst_FtpFileSystem.moc"
