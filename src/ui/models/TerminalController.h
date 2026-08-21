#pragma once

#include "core/terminal/Pty.h"
#include "core/terminal/TerminalScreen.h"

#include <QObject>
#include <QStringList>
#include <QVariantList>

namespace mole {

/// The terminal panel: a shell for the folder you are looking at.
///
/// Part of the shell rather than a tab, because that is what it is for -- you
/// run a command *here*, in the folder in front of you, and read what it says
/// without leaving the listing.
class TerminalController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool visible READ isVisible WRITE setVisible NOTIFY visibleChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(bool available READ isAvailable CONSTANT)
    Q_PROPERTY(bool complete READ isComplete CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY screenChanged)
    Q_PROPERTY(QString workingDirectory READ workingDirectory NOTIFY runningChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY runningChanged)
    Q_PROPERTY(int rows READ rows NOTIFY screenChanged)
    Q_PROPERTY(int columns READ columns NOTIFY screenChanged)
    Q_PROPERTY(int cursorRow READ cursorRow NOTIFY screenChanged)
    Q_PROPERTY(int cursorColumn READ cursorColumn NOTIFY screenChanged)
    /// The whole screen, as rows of styled runs.
    ///
    /// A property rather than a method, because a method call in a binding has
    /// no change signal: QML would evaluate it once and never again, and the
    /// panel would stay blank while the shell talked to it.
    Q_PROPERTY(QVariantList screenRows READ screenRows NOTIFY screenChanged)
    /// The sixteen colours a terminal refers to by index.
    ///
    /// Here rather than in the panel's QML because it is the terminal's own data
    /// rather than the window's paint: a theme has no business deciding what
    /// `\033[31m` looks like, any more than it decides what a keyword in a source
    /// file looks like. It lives next to the escape parser that produces the
    /// indices. See ADR-0072.
    Q_PROPERTY(QStringList ansiPalette READ ansiPalette CONSTANT)

public:
    explicit TerminalController(QObject* parent = nullptr);

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible);
    bool isRunning() const { return m_pty.isRunning(); }
    /// False on a platform with no pseudo-terminal, so the panel can say so
    /// rather than opening onto nothing.
    bool isAvailable() const;
    bool isComplete() const { return TerminalScreen::isComplete(); }

    QString title() const { return m_screen.title(); }
    QString workingDirectory() const { return m_workingDirectory; }
    QString errorText() const { return m_errorText; }

    int rows() const { return m_screen.rows(); }
    int columns() const { return m_screen.columns(); }
    int cursorRow() const { return m_screen.cursorRow(); }
    int cursorColumn() const { return m_screen.cursorColumn(); }

    /// One row as {text, spans} for the view. Colour is carried as runs rather
    /// than per character, because a row of eighty separate items would cost
    /// more to lay out than the whole panel is worth.
    QVariantList screenRows() const;
    static QStringList ansiPalette();
    Q_INVOKABLE QVariantList rowSpans(int index) const;
    Q_INVOKABLE QString rowText(int index) const;

    /// Starts a shell in `directory`, or restarts one that has exited.
    Q_INVOKABLE void open(const QString& directory);
    Q_INVOKABLE void close();
    Q_INVOKABLE void sendKey(int key, int modifiers, const QString& text);
    Q_INVOKABLE void sendText(const QString& text);
    Q_INVOKABLE void setSize(int columns, int rows);
    /// Changes the shell's own directory by typing a cd into it -- the only way
    /// to move a running shell, since its cwd is its own business.
    Q_INVOKABLE void followTo(const QString& directory);

signals:
    void visibleChanged();
    void runningChanged();
    void screenChanged();

private:
    Pty m_pty;
    TerminalScreen m_screen;
    bool m_visible = false;
    QString m_workingDirectory;
    QString m_errorText;
};

} // namespace mole
