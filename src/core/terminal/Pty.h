#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

class QSocketNotifier;

namespace mole {

/// A shell running on a pseudo-terminal.
///
/// A pty rather than a plain pipe, because a shell behaves completely
/// differently without one: no prompt, no job control, no colour, and line
/// buffering that makes interactive use impossible. Programs ask whether their
/// output is a terminal and act on the answer.
///
/// Reading is driven by a socket notifier on the master descriptor, so nothing
/// polls and nothing blocks the interface.
class Pty : public QObject
{
    Q_OBJECT

public:
    /// What to start, beyond the shell itself.
    struct Options
    {
        /// Arguments instead of the default `-i`. Empty means `-i`.
        ///
        /// Here so the panel and the screenshot harness can share one shape. The
        /// harness used to install a wrapper script because "the panel passes
        /// only -i and there is no way to add --norc from here", which is a
        /// second way of starting a shell that behaves differently from the
        /// first. See MOLE-363.
        QStringList arguments;
        /// Added to the environment the child gets, as `NAME=value`. `TERM` is
        /// set for you and can be overridden here.
        QStringList environment;
    };

    explicit Pty(QObject* parent = nullptr);
    ~Pty() override;

    /// Starts `shell` (or the user's own when empty) in `workingDirectory`.
    bool start(const QString& workingDirectory, const QString& shell = {}, QString* errorOut = nullptr);
    bool start(
        const QString& workingDirectory, const QString& shell, const Options& options, QString* errorOut);
    bool isRunning() const { return m_master >= 0; }
    void stop();

    /// What the shell exited with, once it has. -1 before that.
    ///
    /// 128 + the signal number for a shell that was killed, which is the
    /// convention every shell already uses for its own children.
    int exitCode() const { return m_exitCode; }

    /// Queues `data` and writes what the terminal will take now.
    ///
    /// The master is O_NONBLOCK, so a large write returns short. It used to break
    /// out of its loop on the first EAGAIN and discard the rest, so a paste beyond
    /// a few kilobytes arrived truncated -- and a truncated paste into a shell is
    /// a command nobody typed. The tail waits for the descriptor to be writable.
    void write(const QByteArray& data);
    /// Tells the child its window changed. Without this, anything that draws a
    /// full screen wraps at the wrong column.
    void resize(int columns, int rows);

signals:
    void output(const QByteArray& data);
    /// The shell exited; the panel shows this rather than appearing to hang.
    void finished(int exitCode);

private:
    void readReady();
    void writeReady();
    /// Waits for the child and answers what it exited with. Called once.
    ///
    /// One place, because there were two and they disagreed. stop() sent SIGHUP
    /// and did a single waitpid(WNOHANG) into a status it threw away; readReady()
    /// then waited a second time on EOF, got ECHILD from a child the first call
    /// had already reaped, left status at 0, and reported "exited with code 0"
    /// for a shell that died with 127. When the first call was too early instead,
    /// nothing ever waited again and the child stayed a zombie for the life of the
    /// process -- one per panel anybody closed. See MOLE-363.
    ///
    /// `graceMs` is how long a child that has been asked to go is given before it
    /// is killed. Bounded and short: this runs on the thread that draws.
    int reapChild(int graceMs);

    int m_master = -1;
    qint64 m_child = -1;
    int m_exitCode = -1;
    QSocketNotifier* m_notifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    QByteArray m_outgoing;
};

} // namespace mole
