#include "plugins/network/SftpFileSystem.h"
#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/TestbedControl.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
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
    /// The other supported way in, which the live suite never exercised: a
    /// private key and no password at all. Key-only is what most hardened
    /// servers offer, and it is the configuration MOLE-374 broke.
    QString privateKeyPath;
    QString privateKeyPassphrase;
    /// A directory on the server the test may create things under.
    QString base;

    bool isConfigured() const { return !host.isEmpty() && !user.isEmpty(); }
    bool hasKey() const { return isConfigured() && !privateKeyPath.isEmpty(); }
};

Account accountFromEnvironment()
{
    const auto value = [](const char* name) { return QString::fromLocal8Bit(qgetenv(name)); };

    Account account;
    account.host = value("MOLE_TEST_SFTP_HOST");
    account.user = value("MOLE_TEST_SFTP_USER");
    account.password = value("MOLE_TEST_SFTP_PASS");
    account.privateKeyPath = value("MOLE_TEST_SFTP_KEY");
    account.privateKeyPassphrase = value("MOLE_TEST_SFTP_KEY_PASS");
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

    /// One long-format line per entry, exactly as the server gave them.
    QStringList linesIn(const QString& absolutePath) const
    {
        CURL* handle = prepare();
        if (!handle)
            return {};
        QByteArray listing;
        const QByteArray url = urlFor(absolutePath, true);
        curl_easy_setopt(handle, CURLOPT_URL, url.constData());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &listing);
        const bool listed = curl_easy_perform(handle) == CURLE_OK;
        curl_easy_cleanup(handle);
        if (!listed)
            return {};
        return QString::fromUtf8(listing).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    /// What the server says is in a directory. Asked out of band on purpose: the
    /// question is what the backend left behind, and the backend is not a
    /// trustworthy witness to that.
    QStringList namesIn(const QString& absolutePath) const
    {
        QStringList names;
        const QStringList lines = linesIn(absolutePath);
        for (const QString& line : lines) {
            const int space = line.lastIndexOf(QLatin1Char(' '));
            if (space < 0)
                continue;
            const QString name = line.mid(space + 1);
            if (name != QLatin1String(".") && name != QLatin1String(".."))
                names.append(name);
        }
        return names;
    }

    /// Deletes a tree, so a failed run does not leave litter on a real server.
    void removeTree(const QString& absolutePath) const
    {
        const QStringList lines = linesIn(absolutePath);
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

/// A megabyte of content that depends on where it sits in the file.
///
/// A file of repeated zeroes would prove only that the right number of bytes
/// arrived; content that differs block by block also proves they arrived in the
/// right order and none was skipped over.
constexpr int kBlockSize = 1024 * 1024;

QByteArray blockAt(int index)
{
    QByteArray block(kBlockSize, Qt::Uninitialized);
    for (int i = 0; i < kBlockSize; ++i)
        block[i] = static_cast<char>((i * 31 + index * 17) & 0xff);
    return block;
}

/// A server that answers listings out of a script instead of over a network.
///
/// It exists because SFTP servers disagree about what "list this file" means.
/// One does not refuse: it answers with a "." row describing the file itself.
/// Another refuses and says the path does not exist -- which is true of the
/// directory that was asked for and false of the file that is sitting there. The
/// backend has to turn both into the same answer, and neither can be arranged on
/// a server that is behaving, so the answer is supplied here instead.
class ServerThatAnswers final : public SftpFileSystem
{
public:
    ServerThatAnswers()
        : SftpFileSystem(QStringLiteral("sftp"), SftpSettings {})
    {
    }

    /// What the server sends back when this directory is listed.
    void answers(const QString& path, const QByteArray& listing) { m_listings.insert(path, listing); }
    /// How the server refuses when this path is listed.
    void refuses(const QString& path, CURLcode code) { m_refusals.insert(path, code); }

    /// One `ls -l` row in the shape libcurl produces for SFTP.
    static QByteArray row(const QString& name, bool isDir, int size)
    {
        return QStringLiteral("%1rw-r--r--   1 -        -   %2 Aug  9 08:54 %3\n")
            .arg(isDir ? QLatin1Char('d') : QLatin1Char('-'))
            .arg(size, 13)
            .arg(name)
            .toUtf8();
    }

protected:
    net::Response fetchListing(const VfsUri& dir, const CancelToken&) override
    {
        const QString path = dir.path().isEmpty() ? QStringLiteral("/") : dir.path();

        net::Response response;
        if (m_refusals.contains(path)) {
            response.code = m_refusals.value(path);
            response.detail = QStringLiteral("the server refused");
            return response;
        }
        response.body = m_listings.value(path);
        return response;
    }

private:
    QHash<QString, QByteArray> m_listings;
    QHash<QString, CURLcode> m_refusals;
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
    void aPrivateKeyIsPartOfEveryLeaseAndNotOneOfThem();
    void aKeyPathWrittenWithATildeBecomesAnAbsoluteOne();
    void nothingOutsideThePoolSetsAnSshCredential();

    void aServerThatDescribesTheFileItselfIsUnderstoodToMeanAFile();
    void aServerThatRefusesToListAFileIsUnderstoodToMeanAFile();
    void aServerThatRefusesForAnotherReasonIsStillUnderstood();
    void aPathThatIsNotThereIsNotCalledAFile();
    void aDirectoryStillListsItsChildrenAndNotItsDotRows();

    void itSatisfiesTheConformanceSuite();
    void aLargeFileArrivesWhole();
    void aLargeFileGoesUpWhole();
    void aReadWhoseConnectionIsCutDoesNotLookLikeAWholeFile();
    void aKilledUploadLeavesNothingThatLooksFinished();
    void aHostKeyThatChangedIsRefused();
    void aKeyOnlyDriveDoesEverythingAndNotOnlyBrowsing();
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

/// A key-only drive browsed and could do nothing else.
///
/// `fetchListing()` set CURLOPT_SSH_PRIVATE_KEYFILE and CURLOPT_KEYPASSWD on its
/// own lease, and nothing else did. `CurlPool::prepare()` calls
/// `curl_easy_reset()` on every take() and takeFresh() hands out a handle nobody
/// has touched, so nothing set on one lease survives into the next: the listing
/// authenticated with the key and every read, write, mkdir, rm and rename fell
/// back to the password that was not there. CURLE_LOGIN_DENIED became
/// AccessDenied, "the server refused the credentials", which points at exactly
/// the wrong problem. See MOLE-374.
void TestSftpFileSystem::aPrivateKeyIsPartOfEveryLeaseAndNotOneOfThem()
{
    SftpSettings settings;
    settings.host = QStringLiteral("example");
    settings.username = QStringLiteral("someone");
    settings.privateKeyPath = QStringLiteral("/keys/id_ed25519");
    settings.privateKeyPassphrase = QStringLiteral("open sesame");

    // The options the pool prepares *every* handle with, which is the only place
    // an identity can live once a lease cannot carry one.
    const net::TransportOptions options = transportOptionsFor(settings);
    QCOMPARE(options.privateKeyPath, settings.privateKeyPath);
    QCOMPARE(options.privateKeyPassphrase, settings.privateKeyPassphrase);
}

void TestSftpFileSystem::aKeyPathWrittenWithATildeBecomesAnAbsoluteOne()
{
    // libcurl does not expand a tilde, and the form's own help suggests
    // ~/.ssh/id_ed25519 -- so the spelling the interface recommends was handed
    // to the server verbatim and came back as a refused credential.
    QVariantMap config;
    config.insert(QStringLiteral("host"), QStringLiteral("example"));
    config.insert(QStringLiteral("user"), QStringLiteral("someone"));
    config.insert(QStringLiteral("privateKey"), QStringLiteral("~/.ssh/id_ed25519"));

    const SftpSettings settings = SftpFileSystemFactory::settingsFrom(config);
    QCOMPARE(settings.privateKeyPath, QDir::homePath() + QStringLiteral("/.ssh/id_ed25519"));
    QVERIFY(!settings.privateKeyPath.contains(QLatin1Char('~')));

    // A path that is already absolute is left exactly as it was.
    config.insert(QStringLiteral("privateKey"), QStringLiteral("/etc/keys/deploy~1"));
    QCOMPARE(
        SftpFileSystemFactory::settingsFrom(config).privateKeyPath, QStringLiteral("/etc/keys/deploy~1"));
}

void TestSftpFileSystem::nothingOutsideThePoolSetsAnSshCredential()
{
    // The fault was one lease knowing something the other four did not, so the
    // rule that stops it coming back is about *where* a credential may be set,
    // and that is cheaper to state here than to catch in review. Every backend
    // in this directory takes its identity from the options the pool prepares
    // with; a curl_easy_setopt naming a key anywhere else is a fifth lease
    // waiting to be forgotten.
    const QDir directory(QStringLiteral(MOLE_SHELL_SOURCE_DIR) + QStringLiteral("/plugins/network"));
    const QStringList sources = directory.entryList({ QStringLiteral("*.cpp") }, QDir::Files, QDir::Name);
    QVERIFY(!sources.isEmpty());

    QStringList offenders;
    for (const QString& name : sources) {
        if (name == QLatin1String("CurlTransport.cpp"))
            continue;
        QFile source(directory.filePath(name));
        QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(source.fileName()));
        const QString text = QString::fromUtf8(source.readAll());
        for (const QString& option :
            { QStringLiteral("CURLOPT_SSH_PRIVATE_KEYFILE"), QStringLiteral("CURLOPT_KEYPASSWD"),
                QStringLiteral("CURLOPT_USERNAME"), QStringLiteral("CURLOPT_PASSWORD") }) {
            if (text.contains(option))
                offenders.append(QStringLiteral("%1 sets %2").arg(name, option));
        }
    }
    QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QStringLiteral("; "))));
}

