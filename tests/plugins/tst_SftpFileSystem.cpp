#include "plugins/network/SftpFileSystem.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"

#include <QUrl>

#include <algorithm>
#include <cstring>
#include <curl/curl.h>

using namespace mole;
using namespace mole::test;

namespace {

/// Where the live account comes from. Never a value in this file: the suite has
/// to stay green on a machine with no server to talk to, and a test account
/// committed to a repository is an account that has been given away.
struct Account
{
    QString host;
    int port = 22;
    QString user;
    QString password;
    /// A directory on the server the test may create things under.
    QString base;

    bool isConfigured() const { return !host.isEmpty() && !user.isEmpty(); }
};

Account accountFromEnvironment()
{
    const auto value = [](const char* name) { return QString::fromLocal8Bit(qgetenv(name)); };

    Account account;
    account.host = value("MOLE_TEST_SFTP_HOST");
    account.user = value("MOLE_TEST_SFTP_USER");
    account.password = value("MOLE_TEST_SFTP_PASS");
    account.base = value("MOLE_TEST_SFTP_BASE");
    if (account.base.isEmpty())
        account.base = QStringLiteral("/Shared");

    const int port = value("MOLE_TEST_SFTP_PORT").toInt();
    account.port = port > 0 ? port : 22;
    return account;
}

/// Seeds the server through plain libcurl rather than through SftpFileSystem.
///
/// The conformance contract asks for an out-of-band channel, and for a live
/// server this is the only one there is. It matters more here than for a local
/// backend: if the backend both created and read the fixture, a listing bug that
/// mirrored a writing bug would cancel out and the suite would pass.
class RawSftp
{
public:
    explicit RawSftp(const Account& account)
        : m_account(account)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    QByteArray urlFor(const QString& absolutePath, bool asDirectory) const
    {
        QByteArray url = "sftp://" + m_account.host.toUtf8() + ':' + QByteArray::number(m_account.port);
        url += QUrl::toPercentEncoding(absolutePath, "/");
        if (asDirectory && !url.endsWith('/'))
            url += '/';
        return url;
    }

    bool command(const QString& text, const QString& contextDir) const
    {
        CURL* handle = prepare();
        if (!handle)
            return false;
        const QByteArray url = urlFor(contextDir, true);
        curl_slist* commands = curl_slist_append(nullptr, text.toUtf8().constData());
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_QUOTE, commands);
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
        const CURLcode code = curl_easy_perform(handle);
        curl_slist_free_all(commands);
        curl_easy_cleanup(handle);
        return code == CURLE_OK;
    }

    bool putFile(const QString& absolutePath, const QByteArray& contents) const
    {
        CURL* handle = prepare();
        if (!handle)
            return false;
        const QByteArray url = urlFor(absolutePath, false);
        Payload payload { contents, 0 };
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(handle, CURLOPT_READFUNCTION, readPayload);
        curl_easy_setopt(handle, CURLOPT_READDATA, &payload);
        curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(contents.size()));
        const CURLcode code = curl_easy_perform(handle);
        curl_easy_cleanup(handle);
        return code == CURLE_OK;
    }

    /// Deletes a tree, so a failed run does not leave litter on a real server.
    void removeTree(const QString& absolutePath) const
    {
        CURL* handle = prepare();
        if (!handle)
            return;
        QByteArray listing;
        const QByteArray url = urlFor(absolutePath, true);
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &listing);
        const bool listed = curl_easy_perform(handle) == CURLE_OK;
        curl_easy_cleanup(handle);
        if (!listed)
            return;

        const QStringList lines = QString::fromUtf8(listing).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const int space = line.lastIndexOf(QLatin1Char(' '));
            if (space < 0)
                continue;
            const QString name = line.mid(space + 1);
            if (name == QLatin1String(".") || name == QLatin1String(".."))
                continue;
            const QString child = absolutePath + QLatin1Char('/') + name;
            if (line.startsWith(QLatin1Char('d')))
                removeTree(child);
            else
                command(QStringLiteral("rm \"%1\"").arg(child), absolutePath);
        }
        command(QStringLiteral("rmdir \"%1\"").arg(absolutePath), parentOf(absolutePath));
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
        return handle;
    }

    Account m_account;
};

} // namespace

class TestSftpFileSystem : public QObject
{
    Q_OBJECT

private slots:
    void aFormWithoutAHostIsRefused();
    void aFormWithoutCredentialsIsRefused();
    void theSchemeAndRootComeFromTheDrive();
    void theFormAsksOnlyWhatSftpNeeds();
    void itSatisfiesTheConformanceSuite();
};

