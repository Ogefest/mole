#include "plugins/network/FtpFileSystem.h"
#include "plugins/network/S3FileSystem.h"
#include "plugins/network/SftpFileSystem.h"
#include "plugins/network/WebdavFileSystem.h"
#include "scale/HeavyPayload.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLocale>
#include <QStorageInfo>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

namespace {

QString env(const char* name)
{
    return QString::fromLocal8Bit(qgetenv(name));
}

qint64 envBytes(const char* name, qint64 fallback)
{
    bool ok = false;
    const qint64 value = env(name).toLongLong(&ok);
    return ok && value > 0 ? value : fallback;
}

/// A drive this tier can push a large file at, and what it may be asked for.
struct Target
{
    QString name;
    FileSystemPtr fileSystem;
    VfsUri directory;
    /// What this destination has room for, when the machine it lives on cannot
    /// be asked. Zero means no limit was declared.
    qint64 capacity = 0;
};

/// Every backend the environment has been told about.
///
/// Nothing is hardcoded and nothing is guessed: a drive that has no variables
/// set is simply not in the list, and the run says which ones it had. That is
/// the same rule the live conformance suites already follow, and it is what
/// keeps this suite green on a machine with no server to talk to.
QList<Target> targetsFromEnvironment()
{
    QList<Target> targets;

    if (!env("MOLE_TEST_SFTP_HOST").isEmpty()) {
        const auto sftpTarget = [](const QString& label, int port, qint64 capacity) {
            SftpSettings settings;
            settings.host = env("MOLE_TEST_SFTP_HOST");
            settings.port = port;
            settings.username = env("MOLE_TEST_SFTP_USER");
            settings.password = env("MOLE_TEST_SFTP_PASS");
            settings.remoteRoot = QStringLiteral("/");

            Target target;
            target.name = label;
            target.fileSystem = std::make_shared<SftpFileSystem>(QStringLiteral("sftp"), settings);
            QString base = env("MOLE_TEST_SFTP_BASE");
            if (base.isEmpty())
                base = QStringLiteral("/Shared");
            target.directory = VfsUri(QStringLiteral("sftp"), QString(), base);
            target.capacity = capacity;
            return target;
        };

        const int port = env("MOLE_TEST_SFTP_PORT").toInt();
        targets.append(sftpTarget(
            QStringLiteral("sftp"), port > 0 ? port : 22, envBytes("MOLE_TEST_HEAVY_CAP_SFTP", 0)));

        // The second server configuration: a cipher with a small block size and
        // an explicit RekeyLimit, which is the pairing a large read used to stop
        // dead in. See ADR-0013.
        const int rekeyPort = env("MOLE_TEST_SFTP_REKEY_PORT").toInt();
        if (rekeyPort > 0) {
            targets.append(
                sftpTarget(QStringLiteral("sftp-rekey"), rekeyPort, envBytes("MOLE_TEST_HEAVY_CAP_SFTP", 0)));
        }
    }

    if (!env("MOLE_TEST_S3_KEY_ID").isEmpty() && !env("MOLE_TEST_S3_BUCKET").isEmpty()) {
        S3Settings settings;
        settings.accessKeyId = env("MOLE_TEST_S3_KEY_ID");
        settings.secretAccessKey = env("MOLE_TEST_S3_SECRET");
        settings.bucket = env("MOLE_TEST_S3_BUCKET");
        settings.endpoint = env("MOLE_TEST_S3_ENDPOINT");
        settings.region = env("MOLE_TEST_S3_REGION");
        settings.pathStyleAddressing = true;
        settings.useHttps = settings.endpoint.startsWith(QLatin1String("https"));
        settings.verifyTls = env("MOLE_TEST_IGNORE_SELF_SIGNED_CERT").isEmpty();
        if (settings.endpoint.startsWith(QLatin1String("http://")))
            settings.endpoint = settings.endpoint.mid(7);
        else if (settings.endpoint.startsWith(QLatin1String("https://")))
            settings.endpoint = settings.endpoint.mid(8);

        Target target;
        target.name = QStringLiteral("s3");
        target.fileSystem = std::make_shared<S3FileSystem>(QStringLiteral("s3"), settings);
        target.directory = VfsUri(QStringLiteral("s3"), QString(), QStringLiteral("/"));
        target.capacity = envBytes("MOLE_TEST_HEAVY_CAP_S3", 0);
        targets.append(target);
    }

    if (!env("MOLE_TEST_WEBDAV_URL").isEmpty()) {
        WebdavSettings settings;
        settings.baseUrl = env("MOLE_TEST_WEBDAV_URL");
        settings.username = env("MOLE_TEST_WEBDAV_USER");
        settings.password = env("MOLE_TEST_WEBDAV_PASS");
        settings.verifyTls = env("MOLE_TEST_IGNORE_SELF_SIGNED_CERT").isEmpty();
        settings.remoteRoot = QStringLiteral("/");

        Target target;
        target.name = QStringLiteral("webdav");
        target.fileSystem = std::make_shared<WebdavFileSystem>(QStringLiteral("dav"), settings);
        target.directory = VfsUri(QStringLiteral("dav"), QString(), QStringLiteral("/"));
        target.capacity = envBytes("MOLE_TEST_HEAVY_CAP_WEBDAV", 0);
        targets.append(target);
    }

    if (!env("MOLE_TEST_FTP_HOST").isEmpty()) {
        FtpSettings settings;
        settings.host = env("MOLE_TEST_FTP_HOST");
        const int port = env("MOLE_TEST_FTP_PORT").toInt();
        settings.port = port > 0 ? port : 21;
        settings.username = env("MOLE_TEST_FTP_USER");
        settings.password = env("MOLE_TEST_FTP_PASS");
        settings.verifyTls = env("MOLE_TEST_IGNORE_SELF_SIGNED_CERT").isEmpty();
        settings.remoteRoot = QStringLiteral("/");

        Target target;
        target.name = QStringLiteral("ftp");
        target.fileSystem = std::make_shared<FtpFileSystem>(QStringLiteral("ftp"), settings);
        QString base = env("MOLE_TEST_FTP_BASE");
        if (base.isEmpty())
            base = QStringLiteral("/Shared");
        target.directory = VfsUri(QStringLiteral("ftp"), QString(), base);
        target.capacity = envBytes("MOLE_TEST_HEAVY_CAP_FTP", 0);
        targets.append(target);
    }

    return targets;
}

QString rate(qint64 bytes, qint64 milliseconds)
{
    if (milliseconds <= 0)
        return QStringLiteral("instant");
    const double megabytes = double(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MiB/s").arg(megabytes / (double(milliseconds) / 1000.0), 0, 'f', 1);
}

} // namespace

/// The sizes that break things, against a real server.
///
/// Not part of `make test`: these move gigabytes and need a machine to move them
/// to. `make test-heavy` runs them, and without the environment variables that
/// name a server every case skips itself and says so -- a skip is a result here,
/// which is why each one names what was missing rather than passing quietly.
///
/// What is asserted is deliberately more than "the copy worked":
///
/// - **every byte arrives**, checked against what belongs at each offset
/// - **peak temporary space stays small**, which is the check that would have
///   caught staging before it became a wall (ADR-0014)
/// - **memory and file descriptors come back**, so a leak shows up here rather
///   than after eight hours of somebody's backup
/// - **throughput is recorded**, so a regression in speed is as visible as one
///   in correctness
class TestHeavyTransfers : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void aLargeLocalCopyKeepsItsResources();
    void aFileAcrossAThirtyTwoBitBoundaryArrivesWhole_data();
    void aFileAcrossAThirtyTwoBitBoundaryArrivesWhole();
    void aLargeFileMakesTheRoundTrip_data();
    void aLargeFileMakesTheRoundTrip();

private:
    /// The payload every case uses, in bytes.
    qint64 payloadBytes() const { return m_payloadBytes; }
    /// Writes the source file and returns its uri, or fails the test.
    VfsUri makeSource(const QString& name);
    void record(const QString& scenario, qint64 bytes, qint64 milliseconds, const ResourceWatch& watch);

