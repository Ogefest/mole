#include "plugins/network/FtpFileSystem.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"

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

    bool command(const QByteArray& text, const QString& contextDir) const
    {
        CURL* handle = prepare();
        if (!handle)
            return false;
        const QByteArray url = urlFor(contextDir, true);
        curl_slist* commands = curl_slist_append(nullptr, text.constData());
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_QUOTE, commands);
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
        const CURLcode code = curl_easy_perform(handle);
        curl_slist_free_all(commands);
        curl_easy_cleanup(handle);
        return code == CURLE_OK;
    }

    bool putFile(const QString& path, const QByteArray& contents) const
    {
        CURL* handle = prepare();
        if (!handle)
            return false;
        Payload payload { contents, 0 };
        const QByteArray url = urlFor(path, false);
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(handle, CURLOPT_READFUNCTION, readPayload);
        curl_easy_setopt(handle, CURLOPT_READDATA, &payload);
        curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(contents.size()));
        const CURLcode code = curl_easy_perform(handle);
        curl_easy_cleanup(handle);
        return code == CURLE_OK;
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

class TestFtpFileSystem : public QObject
{
    Q_OBJECT

private slots:
    void aFormWithoutAHostIsRefused();
    void anEmptyUserBecomesAnonymous();
    void encryptionDefaultsToTryingTls();
    void theFormAsksOnlyWhatFtpNeeds();
    void itSatisfiesTheConformanceSuite();
};

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
}

void TestFtpFileSystem::itSatisfiesTheConformanceSuite()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No FTP account in the environment; set MOLE_TEST_FTP_HOST, MOLE_TEST_FTP_USER "
              "and MOLE_TEST_FTP_PASS to run this against a real server.");
    }

    const RawFtp raw(account);
    const QString base
        = account.base + QStringLiteral("/mole-ftp-%1").arg(QCoreApplication::applicationPid());

    raw.removeTree(base);
    QVERIFY2(raw.command("MKD " + base.toUtf8(), account.base),
        "could not create the working directory on the server");

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
        return raw.putFile(base + QLatin1Char('/') + relative, contents);
    };
    context.seedDir = [&raw, &base](const QString& relative) {
        const QString path = base + QLatin1Char('/') + relative;
        return raw.command("MKD " + path.toUtf8(), RawFtp::parentOf(path));
    };

    runFileSystemConformance(context);

    raw.removeTree(base);
}

MOLE_TEST_MAIN(TestFtpFileSystem)

#include "tst_FtpFileSystem.moc"