/// Asked to list a file, a server that answers with a "." row describing the
/// file is describing a file, and saying so is the whole of the answer.
///
/// Dropping the dot rows before looking at them -- which is the obvious thing to
/// do with them -- turns "this is a file" into "this is an empty directory", and
/// an empty directory is what the user then gets shown in place of the file they
/// double-clicked.
void TestSftpFileSystem::aServerThatDescribesTheFileItselfIsUnderstoodToMeanAFile()
{
    ServerThatAnswers server;
    server.answers(QStringLiteral("/report.txt"), ServerThatAnswers::row(QStringLiteral("."), false, 12));

    const Result<FileEntryList> listed
        = server.list(VfsUri::fromString(QStringLiteral("sftp:///report.txt")), CancelToken());
    QVERIFY(!listed.ok());
    QCOMPARE(listed.error().code, VfsError::NotADirectory);
}

/// The other kind of server, which refuses instead -- and says the path does not
/// exist, because the directory it was asked for does not.
///
/// Passing that on would be a lie about the file, which is there. The backend
/// asks what the path actually is before it explains a failure, and the layers
/// above must not have to know which kind of server they are talking to.
void TestSftpFileSystem::aServerThatRefusesToListAFileIsUnderstoodToMeanAFile()
{
    ServerThatAnswers server;
    server.refuses(QStringLiteral("/report.txt"), CURLE_REMOTE_FILE_NOT_FOUND);
    server.answers(QStringLiteral("/"),
        ServerThatAnswers::row(QStringLiteral("."), true, 0)
            + ServerThatAnswers::row(QStringLiteral("report.txt"), false, 12)
            + ServerThatAnswers::row(QStringLiteral("photos"), true, 0));

    const Result<FileEntryList> listed
        = server.list(VfsUri::fromString(QStringLiteral("sftp:///report.txt")), CancelToken());
    QVERIFY(!listed.ok());
    QCOMPARE(listed.error().code, VfsError::NotADirectory);
}