    qint64 m_payloadBytes = 0;
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::unique_ptr<QTemporaryDir> m_dir;
    QStringList m_report;
};

void TestHeavyTransfers::initTestCase()
{
    // Small by default so that running this binary by hand is quick; the
    // `make test-heavy` script is what asks for gigabytes.
    m_payloadBytes = envBytes("MOLE_TEST_HEAVY_BYTES", 256 * 1024 * 1024);
    qInfo("payload: %s", qPrintable(QLocale().formattedDataSize(m_payloadBytes)));
}

void TestHeavyTransfers::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_disk = std::make_shared<LocalFileSystem>();
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void TestHeavyTransfers::cleanup()
{
    m_tasks.reset();
    m_disk.reset();
    m_dir.reset();
}

VfsUri TestHeavyTransfers::makeSource(const QString& name)
{
    const QString path = QDir(m_dir->path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning("could not write the payload to %s", qPrintable(path));
        return {};
    }

    QString problem;
    if (!HeavyPayload::writeTo(file, m_payloadBytes, &problem)) {
        qWarning("%s", qPrintable(problem));
        return {};
    }
    file.close();
    return VfsUri::fromLocalPath(path);
}

void TestHeavyTransfers::record(
    const QString& scenario, qint64 bytes, qint64 milliseconds, const ResourceWatch& watch)
{
    const QString line = QStringLiteral("%1  %2  %3  %4")
                             .arg(scenario, -28)
                             .arg(QLocale().formattedDataSize(bytes), -12)
                             .arg(rate(bytes, milliseconds), -14)
                             .arg(watch.summary());
    qInfo("%s", qPrintable(line));
    m_report.append(line);

    // Appended rather than printed only, so two runs a month apart can be put
    // side by side: a copy that is correct and half the speed it was is a
    // regression nobody would otherwise notice.
    const QString reportPath = env("MOLE_TEST_HEAVY_REPORT");
    if (reportPath.isEmpty())
        return;
    QFile report(reportPath);
    if (report.open(QIODevice::Append | QIODevice::Text))
        report.write(line.toUtf8() + '\n');
}

