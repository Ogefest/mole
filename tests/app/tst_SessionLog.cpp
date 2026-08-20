#include "app/SessionLog.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace mole;
using namespace mole::test;

/// The log exists for the runs that end badly, so the tests have to end badly
/// too. Each crash happens in a forked child: the parent stays alive to read
/// what the child managed to write on its way down.
class TestSessionLog : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void writesWhatTheProgramSays();
    void keepsThePreviousRun();
    void aLogThatEndsInACrashSurvivesTheRunsAfterIt();
    void aCrashLeavesABacktrace();
    void aCrashStillReachesTheDefaultHandler();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;

    QString contents() const
    {
        QFile file(m_path);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }

    /// Runs `body` in a child process and returns how it died.
    static int died(const std::function<void()>& body)
    {
        const pid_t pid = fork();
        if (pid == 0) {
            body();
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return status;
    }
};

void TestSessionLog::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->filePath(QStringLiteral("session.log"));
    qputenv("MOLE_LOG_PATH", m_path.toLocal8Bit());
}

void TestSessionLog::writesWhatTheProgramSays()
{
    QCOMPARE(sessionLog::install(), m_path);
    qWarning("a thing went wrong");
    qInfo("and then a thing went right");
    sessionLog::shutdown();

    const QString text = contents();
    QVERIFY2(text.contains(QStringLiteral("a thing went wrong")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("and then a thing went right")), qPrintable(text));
    // The level is worth having: a log where nothing is marked as a warning is
    // a log nobody skims successfully.
    QVERIFY2(text.contains(QStringLiteral("warning")), qPrintable(text));
}

void TestSessionLog::keepsThePreviousRun()
{
    sessionLog::install();
    qWarning("the run that crashed");
    sessionLog::shutdown();

    // Starting again is how anybody reacts to a crash, and it must not be what
    // destroys the explanation.
    sessionLog::install();
    qWarning("the run after it");
    sessionLog::shutdown();

    QFile previous(m_path + QStringLiteral(".1"));
    QVERIFY(previous.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(previous.readAll()).contains(QStringLiteral("the run that crashed")));
    QVERIFY(contents().contains(QStringLiteral("the run after it")));
}

/// One previous run is not enough when the previous run crashed.
///
/// Noticing a crash takes one restart and diagnosing it takes another, and two
/// restarts is exactly what rotation keeping a single `.1` throws away. That is how
/// the backtrace for MOLE-265 was lost: a segfault, then two `make run` attempts, and
/// the frames that named the fault were gone. A log that ends in a crash is the one
/// log worth keeping, so it is moved aside under a name rotation never reuses.
void TestSessionLog::aLogThatEndsInACrashSurvivesTheRunsAfterIt()
{
    sessionLog::install();
    qWarning("the run that crashed");
    // The marker the crash handler writes, put there through the ordinary handler
    // rather than by crashing this process: what is being tested is the rotation, and
    // the real signal path is covered by aCrashLeavesABacktrace below.
    qWarning("---- crashed: signal 11 at 0xdeadbeef ----");
    sessionLog::shutdown();

    // Two more runs, which is one more than rotation keeps.
    for (const char* line : { "the run that noticed", "the run that went looking" }) {
        sessionLog::install();
        qWarning("%s", line);
        sessionLog::shutdown();
    }

    const QFileInfoList kept
        = QDir(m_dir->path())
              .entryInfoList({ QStringLiteral("session-crash-*.log") }, QDir::Files, QDir::Name);
    QCOMPARE(kept.size(), 1);
    QFile crashed(kept.first().absoluteFilePath());
    QVERIFY(crashed.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(crashed.readAll());
    QVERIFY2(text.contains(QStringLiteral("the run that crashed")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("crashed: signal")), qPrintable(text));

    // And an ordinary run is still only kept once, so a tidy directory stays tidy.
    QVERIFY(QFile::exists(m_path + QStringLiteral(".1")));
}

void TestSessionLog::aCrashLeavesABacktrace()
{
    const int status = died([] {
        sessionLog::install();
        qWarning("about to fall over");
        std::raise(SIGSEGV);
    });

    QVERIFY2(WIFSIGNALED(status), "the child died of a signal");
    QCOMPARE(WTERMSIG(status), SIGSEGV);

    const QString text = contents();
    // The last thing said before the fall is usually the thing worth reading.
    QVERIFY2(text.contains(QStringLiteral("about to fall over")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("crashed: signal 11")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("backtrace:")), qPrintable(text));
    // A backtrace of no frames is a header, not a backtrace.
    QVERIFY2(text.count(QLatin1Char('\n')) > 6, qPrintable(text));

    // And the next start sets this log aside rather than rotating it towards
    // oblivion -- checked here against the marker the handler really wrote, because
    // aLogThatEndsInACrashSurvivesTheRunsAfterIt puts that marker there itself and
    // would go on passing if the two ever stopped matching.
    sessionLog::install();
    sessionLog::shutdown();
    const QFileInfoList kept
        = QDir(m_dir->path())
              .entryInfoList({ QStringLiteral("session-crash-*.log") }, QDir::Files, QDir::Name);
    QCOMPARE(kept.size(), 1);
    QFile crashed(kept.first().absoluteFilePath());
    QVERIFY(crashed.open(QIODevice::ReadOnly));
    QVERIFY2(QString::fromUtf8(crashed.readAll()).contains(QStringLiteral("about to fall over")),
        "the log set aside is the one that crashed");
}

void TestSessionLog::aCrashStillReachesTheDefaultHandler()
{
    // Swallowing the signal would leave the shell reporting a clean exit and no
    // core file, which is worse than no handler at all.
    const int status = died([] {
        sessionLog::install();
        std::raise(SIGABRT);
    });

    QVERIFY(WIFSIGNALED(status));
    QCOMPARE(WTERMSIG(status), SIGABRT);
    QVERIFY(contents().contains(QStringLiteral("crashed: signal 6")));
}

MOLE_TEST_MAIN(TestSessionLog)
#include "tst_SessionLog.moc"
