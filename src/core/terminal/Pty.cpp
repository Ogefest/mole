#include "core/terminal/Pty.h"

#include <QDir>
#include <QSocketNotifier>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;
#endif

namespace mole {

Pty::Pty(QObject* parent)
    : QObject(parent)
{
}

Pty::~Pty()
{
    stop();
}

#ifdef Q_OS_UNIX

namespace {

    /// How long a shell asked to go is given before it is killed.
    ///
    /// Short and bounded, because this is waited out on the thread that draws.
    /// A shell told its terminal has gone away exits at once; anything that does
    /// not was not going to.
    constexpr int kHangUpGraceMs = 200;
    /// And after the pty has closed, which means the child is already on its way
    /// out -- this is the gap between its last write and its exit status.
    constexpr int kExitGraceMs = 500;

} // namespace

bool Pty::start(const QString& workingDirectory, const QString& shell, QString* errorOut)
{
    return start(workingDirectory, shell, Options {}, errorOut);
}

bool Pty::start(
    const QString& workingDirectory, const QString& shell, const Options& options, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    if (isRunning())
        return fail(QStringLiteral("Already running"));

    QString program = shell;
    if (program.isEmpty())
        program = QString::fromLocal8Bit(qgetenv("SHELL"));
    if (program.isEmpty())
        program = QStringLiteral("/bin/sh");

    // **Everything the child needs is built here, before the fork.** The child
    // side used to call QDir::toNativeSeparators().toLocal8Bit(), qputenv() --
    // which is setenv, which mallocs -- and program.toLocal8Bit(). Mole is
    // multi-threaded by then, and after fork() only the forking thread exists:
    // a malloc lock another thread happened to be holding at that instant is
    // held for ever in the child, which deadlocks before it can exec. A panel
    // that never prints a prompt, once in a while, for no visible reason.
    // See MOLE-363.
    //
    // What is left in the child is chdir and execve, both async-signal-safe.
    const QByteArray directory = workingDirectory.isEmpty()
        ? QByteArray()
        : QDir::toNativeSeparators(workingDirectory).toLocal8Bit();
    const QByteArray shellPath = program.toLocal8Bit();

    QList<QByteArray> argumentBytes { shellPath };
    if (options.arguments.isEmpty()) {
        argumentBytes.append(QByteArrayLiteral("-i"));
    } else {
        for (const QString& argument : options.arguments)
            argumentBytes.append(argument.toLocal8Bit());
    }
    std::vector<char*> argv;
    argv.reserve(argumentBytes.size() + 1);
    for (QByteArray& argument : argumentBytes)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    // TERM first, so a caller may override it, and the process environment after
    // it -- which is what the shell inherited before this and still does.
    QList<QByteArray> environmentBytes { QByteArrayLiteral("TERM=xterm-256color") };
    for (const QString& entry : options.environment)
        environmentBytes.append(entry.toLocal8Bit());
    for (char** entry = environ; entry && *entry; ++entry) {
        const QByteArray existing(*entry);
        const int equals = existing.indexOf('=');
        const QByteArray name = equals > 0 ? existing.left(equals + 1) : QByteArray();
        bool overridden = false;
        for (const QByteArray& already : environmentBytes) {
            if (!name.isEmpty() && already.startsWith(name)) {
                overridden = true;
                break;
            }
        }
        if (!overridden)
            environmentBytes.append(existing);
    }
    std::vector<char*> envp;
    envp.reserve(environmentBytes.size() + 1);
    for (QByteArray& entry : environmentBytes)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
    if (pid < 0)
        return fail(QStringLiteral("Could not start a terminal: %1").arg(strerror(errno)));

    if (pid == 0) {
        // The child. Nothing here allocates or touches Qt -- after fork only
        // async-signal-safe calls are defined, and this side is about to be
        // replaced by exec anyway.
        if (!directory.isEmpty()) {
            if (::chdir(directory.constData()) != 0) {
                // Not fatal: a shell in the wrong folder still beats no shell,
                // and the user can see where it landed.
            }
        }
        ::execve(shellPath.constData(), argv.data(), envp.data());
        ::_exit(127); // exec only returns on failure
    }

    m_master = master;
    m_child = pid;
    m_exitCode = -1;
    m_outgoing.clear();

    // Non-blocking, because a read on a live terminal with nothing to say would
    // otherwise stop the interface until the user typed something.
    const int flags = ::fcntl(m_master, F_GETFL, 0);
    ::fcntl(m_master, F_SETFL, flags | O_NONBLOCK);

    m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Pty::readReady);
    m_writeNotifier = new QSocketNotifier(m_master, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false); // only while something is queued
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &Pty::writeReady);
    return true;
}