void TestHeavyTransfers::aLargeLocalCopyKeepsItsResources()
{
    // No server needed, so this one runs wherever the tier is run. Local disk
    // is also the only backend that stages nothing by design, which makes it
    // the control: whatever scratch space this reports is the floor.
    const VfsUri source = makeSource(QStringLiteral("payload.bin"));
    QVERIFY(source.isValid());
    QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("dst")));

    TransferTask::Request request;
    request.sourceFileSystem = m_disk;
    request.targetFileSystem = m_disk;
    request.sources = { source };
    request.targetDirectory = VfsUri::fromLocalPath(QDir(m_dir->path()).filePath(QStringLiteral("dst")));

    QElapsedTimer clock;
    ResourceWatch watch;
    clock.start();

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 3600000));
    const qint64 elapsed = clock.elapsed();
    watch.stop();

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
    QCOMPARE(task->copiedCount(), 1);
    record(QStringLiteral("local -> local"), m_payloadBytes, elapsed, watch);

    QFile copy(QDir(m_dir->path()).filePath(QStringLiteral("dst/payload.bin")));
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QString problem;
    QVERIFY2(HeavyPayload::verify(copy, m_payloadBytes, &problem), qPrintable(problem));

    QVERIFY2(watch.peakResidentGrowthBytes() < 512LL * 1024 * 1024,
        qPrintable(QStringLiteral("a copy grew by %1 KiB").arg(watch.peakResidentGrowthBytes() / 1024)));
    // A few above the baseline rather than exactly it: the sampler itself opens
    // /proc while it looks, and a thread pool keeps an eventfd or two. What this
    // catches is the shape that matters -- one descriptor kept per file, which
    // over a directory of ten thousand ends the process.
    QVERIFY2(ResourceWatch::openDescriptors() <= watch.baselineDescriptors() + 4,
        qPrintable(QStringLiteral("descriptors went from %1 to %2")
                       .arg(watch.baselineDescriptors())
                       .arg(ResourceWatch::openDescriptors())));
}

