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