void TestSftpFileSystem::aServerThatRefusesForAnotherReasonIsStillUnderstood()
{
    // A refusal that says nothing useful at all is the common case: the protocol
    // reports that the operation did not work and leaves the reason to be
    // guessed. The path itself is what settles it.
    ServerThatAnswers server;
    server.refuses(QStringLiteral("/report.txt"), CURLE_QUOTE_ERROR);
    server.answers(QStringLiteral("/"), ServerThatAnswers::row(QStringLiteral("report.txt"), false, 12));

    const Result<FileEntryList> listed
        = server.list(VfsUri::fromString(QStringLiteral("sftp:///report.txt")), CancelToken());
    QVERIFY(!listed.ok());
    QCOMPARE(listed.error().code, VfsError::NotADirectory);
}

void TestSftpFileSystem::aPathThatIsNotThereIsNotCalledAFile()
{
    // The other half of the same rule, and the one that keeps it honest: a path
    // that really is missing has to stay missing. An explanation that turns
    // every failed listing into "that is a file" would send whoever asked
    // looking for something that was never there.
    ServerThatAnswers server;
    server.refuses(QStringLiteral("/gone.txt"), CURLE_REMOTE_FILE_NOT_FOUND);
    server.answers(QStringLiteral("/"), ServerThatAnswers::row(QStringLiteral("report.txt"), false, 12));

    const Result<FileEntryList> listed
        = server.list(VfsUri::fromString(QStringLiteral("sftp:///gone.txt")), CancelToken());
    QVERIFY(!listed.ok());
    QCOMPARE(listed.error().code, VfsError::NotFound);
}