void TestSftpFileSystem::aFormWithoutAHostIsRefused()
{
    SftpFileSystemFactory factory;
    QString error;
    const QVariantMap config { { QStringLiteral("user"), QStringLiteral("someone") },
        { QStringLiteral("password"), QStringLiteral("secret") } };

    QVERIFY(factory.create(config, &error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("host")), qPrintable(error));
}

void TestSftpFileSystem::aFormWithoutCredentialsIsRefused()
{
    SftpFileSystemFactory factory;
    QString error;
    const QVariantMap config { { QStringLiteral("host"), QStringLiteral("nas.local") },
        { QStringLiteral("user"), QStringLiteral("someone") } };

    // Neither a password nor a key means there is no way to log in, and saying so
    // now beats a connection that fails later for reasons nobody can see.
    QVERIFY(factory.create(config, &error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("password")), qPrintable(error));
}

void TestSftpFileSystem::theSchemeAndRootComeFromTheDrive()
{
    const QVariantMap config { { QStringLiteral("host"), QStringLiteral("nas.local") },
        { QStringLiteral("user"), QStringLiteral("someone") },
        { QStringLiteral("password"), QStringLiteral("secret") }, { QStringLiteral("port"), 2222 },
        { QStringLiteral("__root"), QStringLiteral("volume1/photos") } };

    const SftpSettings settings = SftpFileSystemFactory::settingsFrom(config);
    QCOMPARE(settings.port, 2222);
    // A root given without a leading slash is still an absolute path on the
    // server; the alternative is a drive that silently points somewhere else.
    QCOMPARE(settings.remoteRoot, QStringLiteral("/volume1/photos"));

    QString error;
    SftpFileSystemFactory factory;
    QVariantMap withScheme = config;
    withScheme.insert(QStringLiteral("__scheme"), QStringLiteral("photos"));
    const FileSystemPtr fs = factory.create(withScheme, &error);
    QVERIFY2(fs != nullptr, qPrintable(error));
    QCOMPARE(fs->scheme(), QStringLiteral("photos"));
}

void TestSftpFileSystem::theFormAsksOnlyWhatSftpNeeds()
{
    const SftpFileSystemFactory factory;
    const QList<ConnectionField> fields = factory.connectionFields();

    // The point of dropping rclone was that a generated form asked eighty
    // questions. This one is short by construction, and the test says so out
    // loud so nobody quietly grows it back.
    QVERIFY2(fields.size() <= 8, "the SFTP form should stay short enough to fill in");

    int required = 0;
    bool hasPasswordField = false;
    for (const ConnectionField& field : fields) {
        if (field.required && !field.advanced)
            ++required;
        if (field.kind == ConnectionField::Password)
            hasPasswordField = true;
    }
    QCOMPARE(required, 2); // host and user, and nothing else
    QVERIFY2(hasPasswordField, "a password field is what routes the secret to the credential store");
}

void TestSftpFileSystem::itSatisfiesTheConformanceSuite()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No SFTP account in the environment; set MOLE_TEST_SFTP_HOST, "
              "MOLE_TEST_SFTP_USER and MOLE_TEST_SFTP_PASS to run this against a real server.");
    }

    const RawSftp raw(account);
    const QString base
        = account.base + QStringLiteral("/mole-conformance-%1").arg(QCoreApplication::applicationPid());

    // A leftover from an interrupted run would fail the "root must start out
    // empty" check with something that looks like a backend fault.
    raw.removeTree(base);
    QVERIFY2(raw.command(QStringLiteral("mkdir \"%1\"").arg(base), account.base),
        "could not create the working directory on the server");

    SftpSettings settings;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = base;

    ConformanceContext context;
    context.fileSystem = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settings);
    context.root = VfsUri(QStringLiteral("sftp"), QString(), QStringLiteral("/"));
    context.seedFile = [&raw, &base](const QString& relative, const QByteArray& contents) {
        return raw.putFile(base + QLatin1Char('/') + relative, contents);
    };
    context.seedDir = [&raw, &base](const QString& relative) {
        const QString path = base + QLatin1Char('/') + relative;
        return raw.command(QStringLiteral("mkdir \"%1\"").arg(path), RawSftp::parentOf(path));
    };

    runFileSystemConformance(context);

    raw.removeTree(base);
}

MOLE_TEST_MAIN(TestSftpFileSystem)

#include "tst_SftpFileSystem.moc"
