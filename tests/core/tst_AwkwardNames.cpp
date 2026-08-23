#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/NameRules.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTest>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace mole;
using namespace mole::test;

/// Names people really have, and shapes a filesystem really holds.
///
/// Every one of these is a name somebody's camera, phone or build system
/// produced, and each breaks a different layer: a quote breaks a command line, a
/// hash breaks a url, a newline breaks a protocol that sends one command per
/// line, and a combining character breaks the assumption that two files with the
/// same name on screen are the same file.
///
/// What is offline here is the part that is: local disk is a real filesystem, so
/// a name that survives a copy over it has survived a real `open`. The parts
/// that need a server -- what SFTP does with a quote, what a url builder does
/// with a hash -- are the same names against the testbed.
class TestAwkwardNames : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void cleanupTestCase();

    void aNameSurvivesACopyWhateverIsInIt_data();
    void aNameSurvivesACopyWhateverIsInIt();
    void aNameSurvivesBeingMadeIntoAUriAndBack_data();
    void aNameSurvivesBeingMadeIntoAUriAndBack();

    void twoNamesThatLookAlikeStayTwoFiles();
    void aSymlinkLoopDoesNotTrapTheWalk();
    void aBrokenSymlinkIsReportedAndTheRestOfTheJobGoesOn();
    void aFifoIsRefusedRatherThanHungOn();
    void aFileNobodyMayReadIsReportedAndTheCountIsHonest();

private:
    TransferTask* copyEverythingFromSource();

    /// How many of the table's names were really put through a copy. Counted
    /// because the interesting failure of a suite that skips per row is a table
    /// that quietly stopped covering anything.
    int m_exercised = 0;
    /// What the disk under the fixture accepts. Read from the backend once and
    /// kept, because cleanup() releases it before cleanupTestCase() runs -- and
    /// because one source of truth is what makes the count below checkable.
    NameRules m_diskRules = LocalFileSystem().nameRules();

    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<LocalFileSystem> m_disk;
    std::unique_ptr<TempTree> m_tree;
};

void TestAwkwardNames::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_disk = std::make_shared<LocalFileSystem>();
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->makeDirs(QStringLiteral("source")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("arrived")));
}

void TestAwkwardNames::cleanupTestCase()
{
    // Every name in the table, on a filesystem that holds them all. Without
    // this, weakening a name -- or a rule set that started refusing one by
    // mistake -- would trade real coverage for a green tick, and the run would
    // look exactly the same.
    static constexpr int kNamesInTheTable = 20;
    static constexpr int kOnlyAPosixFilesystemHoldsThese = 6;

    const int expected = checkName(QStringLiteral("really?.txt"), m_diskRules).isRejected()
        ? kNamesInTheTable - kOnlyAPosixFilesystemHoldsThese
        : kNamesInTheTable;

    QCOMPARE(m_exercised, expected);
}

void TestAwkwardNames::cleanup()
{
    m_tasks.reset();
    m_disk.reset();
    m_tree.reset();
}

TransferTask* TestAwkwardNames::copyEverythingFromSource()
{
    TransferTask::Request request;
    request.sourceFileSystem = m_disk;
    request.targetFileSystem = m_disk;
    request.sources = { m_tree->rootUri().child(QStringLiteral("source")) };
    request.targetDirectory = m_tree->rootUri().child(QStringLiteral("arrived"));

    auto* task = new TransferTask(request);
    m_tasks->submit(task);
    return waitForTask(task, 30000) ? task : nullptr;
}

void TestAwkwardNames::aNameSurvivesACopyWhateverIsInIt_data()
{
    QTest::addColumn<QString>("name");
    /// False for a name only a POSIX filesystem will hold. Stated per row rather
    /// than worked out, so that adding one is a decision, and checked against
    /// the rules below so that stating it wrongly is caught.
    QTest::addColumn<bool>("portable");

    QTest::newRow("a space") << QStringLiteral("holiday photos.jpg") << true;
    QTest::newRow("double quotes") << QStringLiteral("say \"cheese\".jpg") << false;
    QTest::newRow("a single quote") << QStringLiteral("mole's notes.txt") << true;
    QTest::newRow("a backslash") << QStringLiteral("back\\slash.txt") << false;
    QTest::newRow("a tab") << QStringLiteral("two\tcolumns.tsv") << false;
    QTest::newRow("a newline") << QStringLiteral("first\nsecond.txt") << false;
    QTest::newRow("a hash") << QStringLiteral("draft #3.txt") << true;
    QTest::newRow("a question mark") << QStringLiteral("really?.txt") << false;
    QTest::newRow("an ampersand") << QStringLiteral("this & that.txt") << true;
    QTest::newRow("a percent") << QStringLiteral("100% done.txt") << true;
    QTest::newRow("a plus and an equals") << QStringLiteral("a+b=c.txt") << true;
    QTest::newRow("something that looks encoded") << QStringLiteral("already%20encoded%2Fname.txt") << true;
    QTest::newRow("emoji") << QStringLiteral("holiday \xF0\x9F\x8F\x96\xEF\xB8\x8F.jpg") << true;
    QTest::newRow("a right-to-left mark") << QStringLiteral("report\xE2\x80\xAB.txt") << true;
    QTest::newRow("combining characters") << QStringLiteral("cafe\xCC\x81.txt") << true;
    QTest::newRow("three dots") << QStringLiteral("...") << false;
    QTest::newRow("dot dot something") << QStringLiteral("..foo") << true;
    QTest::newRow("a leading dash") << QStringLiteral("-rf.txt") << true;
    QTest::newRow("a leading double dash") << QStringLiteral("--force.txt") << true;
    // 255 bytes is the limit on every filesystem this runs on; one more is a
    // different answer from the kernel rather than from us.
    QTest::newRow("a 255-byte name") << QString(251, QLatin1Char('n')) + QStringLiteral(".txt") << true;
}