void TestSftpFileSystem::aDirectoryStillListsItsChildrenAndNotItsDotRows()
{
    ServerThatAnswers server;
    server.answers(QStringLiteral("/photos"),
        ServerThatAnswers::row(QStringLiteral("."), true, 0)
            + ServerThatAnswers::row(QStringLiteral(".."), true, 0)
            + ServerThatAnswers::row(QStringLiteral("beach.jpg"), false, 4096)
            + ServerThatAnswers::row(QStringLiteral("raw"), true, 0));

    const Result<FileEntryList> listed
        = server.list(VfsUri::fromString(QStringLiteral("sftp:///photos")), CancelToken());
    QVERIFY2(listed.ok(), qPrintable(listed.error().message));
    QCOMPARE(listed.value().size(), 2);
    QCOMPARE(listed.value().at(0).name, QStringLiteral("beach.jpg"));
    QCOMPARE(listed.value().at(0).size, 4096);
    QVERIFY(!listed.value().at(0).isDir);
    QCOMPARE(listed.value().at(1).name, QStringLiteral("raw"));
    QVERIFY(listed.value().at(1).isDir);
}

/// The case the control channel exists for.
///
/// A server that behaves proves very little. What a file manager has to survive
/// is the connection going away in the middle of a transfer -- and the failure
/// that matters is not the error, it is the *silence*: bytes stopping early and
/// being handed over as though the file were complete. That has happened here
/// before, which is why a copy is weighed at the destination now.
///
/// The cut is triggered on a byte offset rather than after a wait. A test that
/// sleeps for 200 ms passes on one machine and fails on another.
void TestSftpFileSystem::aReadWhoseConnectionIsCutDoesNotLookLikeAWholeFile()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured())
        QSKIP("No SFTP account in the environment.");
    if (!TestbedControl::isAvailable()) {
        QSKIP("No control channel; set MOLE_TEST_CONTROL to a command that can interfere with the "
              "server, e.g. \"ssh user@machine sudo mole-control\".");
    }

    int megabytes = qEnvironmentVariableIntValue("MOLE_TEST_SFTP_LARGE_MB");
    if (megabytes <= 0)
        megabytes = 64;

    SftpSettings settings;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = QStringLiteral("/");

    auto fileSystem = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settings);
    const RawSftp raw(account);

    const QString remotePath
        = account.base + QStringLiteral("/mole-cut-%1.bin").arg(QCoreApplication::applicationPid());
    QByteArray payload;
    payload.reserve(megabytes * kBlockSize);
    for (int block = 0; block < megabytes; ++block)
        payload.append(blockAt(block));
    QVERIFY2(raw.putFile(remotePath, payload), "could not put a large file on the server");

    // Slowed down first, with the same channel. Over a local network the file
    // arrives before a command to cut the connection has finished travelling,
    // so without this the test can only ever report that it was too late --
    // which is the shape of a test that never actually runs.
    const QString limited
        = TestbedControl::run({ QStringLiteral("netem"), QStringLiteral("rate"), QStringLiteral("40mbit") });
    QVERIFY2(!limited.isEmpty(), "the control channel has to say what it did");

    const VfsUri target(QStringLiteral("sftp"), QString(), remotePath);
    Result<std::unique_ptr<QIODevice>> opened = fileSystem->openRead(target, payload.size());
    QVERIFY2(opened.ok(), qPrintable(opened.error().message));
    std::unique_ptr<QIODevice> stream = std::move(opened.value());

    const qint64 cutAfter = payload.size() / 3;
    qint64 read = 0;
    bool cut = false;
    QString whatWasDone;

    while (read < payload.size()) {
        const QByteArray chunk = stream->read(256 * 1024);
        if (chunk.isEmpty())
            break;
        read += chunk.size();

        // Triggered on the offset itself, not on a clock.
        if (!cut && read >= cutAfter) {
            cut = true;
            // The network, not the socket. Killing the connection with ss
            // leaves whatever curl has already buffered to be read out, so a
            // transfer can finish after the socket is gone -- which is a real
            // thing to know and the wrong thing to build this on. Total loss
            // stops bytes arriving, full stop.
            whatWasDone = TestbedControl::run(
                { QStringLiteral("netem"), QStringLiteral("loss"), QStringLiteral("100%") });
            QVERIFY2(!whatWasDone.isEmpty(), "the control channel has to say what it did");
        }
    }

    // Put back before anything is asserted, so a failing assertion cannot leave
    // the machine throttled for whatever runs next.
    TestbedControl::restore();
    QVERIFY2(cut, "the file was read before anything could be done to the connection");
    raw.removeTree(remotePath);

    // The claim, and it holds either way round rather than skipping when the
    // race goes the other way. A test that reports "nothing to assert" is a
    // test that does not run, and `make test-live` is right to treat a skip as
    // a result rather than as a pass.
    //
    // Everything arrived: then it has to be the right bytes. A cut that lands
    // after the last one is a fair outcome, but "we got it all" is only worth
    // anything if what we got is what was sent.
    if (read == payload.size()) {
        stream.reset();
        const Result<std::unique_ptr<QIODevice>> again = fileSystem->openRead(target, payload.size());
        QVERIFY2(again.ok(), qPrintable(again.error().message));
        QCOMPARE(again.value()->readAll().size(), payload.size());
        return;
    }

    // Or it stopped short -- and then the stream has to say so. Short and
    // silent is the one outcome that must never happen, because it is
    // indistinguishable from a file that was simply that size.
    QVERIFY2(read < payload.size(), "sanity: this branch is the short read");
    QVERIFY2(stream->atEnd() == false || !stream->errorString().isEmpty(),
        qPrintable(QStringLiteral("stopped at %1 of %2 bytes and reported nothing. %3")
                       .arg(read)
                       .arg(payload.size())
                       .arg(whatWasDone)));
}

