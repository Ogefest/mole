#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringDecoder>

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
    /// Which of the two maintains the screen.
    enum class Emulator {
        /// libvterm where this build has it, the built-in parser otherwise. What
        /// the panel asks for.
        Best,
        /// The built-in parser, whatever this build has.
        ///
        /// For a test, and it earns its place: the fallback is a shipped
        /// configuration -- libvterm is optional and a build without it parses
        /// everything this way -- and with libvterm present it is code no test on
        /// the machine can reach. The faults MOLE-363 found in it include writing
        /// outside the grid from a sequence `cat` of the wrong file produces, so
        /// it has to be reachable whatever the build found.
        BuiltIn,
    };

    explicit TerminalScreen(QObject* parent = nullptr);
    explicit TerminalScreen(Emulator emulator, QObject* parent = nullptr);
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
    /// The cell, or null when there is none there.
    ///
    /// A pointer rather than a reference to a shared scratch cell: the scratch
    /// was `static`, so two screens wrote to the same one, and returning it made
    /// an out-of-range write silently succeed. A caller that cannot see the
    /// difference between a cell and nowhere is a caller that will get it wrong.
    TerminalCell* cellAt(int row, int column);
    /// Whether the built-in parser is the one running. False only when this build
    /// has libvterm *and* Emulator::Best was asked for.
    bool usingBuiltInParser() const;
#ifdef MOLE_HAVE_VTERM
    void readBackFromVterm();
#endif

    Emulator m_emulator = Emulator::Best;
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
    ///
    /// `SwallowOne` is for the two-byte designators -- `ESC ( B`, which `tput
    /// sgr0` emits and therefore most prompts do -- whose second byte used to
    /// fall through to Ground and print. `SwallowOne` eats it.
    enum class State { Ground, Escape, SwallowOne, ControlSequence, OperatingSystemCommand };
    State m_state = State::Ground;
    QByteArray m_pending;
    /// Decoded incrementally, because a multi-byte character can arrive split
    /// across two reads. A member and not `static thread_local`: two screens on
    /// one thread shared the partial state and corrupted each other's, and a new
    /// shell inherited whatever the last one left half-decoded.
    QStringDecoder m_decoder { QStringDecoder::Utf8 };
    QString m_title;
    bool m_sawUnsupported = false;
};

} // namespace mole
