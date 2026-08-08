#pragma once

#include <QObject>
#include <QString>

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
    explicit Pty(QObject* parent = nullptr);
    ~Pty() override;

    /// Starts `shell` (or the user's own when empty) in `workingDirectory`.
    bool start(const QString& workingDirectory, const QString& shell = {}, QString* errorOut = nullptr);
    bool isRunning() const { return m_master >= 0; }
    void stop();

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

    int m_master = -1;
    qint64 m_child = -1;
    QSocketNotifier* m_notifier = nullptr;
};

} // namespace mole