void TestSftpFileSystem::aLargeFileArrivesWhole()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No SFTP account in the environment; set MOLE_TEST_SFTP_HOST, "
              "MOLE_TEST_SFTP_USER and MOLE_TEST_SFTP_PASS to run this against a real server.");
    }

    // Reported from a real drive: copying from an SFTP server to local disk left
    // files that were only part of what was on the server, and nothing said so.
    // The conformance suite works in kilobytes, where a read never has time to be
    // interrupted; this one is large enough that the transfer takes seconds and
    // spans many SFTP reads, which is the only place the fault appears.
    int megabytes = qEnvironmentVariableIntValue("MOLE_TEST_SFTP_LARGE_MB");
    if (megabytes <= 0)
        megabytes = 64;

    // A file already on the server, when one is named -- the closest thing to
    // the case as reported, and it writes nothing.
    const QString existing = QString::fromLocal8Bit(qgetenv("MOLE_TEST_SFTP_LARGE_PATH"));

    SftpSettings settings;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = QStringLiteral("/");

    auto fileSystem = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settings);
    const RawSftp raw(account);

    QString remotePath = existing;
    const QString scratch
        = account.base + QStringLiteral("/mole-large-%1.bin").arg(QCoreApplication::applicationPid());
    QByteArray payload;

    if (remotePath.isEmpty()) {
        payload.reserve(megabytes * kBlockSize);
        for (int block = 0; block < megabytes; ++block)
            payload.append(blockAt(block));
        QVERIFY2(raw.putFile(scratch, payload), "could not put a large file on the server");
        remotePath = scratch;
    }

    const VfsUri target = VfsUri(QStringLiteral("sftp"), QString(), remotePath);

    const Result<FileEntry> what = fileSystem->stat(target);
    QVERIFY2(what.ok(), qPrintable(what.error().message));
    const qint64 announced = what.value().size;
    QVERIFY2(announced > 0, "the server reported an empty file, so there is nothing to prove here");

    const Result<std::unique_ptr<QIODevice>> stream = fileSystem->openRead(target);
    QVERIFY2(stream.ok(), qPrintable(stream.error().message));

    qint64 read = 0;
    int block = 0;
    bool contentsMatch = true;
    while (!stream.value()->atEnd()) {
        const QByteArray chunk = stream.value()->read(kBlockSize);
        if (chunk.isEmpty())
            break;
        if (!payload.isEmpty() && chunk.size() == kBlockSize && chunk != blockAt(block))
            contentsMatch = false;
        read += chunk.size();
        ++block;
    }

    if (!scratch.isEmpty() && existing.isEmpty())
        raw.command(QStringLiteral("rm \"%1\"").arg(scratch), RawSftp::parentOf(scratch));

    // The whole point: as many bytes as the server said the file has. A read
    // that stops early has to be an error, never a short file handed over as if
    // it were the real one.
    QCOMPARE(read, announced);
    QVERIFY2(contentsMatch, "the bytes that arrived are not the bytes that were sent");
}

