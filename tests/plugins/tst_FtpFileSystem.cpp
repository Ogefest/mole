#include "plugins/network/FtpFileSystem.h"
#include "plugins/network/TransferStreams.h"
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
    void aWriteStreamsRatherThanStagingTheWholeFile();
    void aLargeReadStreamsRatherThanStagingTheWholeFile();
    void rangedFetchDeliversExactlyTheSpanItAsksFor();
    void aFileOverTheThresholdReadsBackByteForByte();
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
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No FTP account in the environment; set MOLE_TEST_FTP_HOST, MOLE_TEST_FTP_USER "
              "and MOLE_TEST_FTP_PASS to run this against a real server.");
    }

    const RawFtp raw(account);
    const QString base
        = account.base + QStringLiteral("/mole-ftp-range-%1").arg(QCoreApplication::applicationPid());
    raw.removeTree(base);
    QVERIFY2(raw.command("MKD " + base.toUtf8(), account.base),
        "could not create the working directory on the server");

    // Position-dependent contents, so a span that came back from the wrong
    // offset is a different failure from one that came back the wrong length.
    QByteArray payload;
    payload.reserve(300000);
    for (int block = 0; block < 300; ++block)
        payload += QByteArray(1000, static_cast<char>(block % 251));
    const QString path = base + QStringLiteral("/ranged.bin");
    QVERIFY(raw.putFile(path, payload));

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
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No FTP account in the environment; set MOLE_TEST_FTP_HOST, MOLE_TEST_FTP_USER "
              "and MOLE_TEST_FTP_PASS to run this against a real server.");
    }

    const RawFtp raw(account);
    const QString base
        = account.base + QStringLiteral("/mole-ftp-big-%1").arg(QCoreApplication::applicationPid());
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
    settings.verifyTls = !account.ignoreSelfSignedCert;
    auto fs = std::make_shared<FtpFileSystem>(QStringLiteral("ftp"), settings);

    // Just over the figure that decides between staging and streaming, so the
    // test costs one file of that size rather than a disk of them.
    const qint64 size = 64LL * 1024 * 1024 + 4096;
    QByteArray payload;
    payload.resize(size);
    for (qint64 i = 0; i < size; ++i)
        payload[i] = static_cast<char>((i * 31 + i / 4096) & 0xff);
    QVERIFY(raw.putFile(base + QStringLiteral("/big.bin"), payload));

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