void TestHeavyTransfers::aFileAcrossAThirtyTwoBitBoundaryArrivesWhole_data()
{
    QTest::addColumn<qint64>("bytes");

    // The sizes where somebody's arithmetic overflows: ours, Qt's, libcurl's, or
    // the server's. One byte past each is the whole point -- a length that fits
    // exactly proves nothing about the code that adds one to it.
    QTest::newRow("a gibibyte and one") << (1LL << 30) + 1;
    QTest::newRow("two gibibytes and one") << (1LL << 31) + 1;
    QTest::newRow("four gibibytes and one") << (1LL << 32) + 1;
}

void TestHeavyTransfers::aFileAcrossAThirtyTwoBitBoundaryArrivesWhole()
{
    QFETCH(qint64, bytes);

    // Local disk only. These are about integer widths rather than about a
    // protocol, and asking a test server for eight gigabytes of room to prove
    // something arithmetic would be a poor trade -- the remote sizes are the
    // round trip above.
    const qint64 needed = 2 * bytes + 256LL * 1024 * 1024;
    const qint64 room = QStorageInfo(m_dir->path()).bytesAvailable();
    if (room < needed) {
        QSKIP(qPrintable(QStringLiteral("needs %1 free and there is %2")
                             .arg(QLocale().formattedDataSize(needed), QLocale().formattedDataSize(room))));
    }

    const qint64 previous = m_payloadBytes;
    m_payloadBytes = bytes;
    const VfsUri source = makeSource(QStringLiteral("boundary.bin"));
    m_payloadBytes = previous;
    QVERIFY(source.isValid());
    QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("dst")));

    TransferTask::Request request;
    request.sourceFileSystem = m_disk;
    request.targetFileSystem = m_disk;
    request.sources = { source };
    request.targetDirectory = VfsUri::fromLocalPath(QDir(m_dir->path()).filePath(QStringLiteral("dst")));

    QElapsedTimer clock;
    ResourceWatch watch;
    clock.start();

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task, 7200000));
    const qint64 elapsed = clock.elapsed();
    watch.stop();

    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
    QCOMPARE(task->copiedCount(), 1);
    record(QStringLiteral("local -> local, %1 bytes").arg(bytes), bytes, elapsed, watch);

    // Every byte against what belongs at that offset, because the failure this
    // is looking for is a length that wrapped -- which leaves a file of exactly
    // the right size with the wrong bytes at the far end.
    QFile copy(QDir(m_dir->path()).filePath(QStringLiteral("dst/boundary.bin")));
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.size(), bytes);
    QString problem;
    QVERIFY2(HeavyPayload::verify(copy, bytes, &problem), qPrintable(problem));

    QFile::remove(QDir(m_dir->path()).filePath(QStringLiteral("boundary.bin")));
    QFile::remove(QDir(m_dir->path()).filePath(QStringLiteral("dst/boundary.bin")));
}

void TestHeavyTransfers::aLargeFileMakesTheRoundTrip_data()
{
    QTest::addColumn<QString>("backend");

    const QList<Target> targets = targetsFromEnvironment();
    if (targets.isEmpty()) {
        // A row all the same, so the run says "nothing was configured" out
        // loud. A data-driven case with no rows is not a skip, it is silence --
        // and silence is exactly how three backends went a year without ever
        // being tested against a server.
        QTest::newRow("nothing configured") << QString();
        return;
    }
    for (const Target& target : targets)
        QTest::newRow(qPrintable(target.name)) << target.name;
}