void TestSftpFileSystem::aLargeFileGoesUpWhole()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured()) {
        QSKIP("No SFTP account in the environment; set MOLE_TEST_SFTP_HOST, "
              "MOLE_TEST_SFTP_USER and MOLE_TEST_SFTP_PASS to run this against a real server.");
    }

    // The other direction, and the one that decides whether a backup can be put
    // somewhere rather than only fetched. Large enough to pass the point where a
    // long transfer stops -- and written block by block, never held whole, which
    // is the property being tested: a file bigger than this machine's memory or
    // its free disk has to be sendable all the same.
    int megabytes = qEnvironmentVariableIntValue("MOLE_TEST_SFTP_LARGE_MB");
    if (megabytes <= 0)
        megabytes = 1500;

    SftpSettings settings;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = account.base;

    auto fileSystem = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settings);
    const QString name = QStringLiteral("/mole-upload-%1.bin").arg(QCoreApplication::applicationPid());
    const VfsUri target = VfsUri(QStringLiteral("sftp"), QString(), name);

    Result<std::unique_ptr<QIODevice>> stream = fileSystem->openWrite(target);
    QVERIFY2(stream.ok(), qPrintable(stream.error().message));

    bool wroteEverything = true;
    for (int block = 0; block < megabytes && wroteEverything; ++block) {
        const QByteArray content = blockAt(block);
        wroteEverything = stream.value()->write(content) == content.size();
    }
    const Result<void> committed = closeAndReport(*stream.value());

    const RawSftp raw(account);
    const auto cleanUp = [&raw, &account, &name] {
        raw.command(QStringLiteral("rm \"%1\"").arg(account.base + name), account.base);
    };

    if (!wroteEverything || !committed.ok()) {
        cleanUp();
        QVERIFY2(wroteEverything, "the stream stopped accepting bytes part way through");
        QVERIFY2(committed.ok(), qPrintable(committed.error().message));
    }

    // The server's own account of what it now holds, which is the only opinion
    // that counts about an upload.
    const Result<FileEntry> what = fileSystem->stat(target);
    if (!what.ok())
        cleanUp();
    QVERIFY2(what.ok(), qPrintable(what.error().message));
    const qint64 announced = what.value().size;

    const Result<std::unique_ptr<QIODevice>> back = fileSystem->openRead(target, announced);
    bool contentsMatch = back.ok();
    qint64 read = 0;
    if (back.ok()) {
        for (int block = 0; block < megabytes; ++block) {
            const QByteArray chunk = back.value()->read(kBlockSize);
            read += chunk.size();
            if (chunk != blockAt(block)) {
                contentsMatch = false;
                break;
            }
        }
    }

    cleanUp();

    QCOMPARE(announced, static_cast<qint64>(megabytes) * kBlockSize);
    QCOMPARE(read, static_cast<qint64>(megabytes) * kBlockSize);
    QVERIFY2(contentsMatch, "what came back is not what was sent");
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

