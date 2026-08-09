#include "app/SessionLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>

#include <atomic>
#include <memory>

#if defined(__unix__)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif

namespace mole::sessionLog {
namespace {

    QMutex g_mutex;
    std::unique_ptr<QFile> g_file;
    QtMessageHandler g_previous = nullptr;
    /// Read by the crash handler, which cannot take a lock or touch a QFile.
    std::atomic_int g_logDescriptor { -1 };

    const char* levelName(QtMsgType type)
    {
        switch (type) {
        case QtDebugMsg:
            return "debug";
        case QtInfoMsg:
            return "info";
        case QtWarningMsg:
            return "warning";
        case QtCriticalMsg:
            return "critical";
        case QtFatalMsg:
            return "fatal";
        }
        return "message";
    }

    void handler(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        // The console keeps behaving exactly as it did. The file is an addition,
        // not a replacement -- a log nobody sees is worse than the terminal.
        if (g_previous)
            g_previous(type, context, message);

        QMutexLocker locker(&g_mutex);
        if (!g_file)
            return;

        QString line = QStringLiteral("%1 %2  %3")
                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                               QString::fromLatin1(levelName(type)).leftJustified(8), message);

        // Where it came from, when the message did not say so itself. QML warnings
        // carry their own file and line in the text; C++ ones do not.
        if (context.file && *context.file && !message.contains(QLatin1String(".qml:")))
            line += QStringLiteral("   [%1:%2]").arg(QString::fromUtf8(context.file)).arg(context.line);

        QTextStream stream(g_file.get());
        stream << line << '\n';
        stream.flush();
        // Flushed to the file, then to the operating system. A crash a microsecond
        // later still leaves the line on disk, which is the entire point.
        g_file->flush();
    }

#if defined(__unix__)

    /// Formats a number into `buffer` without allocating or calling into stdio,
    /// neither of which is safe from a signal handler. Returns how many characters
    /// it wrote.
    int formatNumber(unsigned long long value, char* buffer, int base)
    {
        static const char alphabet[] = "0123456789abcdef";
        char reversed[32];
        int length = 0;
        do {
            reversed[length++] = alphabet[value % static_cast<unsigned>(base)];
            value /= static_cast<unsigned>(base);
        } while (value != 0 && length < 32);

        for (int i = 0; i < length; ++i)
            buffer[i] = reversed[length - 1 - i];
        return length;
    }

    void writeText(int fd, const char* text, size_t length)
    {
        // Return value ignored deliberately: there is nothing useful to do about a
        // failed write from inside a crash, and the process is about to die anyway.
        const ssize_t ignored = write(fd, text, length);
        (void)ignored;
    }

    /// Runs inside a signal handler, so nothing here may allocate, lock or call
    /// into Qt -- all three can deadlock or fault a second time in a process that
    /// is already broken. Only write() and backtrace_symbols_fd(), both of which
    /// are safe to call here, and both of which go straight to a descriptor.
    void crashHandler(int number, siginfo_t* info, void*)
    {
        void* frames[64];
        const int count = backtrace(frames, 64);

        char line[160];
        int length = 0;
        const auto append = [&line, &length](const char* text) {
            for (const char* c = text; *c && length < 150; ++c)
                line[length++] = *c;
        };

        append("\n---- crashed: signal ");
        length += formatNumber(static_cast<unsigned>(number), line + length, 10);

        // The faulting address is worth more than the signal number when the stack
        // cannot be read: a zero there says a null pointer was followed, and a wild
        // value says something else entirely.
        if (info) {
            append(" at 0x");
            length += formatNumber(reinterpret_cast<unsigned long long>(info->si_addr), line + length, 16);
            append(" code ");
            length += formatNumber(static_cast<unsigned>(info->si_code), line + length, 10);
        }
        append(" ----\nbacktrace:\n");

        const int descriptors[] = { STDERR_FILENO, g_logDescriptor.load() };
        for (int fd : descriptors) {
            if (fd < 0)
                continue;
            writeText(fd, line, static_cast<size_t>(length));
            backtrace_symbols_fd(frames, count, fd);
            // A stack of two or three frames means the fault arrived through
            // another runtime's signal forwarding, which this cannot unwind past.
            // Say so, rather than leaving a stack that merely looks short.
            if (count <= 3) {
                static const char note[] = "  (stack unreadable from here -- rerun under "
                                           "`make run-gdb` for a full one)\n";
                writeText(fd, note, sizeof(note) - 1);
            }
            fsync(fd);
        }

        // Back to the default and round again, so the shell still reports a crash
        // and any core pattern still gets its core.
        signal(number, SIG_DFL);
        raise(number);
    }

    void installCrashHandler()
    {
        struct sigaction action;
        sigemptyset(&action.sa_mask);
        action.sa_sigaction = crashHandler;
        // SA_ONSTACK asks for the handler to run on its own signal stack. That
        // matters when the crash is a stack overflow, and it matters whenever a
        // library in the process has installed a small alternate stack of its own:
        // without the flag the handler can fault a second time, turning a
        // diagnosable crash into a silent one. SA_SIGINFO carries the faulting
        // address.
        action.sa_flags = SA_ONSTACK | SA_SIGINFO;

        for (int number : { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL })
            sigaction(number, &action, nullptr);
    }

#else
    void installCrashHandler() { }
#endif

} // namespace

QString defaultPath()
{
    const QByteArray override = qgetenv("MOLE_LOG_PATH");
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("session.log"));
}

QString install()
{
    QMutexLocker locker(&g_mutex);
    if (g_file)
        return g_file->fileName();

    const QString path = defaultPath();
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
        return {};

    // Keep exactly one previous run. The way anybody notices a crash is by
    // starting the program again, and that restart is what would otherwise
    // destroy the log that explains it.
    const QString previous = path + QStringLiteral(".1");
    QFile::remove(previous);
    QFile::rename(path, previous);

    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Text))
        return {};

    g_file = std::move(file);

    QTextStream stream(g_file.get());
    stream << QStringLiteral("---- %1  %2 ----\n")
                  .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                      QCoreApplication::applicationFilePath());
    stream.flush();
    g_file->flush();

    g_logDescriptor.store(g_file->handle());
    installCrashHandler();

    g_previous = qInstallMessageHandler(handler);
    return path;
}

void shutdown()
{
    QtMessageHandler previous = nullptr;
    {
        QMutexLocker locker(&g_mutex);
        if (!g_file)
            return;
        previous = g_previous;
        g_previous = nullptr;
        g_logDescriptor.store(-1);
        g_file->close();
        g_file.reset();
    }
    // Restored outside the lock: the handler takes the same lock.
    qInstallMessageHandler(previous);
}

} // namespace mole::sessionLog
