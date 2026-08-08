#include "core/terminal/Pty.h"

#include <QDir>
#include <QSocketNotifier>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
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

bool Pty::start(const QString& workingDirectory, const QString& shell, QString* errorOut)
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

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, nullptr);
    if (pid < 0)
        return fail(QStringLiteral("Could not start a terminal: %1").arg(strerror(errno)));

    if (pid == 0) {
        // The child. Nothing here may allocate or touch Qt -- after fork only
        // async-signal-safe calls are defined, and this side is about to be
        // replaced by exec anyway.
        if (!workingDirectory.isEmpty()) {
            const QByteArray path = QDir::toNativeSeparators(workingDirectory).toLocal8Bit();
            if (::chdir(path.constData()) != 0) {
                // Not fatal: a shell in the wrong folder still beats no shell,
                // and the user can see where it landed.
            }
        }

        qputenv("TERM", "xterm-256color");
        const QByteArray shellPath = program.toLocal8Bit();
        ::execl(shellPath.constData(), shellPath.constData(), "-i", nullptr);
        ::_exit(127); // exec only returns on failure
    }

    m_master = master;
    m_child = pid;

    // Non-blocking, because a read on a live terminal with nothing to say would
    // otherwise stop the interface until the user typed something.
    const int flags = ::fcntl(m_master, F_GETFL, 0);
    ::fcntl(m_master, F_SETFL, flags | O_NONBLOCK);

    m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Pty::readReady);
    return true;
}

void Pty::stop()
{
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_child > 0) {
        // SIGHUP rather than SIGKILL: a shell told its terminal has gone away
        // cleans up its children, which SIGKILL would orphan.
        ::kill(static_cast<pid_t>(m_child), SIGHUP);
        int status = 0;
        ::waitpid(static_cast<pid_t>(m_child), &status, WNOHANG);
        m_child = -1;
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
    qint64 written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(m_master, data.constData() + written, data.size() - written);
        if (n <= 0)
            break;
        written += n;
    }
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
            int status = 0;
            const pid_t child = static_cast<pid_t>(m_child);
            stop();
            if (child > 0)
                ::waitpid(child, &status, WNOHANG);
            emit finished(WIFEXITED(status) ? WEXITSTATUS(status) : 0);
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

void Pty::stop() { }
void Pty::write(const QByteArray&) { }
void Pty::resize(int, int) { }
void Pty::readReady() { }

#endif // Q_OS_UNIX

} // namespace mole