/// What a process killed outright leaves on the server.
///
/// A transfer that *fails* is tidied up: the backend deletes what it wrote. A
/// process that is killed does not get to tidy up anything -- whatever is on the
/// server at that instant stays there, under whatever name it was going under.
/// So the only protection that survives a SIGKILL is the name the bytes were
/// travelling under all along, and the only honest way to test that is to kill a
/// real process in the middle of a real upload.
///
/// The victim is this same binary, re-run with one test function and an
/// environment variable naming the file to write. It writes until it is stopped,
/// which is what makes the kill land mid-flight rather than after the fact.
void TestSftpFileSystem::aKilledUploadLeavesNothingThatLooksFinished()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured())
        QSKIP("No SFTP account in the environment.");

    SftpSettings settings;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = QStringLiteral("/");

    const QString victimTarget = qEnvironmentVariable("MOLE_TEST_KILL_TARGET");
    if (!victimTarget.isEmpty()) {
        // The victim. Nothing here is asserted: this process exists to be killed,
        // and everything worth checking is checked by the one doing the killing.
        auto fileSystem = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settings);
        const VfsUri target(QStringLiteral("sftp"), QString(), victimTarget);
        Result<std::unique_ptr<QIODevice>> opened = fileSystem->openWrite(target, -1);
        if (!opened.ok())
            return;

        // A ceiling rather than a duration, and a backstop rather than the
        // mechanism: the parent kills this long before it gets here, and this
        // only bounds what a parent that died itself could leave running.
        const QByteArray block = blockAt(0);
        for (int written = 0; written < 1024; ++written) {
            if (opened.value()->write(block) != block.size())
                break;
        }
        return;
    }

    const QString directory = account.base;
    const QString name = QStringLiteral("mole-killed-%1.bin").arg(QCoreApplication::applicationPid());
    const QString remotePath = directory + QLatin1Char('/') + name;
    const QString partialName = name + QStringLiteral(".mole-partial");

    const RawSftp raw(account);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("MOLE_TEST_KILL_TARGET"), remotePath);

    QProcess victim;
    victim.setProcessEnvironment(environment);
    victim.setProcessChannelMode(QProcess::MergedChannels);
    victim.start(QCoreApplication::applicationFilePath(),
        { QStringLiteral("aKilledUploadLeavesNothingThatLooksFinished") });
    QVERIFY2(victim.waitForStarted(10000), "could not start a second copy of this test binary");

    // Killed on the file appearing, not after a wait. Until the server has
    // something on disk there is nothing to interrupt, and a fixed sleep would
    // be either too short on a slow link or wasted on a fast one.
    //
    // Under *either* name, deliberately. Waiting only for the working name
    // would turn the fault into "nothing ever reached the server", which is
    // both untrue and the least useful thing this could report -- the whole
    // question is which name the bytes are travelling under, so both are worth
    // waiting for and the answer is what gets asserted.
    bool appeared = false;
    for (int attempt = 0; attempt < 400 && !appeared; ++attempt) {
        if (victim.state() != QProcess::Running)
            break;
        const QStringList names = raw.namesIn(directory);
        appeared = names.contains(partialName) || names.contains(name);
        if (!appeared)
            QTest::qWait(50);
    }

    const QString transcript = QString::fromLocal8Bit(victim.readAll());
    victim.kill(); // SIGKILL: no destructors, no cleanup, no second chance
    victim.waitForFinished(15000);

    const QStringList after = raw.namesIn(directory);
    // Tidied up before anything is asserted, so a failure cannot leave a
    // multi-gigabyte carcass on the server for whatever runs next.
    raw.command(QStringLiteral("rm \"%1\"").arg(remotePath), directory);
    raw.command(QStringLiteral("rm \"%1.mole-partial\"").arg(remotePath), directory);

    QVERIFY2(appeared,
        qPrintable(QStringLiteral("the upload never reached the server, so nothing was killed "
                                  "mid-flight. The victim said: %1")
                       .arg(transcript)));

    // The claim. Part of a file is on the server -- that is unavoidable and not
    // the fault. The fault is that part of a file is sitting under the name
    // somebody asked for, where the next thing to open it cannot tell.
    QVERIFY2(!after.contains(name),
        qPrintable(QStringLiteral("a killed upload left %1, which looks like a finished file").arg(name)));
    QVERIFY2(after.contains(partialName),
        qPrintable(QStringLiteral("expected the wreckage under %1; the directory held %2")
                       .arg(partialName, after.join(QStringLiteral(", ")))));
}

/// The one case trust-on-first-use exists to catch.
///
/// Accepting a key from a host nobody has met is a judgement call, and Mole
/// makes it: a new host is trusted and its key recorded. A host whose key has
/// *changed* is a different question entirely, and there is no judgement in it —
/// either the server was rebuilt or somebody is standing in the middle, and the
/// two are indistinguishable from here. ADR-0011 says refuse, always, and this
/// is what holds it to that.
///
/// It works on a `known_hosts` of its own. Doing this to the account's real one
/// would be interfering with the machine the suite runs on.
void TestSftpFileSystem::aHostKeyThatChangedIsRefused()
{
    const Account account = accountFromEnvironment();
    if (!account.isConfigured())
        QSKIP("No SFTP account in the environment.");

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString knownHosts = scratch.filePath(QStringLiteral("known_hosts"));

    SftpSettings settings;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.user;
    settings.password = account.password;
    settings.remoteRoot = account.base;
    settings.knownHostsPath = knownHosts;
    settings.acceptNewHostKey = true;

    const VfsUri root(QStringLiteral("sftp"), QString(), QStringLiteral("/"));

    {
        // Met for the first time, so the key is taken on trust and written down.
        SftpFileSystem fileSystem(QStringLiteral("sftp"), settings);
        const Result<FileEntryList> listing = fileSystem.list(root, CancelToken());
        QVERIFY2(listing.ok(), qPrintable(listing.error().message));
    }

    QFile file(knownHosts);
    QVERIFY2(file.open(QIODevice::ReadOnly), "the first connection should have written a known_hosts");
    QByteArray recorded = file.readAll();
    file.close();
    QVERIFY2(!recorded.trimmed().isEmpty(), "nothing was recorded, so nothing can have changed");

    // The same host, a different key.
    //
    // The key has to stay *valid* and simply be a different one. Flipping a
    // character of the base64 was the obvious thing and it is wrong: the blob
    // stops decoding, the parser discards the line, and a discarded line reads
    // as a host nobody has ever met -- so the connection is accepted on trust
    // and the test passes for the wrong reason. Decoding the blob and changing a
    // byte of the key material inside it keeps every length prefix intact, which
    // is what an impostor's key looks like from here.
    QByteArray line;
    for (const QByteArray& candidate : recorded.split('\n')) {
        if (!candidate.trimmed().isEmpty() && !candidate.startsWith('#')) {
            line = candidate.trimmed();
            break;
        }
    }
    const int lastSpace = line.lastIndexOf(' ');
    QVERIFY2(lastSpace > 0,
        qPrintable(QStringLiteral("unrecognised known_hosts line: %1").arg(QString::fromUtf8(line))));

    QByteArray blob = QByteArray::fromBase64(line.mid(lastSpace + 1));
    QVERIFY2(blob.size() > 16, "the recorded key did not decode, so nothing was really recorded");
    blob[blob.size() - 3] = static_cast<char>(blob.at(blob.size() - 3) ^ 0x40);

    const QByteArray changed = line.left(lastSpace + 1) + blob.toBase64() + "\n";
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(changed);
    file.close();

    // And now it is a host we have met, whose key is not the one we recorded.
    // Trusting new hosts is beside the point: this one is not new.
    SftpFileSystem again(QStringLiteral("sftp"), settings);
    const Result<FileEntryList> refused = again.list(root, CancelToken());

    QVERIFY2(!refused.ok(),
        "a host whose key had changed was connected to anyway, which is the whole thing this policy is for");
    QVERIFY2(!refused.error().message.isEmpty(),
        "a refusal has to say something; being turned away for no stated reason is indistinguishable "
        "from the server being down");
}