void TestHeavyTransfers::aLargeFileMakesTheRoundTrip()
{
    QFETCH(QString, backend);
    if (backend.isEmpty()) {
        QSKIP("No server in the environment. Set MOLE_TEST_SFTP_HOST, MOLE_TEST_S3_KEY_ID, "
              "MOLE_TEST_WEBDAV_URL or MOLE_TEST_FTP_HOST -- `make test-heavy` derives them all "
              "from the address of the test machine.");
    }

    Target target;
    for (const Target& candidate : targetsFromEnvironment()) {
        if (candidate.name == backend)
            target = candidate;
    }
    QVERIFY2(target.fileSystem != nullptr, "the drive disappeared between listing and running");

    // A destination that cannot hold it is a skip with the reason, not a
    // failure and not a silent pass. Filling a test machine's disk would take
    // every other suite down with it.
    if (target.capacity > 0 && target.capacity < m_payloadBytes * 2) {
        QSKIP(qPrintable(QStringLiteral("%1 is declared to have room for %2 and this needs twice %3")
                             .arg(backend, QLocale().formattedDataSize(target.capacity),
                                 QLocale().formattedDataSize(m_payloadBytes))));
    }

    const QString name = QStringLiteral("mole-heavy-%1.bin").arg(QCoreApplication::applicationPid());
    const VfsUri source = makeSource(name);
    QVERIFY(source.isValid());
    const VfsUri landed = target.directory.child(name);

    // ---- up ------------------------------------------------------------
    {
        TransferTask::Request request;
        request.sourceFileSystem = m_disk;
        request.targetFileSystem = target.fileSystem;
        request.sources = { source };
        request.targetDirectory = target.directory;
        request.onConflict = TransferTask::Conflict::Overwrite;

        QElapsedTimer clock;
        ResourceWatch watch;
        clock.start();

        auto* task = new TransferTask(request);
        m_tasks->submit(task);
        QVERIFY2(waitForTask(task, 7200000), "the upload never finished");
        const qint64 elapsed = clock.elapsed();
        watch.stop();

        QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
        record(QStringLiteral("local -> %1").arg(backend), m_payloadBytes, elapsed, watch);

        // The assertion this whole tier exists for. A copy that streams needs
        // room for a chunk; one that stages needs room for the file, and the
        // difference is invisible until the file is bigger than the disk.
        const qint64 allowed = qMax<qint64>(64LL * 1024 * 1024, m_payloadBytes / 50);
        QVERIFY2(watch.peakScratchBytes() < allowed,
            qPrintable(QStringLiteral("uploading %1 used %2 of temporary space, which is staging")
                           .arg(QLocale().formattedDataSize(m_payloadBytes),
                               QLocale().formattedDataSize(watch.peakScratchBytes()))));
        QVERIFY2(watch.peakResidentGrowthBytes() < 1024LL * 1024 * 1024,
            qPrintable(
                QStringLiteral("the upload grew by %1 KiB").arg(watch.peakResidentGrowthBytes() / 1024)));
    }

    // ---- and back, byte for byte ----------------------------------------
    {
        QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("back")));
        const VfsUri backDirectory
            = VfsUri::fromLocalPath(QDir(m_dir->path()).filePath(QStringLiteral("back")));

        TransferTask::Request request;
        request.sourceFileSystem = target.fileSystem;
        request.targetFileSystem = m_disk;
        request.sources = { landed };
        request.targetDirectory = backDirectory;
        request.onConflict = TransferTask::Conflict::Overwrite;

        QElapsedTimer clock;
        ResourceWatch watch;
        clock.start();

        auto* task = new TransferTask(request);
        m_tasks->submit(task);
        QVERIFY2(waitForTask(task, 7200000), "the download never finished");
        const qint64 elapsed = clock.elapsed();
        watch.stop();

        QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QLatin1Char(' '))));
        record(QStringLiteral("%1 -> local").arg(backend), m_payloadBytes, elapsed, watch);

        const qint64 allowed = qMax<qint64>(64LL * 1024 * 1024, m_payloadBytes / 50);
        QVERIFY2(watch.peakScratchBytes() < allowed,
            qPrintable(QStringLiteral("downloading %1 used %2 of temporary space, which is staging")
                           .arg(QLocale().formattedDataSize(m_payloadBytes),
                               QLocale().formattedDataSize(watch.peakScratchBytes()))));

        QFile copy(QDir(m_dir->path()).filePath(QStringLiteral("back/") + name));
        QVERIFY2(copy.open(QIODevice::ReadOnly), "nothing came back");
        QString problem;
        QVERIFY2(HeavyPayload::verify(copy, m_payloadBytes, &problem), qPrintable(problem));
    }

    // The machine is left as it was found, whatever happened above.
    target.fileSystem->remove(landed, false);
}

MOLE_TEST_MAIN(TestHeavyTransfers)
#include "tst_HeavyTransfers.moc"
