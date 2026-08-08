#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <memory>

namespace mole {

/// A cell on the screen: a character and how it is drawn.
struct TerminalCell
{
    QChar character = QLatin1Char(' ');
    /// Indices into the 256-colour palette, or -1 for the default.
    int foreground = -1;
    int background = -1;
    bool bold = false;
    bool inverse = false;
};

/// A terminal's screen, and whatever maintains it.
///
/// Two implementations behind one interface. With libvterm available it does the
/// parsing -- it is a complete terminal emulator, so full-screen programs work
/// properly rather than approximately. Without it there is a built-in parser
/// covering printable text, the control characters a shell relies on, cursor
/// movement, erasing and SGR colour: enough for the common use of this panel,
/// which is running a command in the folder you are looking at and reading what
/// it says.
///
/// The fallback is honest about its limits. `sawUnsupported()` becomes true the
/// moment something arrives that it cannot draw, so the panel can say so instead
/// of showing a screen that is subtly wrong.
class TerminalScreen : public QObject
{
    Q_OBJECT

public:
    explicit TerminalScreen(QObject* parent = nullptr);
    /// Out of line, because the emulator it holds is only a forward declaration
    /// here -- destroying it needs the definition.
    ~TerminalScreen() override;

    void resize(int columns, int rows);
    int columns() const { return m_columns; }
    int rows() const { return m_rows; }

    /// Feeds output from the shell.
    void feed(const QByteArray& data);
    void clear();

    /// One row, for the view. Row 0 is the top of the visible screen.
    QList<TerminalCell> row(int index) const;
    /// The row as plain text, for tests and for copying.
    QString rowText(int index) const;

    int cursorRow() const { return m_cursorRow; }
    int cursorColumn() const { return m_cursorColumn; }

    /// What the shell asked the window to be called, when it said.
    QString title() const { return m_title; }

    /// True once something arrived that this build cannot draw. Always false
    /// with libvterm, which handles everything a terminal can be sent.
    bool sawUnsupported() const { return m_sawUnsupported; }
    /// Whether this build has a full terminal emulator behind it.
    static bool isComplete();

    /// Called by the emulator when the shell renames its window. Public because
    /// libvterm reaches it through a C callback, which cannot be a friend.
    void setTitleFromTerminal(const QString& title);

    /// Keys the view sends back to the shell, encoded as the terminal expects.
    /// Belongs here because the encoding is the emulator's business: what a
    /// cursor key sends depends on modes only it knows about.
    static QByteArray encodeKey(int key, int modifiers, const QString& text);

signals:
    void changed();
    void titleChanged();

private:
    void putCharacter(QChar c);
    void newline();
    void carriageReturn();
    void backspace();
    void tab();
    void scrollUp();
    void handleControlSequence(char final, const QList<int>& parameters, bool question);
    void applyGraphicRendition(const QList<int>& parameters);
    void eraseInDisplay(int mode);
    void eraseInLine(int mode);
    TerminalCell& cellAt(int row, int column);
#ifdef MOLE_HAVE_VTERM
    void readBackFromVterm();
#endif

    int m_columns = 80;
    int m_rows = 24;
    QList<QList<TerminalCell>> m_grid;

#ifdef MOLE_HAVE_VTERM
    struct Vterm;
    std::unique_ptr<Vterm> m_vterm;
#endif

    int m_cursorRow = 0;
    int m_cursorColumn = 0;
    int m_savedRow = 0;
    int m_savedColumn = 0;
    TerminalCell m_pen;

    /// Parser state. A sequence can arrive split across reads, so this survives
    /// between calls to feed().
    enum class State { Ground, Escape, ControlSequence, OperatingSystemCommand };
    State m_state = State::Ground;
    QByteArray m_pending;
    QString m_title;
    bool m_sawUnsupported = false;
};

} // namespace mole