void TestAwkwardNames::aNameSurvivesACopyWhateverIsInIt()
{
    QFETCH(QString, name);
    QFETCH(bool, portable);

    // The group each name is in, checked rather than trusted. A row that claims
    // to be portable and is not would go on being skipped on Windows for a
    // reason nobody had noticed.
    const bool windowsWouldHoldIt
        = !checkName(name, NameRules::forPlatform(HostPlatform::Windows)).isRejected();
    QCOMPARE(windowsWouldHoldIt, portable);

    // Asked of the filesystem under the fixture, not of the platform. A
    // FAT-formatted stick or a share on Linux is stricter than the disk it is
    // plugged into, and the same code should handle it rather than a second
    // guess.
    if (const NameVerdict verdict = checkName(name, m_diskRules); verdict.isRejected()) {
        QSKIP(
            qPrintable(QStringLiteral("this filesystem will not hold \"%1\": %2").arg(name, verdict.reason)));
    }
    ++m_exercised;

    const QByteArray payload = QStringLiteral("belongs to %1").arg(name).toUtf8();
    if (!m_tree->writeFile(QStringLiteral("source/") + name, payload))
        QSKIP("this filesystem will not hold that name");

    TransferTask* task = copyEverythingFromSource();
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));

    // Read back with QFile under the name as it was written -- out of band, and
    // byte for byte, so a name that survived as a *similar* name still fails.
    QFile arrived(m_tree->absolute(QStringLiteral("arrived/source/") + name));
    QVERIFY2(
        arrived.open(QIODevice::ReadOnly), qPrintable(QStringLiteral("nothing arrived called %1").arg(name)));
    QCOMPARE(arrived.readAll(), payload);
}

void TestAwkwardNames::aNameSurvivesBeingMadeIntoAUriAndBack_data()
{
    aNameSurvivesACopyWhateverIsInIt_data();
}

void TestAwkwardNames::aNameSurvivesBeingMadeIntoAUriAndBack()
{
    QFETCH(QString, name);
    QFETCH(bool, portable);
    Q_UNUSED(portable)

    // Deliberately not skipped for anybody. A uri is not a native path, so what
    // a name does on the way through this layer is the same question on every
    // system -- and a remote drive really does hand a Windows client a name the
    // local disk would refuse.

    // Every remote backend builds a url out of a uri, and every one of them
    // parses one back. A name that changes on the way through is a file written
    // to the wrong place, or read from it -- and this is the layer where that
    // happens, whether or not there is a server to show it.
    const VfsUri uri = VfsUri::fromString(QStringLiteral("sftp://nas/photos")).child(name);
    QCOMPARE(uri.fileName(), name);

    const VfsUri parsed = VfsUri::fromString(uri.toString());
    QCOMPARE(parsed.scheme(), uri.scheme());
    QCOMPARE(parsed.authority(), uri.authority());
    QCOMPARE(parsed.path(), uri.path());
    QCOMPARE(parsed.fileName(), name);
}

void TestAwkwardNames::twoNamesThatLookAlikeStayTwoFiles()
{
    // The same word in two normal forms: "café" with one character, and with an
    // "e" followed by a combining accent. They look identical and are different
    // names, and a copy that folded them together would silently lose one.
    const QString composed = QString::fromUtf8("caf\xC3\xA9.txt");
    const QString decomposed = QString::fromUtf8("cafe\xCC\x81.txt");
    QVERIFY(composed != decomposed);

    if (!m_tree->writeFile(QStringLiteral("source/") + composed, QByteArray("one")))
        QSKIP("this filesystem will not hold that name");
    if (!m_tree->writeFile(QStringLiteral("source/") + decomposed, QByteArray("two")))
        QSKIP("this filesystem folds the two forms together");

    const Result<FileEntryList> listed
        = m_disk->list(m_tree->rootUri().child(QStringLiteral("source")), CancelToken());
    QVERIFY(listed.ok());
    if (listed.value().size() < 2)
        QSKIP("this filesystem folds the two forms together");

    TransferTask* task = copyEverythingFromSource();
    QVERIFY(task != nullptr);
    QVERIFY2(task->failures().isEmpty(), qPrintable(task->failures().join(QStringLiteral("; "))));
    QCOMPARE(task->copiedCount(), 2);

    QFile first(m_tree->absolute(QStringLiteral("arrived/source/") + composed));
    QFile second(m_tree->absolute(QStringLiteral("arrived/source/") + decomposed));
    QVERIFY(first.open(QIODevice::ReadOnly));
    QVERIFY(second.open(QIODevice::ReadOnly));
    QCOMPARE(first.readAll(), QByteArray("one"));
    QCOMPARE(second.readAll(), QByteArray("two"));
}