/// The half of MOLE-374 that only a server can answer.
///
/// Key-only login is an explicitly supported configuration -- `create()` accepts
/// "either a password or a private key" -- and the live suite had only ever used
/// a password, so a drive that listed directories and then failed every other
/// operation went unnoticed. This is the run that would have caught it: no
/// password anywhere, and every operation the quote-command and transfer paths
/// use, each of which takes its own lease.
void TestSftpFileSystem::aKeyOnlyDriveDoesEverythingAndNotOnlyBrowsing()
{
    const Account account = accountFromEnvironment();
    if (!account.hasKey()) {
        QSKIP("No key-only SFTP account in the environment; set MOLE_TEST_SFTP_KEY (and "
              "MOLE_TEST_SFTP_KEY_PASS if the key has one) beside MOLE_TEST_SFTP_HOST and "
              "MOLE_TEST_SFTP_USER to run this against a real server.");
    }

    SftpSettings settings;
    settings.host = account.host;
    settings.port = account.port;
    settings.username = account.user;
    // Deliberately empty. With a password to fall back on, every lease but the
    // listing's would have logged in anyway and the fault would be invisible.
    settings.password.clear();
    settings.privateKeyPath = account.privateKeyPath;
    settings.privateKeyPassphrase = account.privateKeyPassphrase;
    settings.remoteRoot = account.base;

    SftpFileSystem drive(QStringLiteral("sftp"), settings);
    const VfsUri root(QStringLiteral("sftp"), QString(), QStringLiteral("/"));
    const QString name = QStringLiteral("mole-key-only-%1").arg(QCoreApplication::applicationPid());
    const VfsUri folder = root.child(name);

    // Browsing worked all along; it is here so a failure says which step broke.
    const Result<FileEntryList> listed = drive.list(root, CancelToken());
    QVERIFY2(listed.ok(), qPrintable(listed.error().message));

    // A quote command, on its own lease.
    const Result<void> made = drive.makeDirectory(folder);
    QVERIFY2(made.ok(), qPrintable(made.error().message));

    // A write, a read and a rename, each on another one.
    const VfsUri file = folder.child(QStringLiteral("payload.bin"));
    const QByteArray contents(4096, 'k');
    Result<std::unique_ptr<QIODevice>> writer = drive.openWrite(file, contents.size());
    QVERIFY2(writer.ok(), qPrintable(writer.error().message));
    QCOMPARE(writer.value()->write(contents), static_cast<qint64>(contents.size()));
    writer.value()->close();

    Result<std::unique_ptr<QIODevice>> reader = drive.openRead(file, contents.size());
    QVERIFY2(reader.ok(), qPrintable(reader.error().message));
    QCOMPARE(reader.value()->readAll(), contents);
    reader.value()->close();

    const VfsUri renamed = folder.child(QStringLiteral("payload.done"));
    const Result<void> moved = drive.rename(file, renamed);
    QVERIFY2(moved.ok(), qPrintable(moved.error().message));

    const Result<void> gone = drive.remove(folder, true);
    QVERIFY2(gone.ok(), qPrintable(gone.error().message));
}

MOLE_TEST_MAIN(TestSftpFileSystem)

#include "tst_SftpFileSystem.moc"
