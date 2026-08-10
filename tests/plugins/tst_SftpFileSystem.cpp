#include "plugins/network/SftpFileSystem.h"
#include "support/FileSystemConformance.h"
#include "support/TestbedControl.h"
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
    void aLargeFileArrivesWhole();
    void aLargeFileGoesUpWhole();
    void aReadWhoseConnectionIsCutDoesNotLookLikeAWholeFile();
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
    const QString limited = TestbedControl::run(
        { QStringLiteral("netem"), QStringLiteral("rate"), QStringLiteral("40mbit") });
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

MOLE_TEST_MAIN(TestSftpFileSystem)

#include "tst_SftpFileSystem.moc"