void TestAwkwardNames::aSymlinkLoopDoesNotTrapTheWalk()
{
    // A directory containing a link back to itself. Following it is an infinite
    // descent, and the copy that came of it would fill the disk with the same
    // three files at ever deeper paths.
    QVERIFY(m_tree->makeDirs(QStringLiteral("source/inner")));
    QVERIFY(m_tree->writeFile(QStringLiteral("source/inner/real.txt"), QByteArray("content")));
    QVERIFY(QFile::link(
        m_tree->absolute(QStringLiteral("source")), m_tree->absolute(QStringLiteral("source/inner/loop"))));

    TransferTask* task = copyEverythingFromSource();
    QVERIFY2(task != nullptr, "a copy of a tree with a loop in it has to finish");
    QVERIFY(QFile::exists(m_tree->absolute(QStringLiteral("arrived/source/inner/real.txt"))));
    // Whatever the link became, the descent stopped: nothing four levels down.
    QVERIFY2(!QFile::exists(m_tree->absolute(QStringLiteral("arrived/source/inner/loop/inner/loop"))),
        "the walk followed the link round again");
}

void TestAwkwardNames::aBrokenSymlinkIsReportedAndTheRestOfTheJobGoesOn()
{
    // A link to something that is not there. It is a name in the directory and
    // it cannot be read, which is a reason to report it -- and not a reason to
    // abandon the other files.
    QVERIFY(m_tree->writeFile(QStringLiteral("source/good.txt"), QByteArray("fine")));
    QVERIFY(QFile::link(m_tree->absolute(QStringLiteral("source/not-there.txt")),
        m_tree->absolute(QStringLiteral("source/dangling"))));

    TransferTask* task = copyEverythingFromSource();
    QVERIFY(task != nullptr);
    QFile good(m_tree->absolute(QStringLiteral("arrived/source/good.txt")));
    QVERIFY2(good.open(QIODevice::ReadOnly), "the file beside the broken link still had to arrive");
    QCOMPARE(good.readAll(), QByteArray("fine"));
}

void TestAwkwardNames::aFifoIsRefusedRatherThanHungOn()
{
#ifndef Q_OS_UNIX
    QSKIP("no fifos on this platform");
#else
    // Opening a fifo for reading blocks until somebody writes to it, which for a
    // copy means for ever. A file manager that hangs on one entry in a folder is
    // a file manager that has stopped.
    QVERIFY(m_tree->writeFile(QStringLiteral("source/ordinary.txt"), QByteArray("fine")));
    const QByteArray fifoPath = m_tree->absolute(QStringLiteral("source/pipe")).toLocal8Bit();
    if (mkfifo(fifoPath.constData(), 0644) != 0)
        QSKIP("could not create a fifo here");

    TransferTask* task = copyEverythingFromSource();
    QVERIFY2(task != nullptr, "a copy of a folder containing a fifo has to finish");

    QFile ordinary(m_tree->absolute(QStringLiteral("arrived/source/ordinary.txt")));
    QVERIFY2(ordinary.open(QIODevice::ReadOnly), "the ordinary file beside it still had to arrive");
    QCOMPARE(ordinary.readAll(), QByteArray("fine"));
#endif
}

void TestAwkwardNames::aFileNobodyMayReadIsReportedAndTheCountIsHonest()
{
#ifndef Q_OS_UNIX
    QSKIP("permissions work differently on this platform");
#else
    if (geteuid() == 0)
        QSKIP("running as root, where permissions are not refused");

    // One file in a folder that this account may not read. The rest of the copy
    // goes, the one that could not is named, and the count is what actually
    // arrived rather than what was attempted.
    QVERIFY(m_tree->writeFile(QStringLiteral("source/readable.txt"), QByteArray("fine")));
    QVERIFY(m_tree->writeFile(QStringLiteral("source/secret.txt"), QByteArray("not for you")));
    if (!madeUnreadable(m_tree->absolute(QStringLiteral("source/secret.txt"))))
        QSKIP("this account can read a file with no permissions at all");

    TransferTask* task = copyEverythingFromSource();
    QVERIFY(task != nullptr);
    QCOMPARE(task->copiedCount(), 1);
    QCOMPARE(task->failedCount(), 1);
    QVERIFY2(task->failures().first().contains(QStringLiteral("secret.txt")),
        qPrintable(task->failures().first()));
    QVERIFY(QFile::exists(m_tree->absolute(QStringLiteral("arrived/source/readable.txt"))));

    // Left readable again so the temporary directory can be cleaned up.
    QFile::setPermissions(
        m_tree->absolute(QStringLiteral("source/secret.txt")), QFile::ReadOwner | QFile::WriteOwner);
#endif
}

MOLE_TEST_MAIN(TestAwkwardNames)

#include "tst_AwkwardNames.moc"