int Pty::reapChild(int graceMs)
{
    if (m_child <= 0)
        return m_exitCode;

    const pid_t child = static_cast<pid_t>(m_child);
    m_child = -1;

    int status = 0;
    // Polled rather than waited on outright, so a shell that ignores the hang-up
    // cannot hold the interface. 5 ms a turn: a shell that is going goes on the
    // first or second.
    for (int waited = 0; waited <= graceMs; waited += 5) {
        const pid_t answered = ::waitpid(child, &status, WNOHANG);
        if (answered == child)
            return m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (answered < 0)
            return m_exitCode; // already reaped, or never ours: nothing to learn
        if (waited < graceMs)
            ::usleep(5000);
    }

    // It is not going on its own. SIGKILL cannot be ignored, so this wait ends.
    ::kill(child, SIGKILL);
    if (::waitpid(child, &status, 0) == child)
        return m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return m_exitCode;
}

void Pty::stop()
{
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(false);
        m_writeNotifier->deleteLater();
        m_writeNotifier = nullptr;
    }
    m_outgoing.clear();
    if (m_child > 0) {
        // SIGHUP rather than SIGKILL: a shell told its terminal has gone away
        // cleans up its children, which SIGKILL would orphan. reapChild() escalates
        // if it does not go.
        ::kill(static_cast<pid_t>(m_child), SIGHUP);
        reapChild(kHangUpGraceMs);
    }
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
}

void Pty::write(const QByteArray& data)
{
    if (m_master < 0 || data.isEmpty())
        return;
    m_outgoing.append(data);
    writeReady();
}

void Pty::writeReady()
{
    if (m_master < 0)
        return;

    while (!m_outgoing.isEmpty()) {
        const ssize_t n = ::write(m_master, m_outgoing.constData(), m_outgoing.size());
        if (n > 0) {
            m_outgoing.remove(0, static_cast<int>(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        // The terminal will take no more for now. What is left waits for the
        // descriptor to say it is writable, which is what stopped a long paste
        // being cut off at the first EAGAIN.
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        // A real error: there is nobody to write to any more.
        m_outgoing.clear();
        break;
    }

    if (m_writeNotifier)
        m_writeNotifier->setEnabled(!m_outgoing.isEmpty());
}

void Pty::resize(int columns, int rows)
{
    if (m_master < 0)
        return;
    winsize size {};
    size.ws_col = static_cast<unsigned short>(std::max(1, columns));
    size.ws_row = static_cast<unsigned short>(std::max(1, rows));
    ::ioctl(m_master, TIOCSWINSZ, &size);
}

void Pty::readReady()
{
    if (m_master < 0)
        return;

    QByteArray buffer;
    char chunk[8192];
    while (true) {
        const ssize_t n = ::read(m_master, chunk, sizeof(chunk));
        if (n > 0) {
            buffer.append(chunk, static_cast<int>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        // Zero or a real error means the far end has gone.
        if (n == 0 || (n < 0 && errno != EINTR)) {
            if (!buffer.isEmpty())
                emit output(buffer);
            // Reaped here and nowhere else, before stop() takes the pid away. The
            // pty has closed, so the child is already exiting; the grace is the
            // gap between its last write and its status.
            const int code = reapChild(kExitGraceMs);
            stop();
            emit finished(code < 0 ? 0 : code);
            return;
        }
    }

    if (!buffer.isEmpty())
        emit output(buffer);
}

#else // Q_OS_UNIX

// Windows needs ConPTY, which is a different API entirely. The panel reports
// itself unavailable rather than pretending, exactly as the Parquet viewer does
// without Arrow.
bool Pty::start(const QString&, const QString&, QString* errorOut)
{
    if (errorOut)
        *errorOut = QStringLiteral("A terminal is not available on this platform yet");
    return false;
}

bool Pty::start(const QString&, const QString&, const Options&, QString* errorOut)
{
    if (errorOut)
        *errorOut = QStringLiteral("A terminal is not available on this platform yet");
    return false;
}

void Pty::stop() { }
void Pty::write(const QByteArray&) { }
void Pty::resize(int, int) { }
void Pty::readReady() { }
void Pty::writeReady() { }
int Pty::reapChild(int)
{
    return m_exitCode;
}

#endif // Q_OS_UNIX

} // namespace mole
