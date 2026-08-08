#include "core/terminal/TerminalScreen.h"

#include <QStringDecoder>

#ifdef MOLE_HAVE_VTERM
#include <vterm.h>
#endif

namespace mole {

bool TerminalScreen::isComplete()
{
#ifdef MOLE_HAVE_VTERM
    return true;
#else
    return false;
#endif
}

#ifdef MOLE_HAVE_VTERM
namespace {

    int onTermProp(VTermProp property, VTermValue* value, void* user)
    {
        auto* screen = static_cast<TerminalScreen*>(user);
        if (!screen || property != VTERM_PROP_TITLE || !value)
            return 0;
        screen->setTitleFromTerminal(
            QString::fromUtf8(value->string.str, static_cast<int>(value->string.len)));
        return 1;
    }

} // namespace
#endif

QByteArray TerminalScreen::encodeKey(int key, int modifiers, const QString& text)
{
    // The encodings every terminal agrees on. Kept here rather than in the view
    // because what a key sends is the emulator's business, not the widget's.
    const bool control = (modifiers & 0x04000000) != 0; // Qt::ControlModifier
    const bool alt = (modifiers & 0x08000000) != 0; // Qt::AltModifier

    QByteArray out;
    switch (key) {
    case 0x01000000: // Escape
        out = "\x1b";
        break;
    case 0x01000004: // Return
    case 0x01000005: // Enter
        out = "\r";
        break;
    case 0x01000001: // Tab
        out = "\t";
        break;
    case 0x01000003: // Backspace
        out = "\x7f";
        break;
    case 0x01000013: // Up
        out = "\x1b[A";
        break;
    case 0x01000015: // Down
        out = "\x1b[B";
        break;
    case 0x01000014: // Right
        out = "\x1b[C";
        break;
    case 0x01000012: // Left
        out = "\x1b[D";
        break;
    case 0x01000010: // Home
        out = "\x1b[H";
        break;
    case 0x01000011: // End
        out = "\x1b[F";
        break;
    case 0x01000016: // PageUp
        out = "\x1b[5~";
        break;
    case 0x01000017: // PageDown
        out = "\x1b[6~";
        break;
    case 0x01000007: // Delete
        out = "\x1b[3~";
        break;
    default:
        break;
    }

    if (!out.isEmpty())
        return out;

    if (control && key >= 0x40 && key <= 0x5f) {
        // Ctrl+A..Ctrl+_ are the control characters, which is how Ctrl+C
        // reaches the shell as an interrupt rather than as a letter.
        out.append(static_cast<char>(key - 0x40));
        return out;
    }

    if (text.isEmpty())
        return out;

    if (alt)
        out.append('\x1b'); // Meta is an escape prefix
    out.append(text.toUtf8());
    return out;
}

#ifdef MOLE_HAVE_VTERM

/// Holds the emulator and keeps its screen in sync with ours. libvterm owns the
/// authoritative state; this reads it back after each feed rather than trying to
/// mirror every callback, which is far simpler and just as correct.
struct TerminalScreen::Vterm
{
    VTerm* term = nullptr;
    VTermScreen* screen = nullptr;
    VTermScreenCallbacks callbacks {};
    /// Bytes of an incomplete UTF-8 sequence held back from the last write. A
    /// read boundary can fall inside a character, and handing libvterm half of
    /// one produces a replacement character it can never take back.
    QByteArray partial;

    ~Vterm()
    {
        if (term)
            vterm_free(term);
    }
};

namespace {

    /// How many bytes a UTF-8 sequence beginning with this byte needs.
    int utf8SequenceLength(unsigned char lead)
    {
        if ((lead & 0x80) == 0)
            return 1;
        if ((lead & 0xe0) == 0xc0)
            return 2;
        if ((lead & 0xf0) == 0xe0)
            return 3;
        if ((lead & 0xf8) == 0xf0)
            return 4;
        return 1; // a stray continuation byte; pass it through and let vterm decide
    }

    /// Splits off a trailing incomplete character, if there is one.
    QByteArray takeIncompleteTail(QByteArray& data)
    {
        // At most three bytes can be missing, so only the tail needs looking at.
        for (int back = 1; back <= 3 && back <= data.size(); ++back) {
            const auto lead = static_cast<unsigned char>(data.at(data.size() - back));
            if ((lead & 0xc0) == 0x80)
                continue; // a continuation byte; keep walking back to the lead
            const int needed = utf8SequenceLength(lead);
            if (needed > back) {
                QByteArray tail = data.right(back);
                data.chop(back);
                return tail;
            }
            break;
        }
        return {};
    }

    int onTermProp(VTermProp property, VTermValue* value, void* user);

} // namespace

#endif

TerminalScreen::TerminalScreen(QObject* parent)
    : QObject(parent)
{
    resize(m_columns, m_rows);
}

TerminalScreen::~TerminalScreen() = default;

void TerminalScreen::setTitleFromTerminal(const QString& title)
{
    if (m_title == title)
        return;
    m_title = title;
    emit titleChanged();
}

void TerminalScreen::resize(int columns, int rows)
{
    m_columns = std::max(1, columns);
    m_rows = std::max(1, rows);

    m_grid.resize(m_rows);
    for (QList<TerminalCell>& line : m_grid)
        line.resize(m_columns);

#ifdef MOLE_HAVE_VTERM
    if (!m_vterm) {
        m_vterm = std::make_unique<Vterm>();
        m_vterm->term = vterm_new(m_rows, m_columns);
        vterm_set_utf8(m_vterm->term, 1);
        m_vterm->screen = vterm_obtain_screen(m_vterm->term);

        // Only the properties worth acting on. The screen itself is read back
        // wholesale after each write, so damage callbacks are not needed.
        m_vterm->callbacks.settermprop = &onTermProp;
        vterm_screen_set_callbacks(m_vterm->screen, &m_vterm->callbacks, this);

        // Without this libvterm keeps one buffer and a full-screen program
        // overwrites the scrollback it was supposed to be layered over -- so
        // leaving an editor would lose everything that came before it.
        vterm_screen_enable_altscreen(m_vterm->screen, 1);
        vterm_screen_reset(m_vterm->screen, 1);
    } else {
        vterm_set_size(m_vterm->term, m_rows, m_columns);
    }
#endif

    m_cursorRow = std::min(m_cursorRow, m_rows - 1);
    m_cursorColumn = std::min(m_cursorColumn, m_columns - 1);
    emit changed();
}

#ifdef MOLE_HAVE_VTERM

/// Copies libvterm's screen into ours. Reading the whole grid after each feed
/// costs a few thousand cells and removes every opportunity for the two to
/// disagree -- which chasing damage callbacks would not.
void TerminalScreen::readBackFromVterm()
{
    for (int row = 0; row < m_rows; ++row) {
        for (int column = 0; column < m_columns; ++column) {
            VTermScreenCell source {};
            const VTermPos position { row, column };
            vterm_screen_get_cell(m_vterm->screen, position, &source);

            TerminalCell& target = cellAt(row, column);
            target.character
                = source.chars[0] != 0 ? QChar(static_cast<char32_t>(source.chars[0])) : QLatin1Char(' ');
            target.bold = source.attrs.bold != 0;
            target.inverse = source.attrs.reverse != 0;

            // Only indexed colours are carried; a true-colour cell keeps the
            // default rather than being approximated to the nearest of 256.
            target.foreground = VTERM_COLOR_IS_INDEXED(&source.fg) ? source.fg.indexed.idx : -1;
            target.background = VTERM_COLOR_IS_INDEXED(&source.bg) ? source.bg.indexed.idx : -1;
        }
    }

    VTermPos cursor {};
    VTermState* state = vterm_obtain_state(m_vterm->term);
    vterm_state_get_cursorpos(state, &cursor);
    m_cursorRow = std::clamp(cursor.row, 0, m_rows - 1);
    m_cursorColumn = std::clamp(cursor.col, 0, m_columns - 1);
}

#endif

void TerminalScreen::clear()
{
    for (QList<TerminalCell>& line : m_grid)
        line.fill(TerminalCell {});
    m_cursorRow = 0;
    m_cursorColumn = 0;
    emit changed();
}

TerminalCell& TerminalScreen::cellAt(int row, int column)
{
    static TerminalCell scratch;
    if (row < 0 || row >= m_grid.size())
        return scratch;
    QList<TerminalCell>& line = m_grid[row];
    if (column < 0 || column >= line.size())
        return scratch;
    return line[column];
}

QList<TerminalCell> TerminalScreen::row(int index) const
{
    return index >= 0 && index < m_grid.size() ? m_grid.at(index) : QList<TerminalCell> {};
}

QString TerminalScreen::rowText(int index) const
{
    QString text;
    const QList<TerminalCell> line = row(index);
    for (const TerminalCell& cell : line)
        text.append(cell.character);
    // Trailing blanks are padding, not content, and keeping them makes every
    // comparison and every copy wrong by the width of the screen.
    while (text.endsWith(QLatin1Char(' ')))
        text.chop(1);
    return text;
}

void TerminalScreen::scrollUp()
{
    m_grid.removeFirst();
    QList<TerminalCell> blank;
    blank.resize(m_columns);
    m_grid.append(blank);
}

void TerminalScreen::newline()
{
    ++m_cursorRow;
    if (m_cursorRow >= m_rows) {
        scrollUp();
        m_cursorRow = m_rows - 1;
    }
}

void TerminalScreen::carriageReturn()
{
    m_cursorColumn = 0;
}

void TerminalScreen::backspace()
{
    if (m_cursorColumn > 0)
        --m_cursorColumn;
}

void TerminalScreen::tab()
{
    // Eight-column stops, which is what every shell assumes.
    m_cursorColumn = std::min(m_columns - 1, (m_cursorColumn / 8 + 1) * 8);
}

void TerminalScreen::putCharacter(QChar c)
{
    if (m_cursorColumn >= m_columns) {
        carriageReturn();
        newline();
    }
    TerminalCell& cell = cellAt(m_cursorRow, m_cursorColumn);
    cell.character = c;
    cell.foreground = m_pen.foreground;
    cell.background = m_pen.background;
    cell.bold = m_pen.bold;
    cell.inverse = m_pen.inverse;
    ++m_cursorColumn;
}

void TerminalScreen::eraseInLine(int mode)
{
    QList<TerminalCell>& line = m_grid[m_cursorRow];
    const int from = mode == 0 ? m_cursorColumn : 0;
    const int to = mode == 1 ? m_cursorColumn : m_columns - 1;
    for (int i = from; i <= to && i < line.size(); ++i)
        line[i] = TerminalCell {};
}

void TerminalScreen::eraseInDisplay(int mode)
{
    if (mode == 2 || mode == 3) {
        clear();
        return;
    }
    const int from = mode == 0 ? m_cursorRow : 0;
    const int to = mode == 1 ? m_cursorRow : m_rows - 1;
    for (int i = from; i <= to && i < m_grid.size(); ++i)
        m_grid[i].fill(TerminalCell {});
    eraseInLine(mode);
}

void TerminalScreen::applyGraphicRendition(const QList<int>& parameters)
{
    if (parameters.isEmpty()) {
        m_pen = TerminalCell {};
        return;
    }

    for (int i = 0; i < parameters.size(); ++i) {
        const int code = parameters.at(i);
        if (code == 0) {
            m_pen = TerminalCell {};
        } else if (code == 1) {
            m_pen.bold = true;
        } else if (code == 7) {
            m_pen.inverse = true;
        } else if (code == 22) {
            m_pen.bold = false;
        } else if (code == 27) {
            m_pen.inverse = false;
        } else if (code >= 30 && code <= 37) {
            m_pen.foreground = code - 30;
        } else if (code >= 90 && code <= 97) {
            m_pen.foreground = code - 90 + 8;
        } else if (code == 39) {
            m_pen.foreground = -1;
        } else if (code >= 40 && code <= 47) {
            m_pen.background = code - 40;
        } else if (code >= 100 && code <= 107) {
            m_pen.background = code - 100 + 8;
        } else if (code == 49) {
            m_pen.background = -1;
        } else if ((code == 38 || code == 48) && i + 2 < parameters.size() && parameters.at(i + 1) == 5) {
            // 256-colour form: 38;5;n. The truecolour form (38;2;r;g;b) is
            // stepped over rather than approximated.
            (code == 38 ? m_pen.foreground : m_pen.background) = parameters.at(i + 2);
            i += 2;
        } else if ((code == 38 || code == 48) && i + 4 < parameters.size() && parameters.at(i + 1) == 2) {
            i += 4;
            m_sawUnsupported = true;
        }
    }
}

void TerminalScreen::handleControlSequence(char final, const QList<int>& parameters, bool question)
{
    const auto parameter = [&parameters](int index, int fallback) {
        return index < parameters.size() && parameters.at(index) > 0 ? parameters.at(index) : fallback;
    };

    if (question) {
        // Private modes: alternate screen, cursor visibility and the like. Not
        // handled, and noted so the panel can say the display may be wrong.
        if (final == 'h' || final == 'l') {
            const int mode = parameter(0, 0);
            if (mode == 1049 || mode == 47 || mode == 1047)
                m_sawUnsupported = true;
        }
        return;
    }

    switch (final) {
    case 'A':
        m_cursorRow = std::max(0, m_cursorRow - parameter(0, 1));
        break;
    case 'B':
        m_cursorRow = std::min(m_rows - 1, m_cursorRow + parameter(0, 1));
        break;
    case 'C':
        m_cursorColumn = std::min(m_columns - 1, m_cursorColumn + parameter(0, 1));
        break;
    case 'D':
        m_cursorColumn = std::max(0, m_cursorColumn - parameter(0, 1));
        break;
    case 'G':
        m_cursorColumn = std::clamp(parameter(0, 1) - 1, 0, m_columns - 1);
        break;
    case 'H':
    case 'f':
        m_cursorRow = std::clamp(parameter(0, 1) - 1, 0, m_rows - 1);
        m_cursorColumn = std::clamp(parameter(1, 1) - 1, 0, m_columns - 1);
        break;
    case 'J':
        eraseInDisplay(parameters.isEmpty() ? 0 : parameters.first());
        break;
    case 'K':
        eraseInLine(parameters.isEmpty() ? 0 : parameters.first());
        break;
    case 'm':
        applyGraphicRendition(parameters);
        break;
    case 's':
        m_savedRow = m_cursorRow;
        m_savedColumn = m_cursorColumn;
        break;
    case 'u':
        m_cursorRow = m_savedRow;
        m_cursorColumn = m_savedColumn;
        break;
    default:
        // Anything else is left alone. Guessing at a sequence is how a screen
        // ends up subtly wrong in a way nobody can explain.
        m_sawUnsupported = true;
        break;
    }
}

void TerminalScreen::feed(const QByteArray& data)
{
#ifdef MOLE_HAVE_VTERM
    if (m_vterm && m_vterm->term) {
        QByteArray whole = m_vterm->partial + data;
        m_vterm->partial = takeIncompleteTail(whole);

        vterm_input_write(m_vterm->term, whole.constData(), whole.size());
        readBackFromVterm();
        emit changed();
        return;
    }
#endif

    // Decoded incrementally: a multi-byte character can arrive split across two
    // reads, and decoding each read on its own would corrupt it.
    static thread_local QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(data);

    for (const QChar c : text) {
        switch (m_state) {
        case State::Ground:
            if (c == QLatin1Char('\x1b')) {
                m_state = State::Escape;
                m_pending.clear();
            } else if (c == QLatin1Char('\n')) {
                newline();
            } else if (c == QLatin1Char('\r')) {
                carriageReturn();
            } else if (c == QLatin1Char('\b')) {
                backspace();
            } else if (c == QLatin1Char('\t')) {
                tab();
            } else if (c == QLatin1Char('\a')) {
                // The bell. Nothing to draw, and flashing the window would be
                // worse than silence.
            } else if (c.unicode() >= 32) {
                putCharacter(c);
            }
            break;

        case State::Escape:
            if (c == QLatin1Char('[')) {
                m_state = State::ControlSequence;
                m_pending.clear();
            } else if (c == QLatin1Char(']')) {
                m_state = State::OperatingSystemCommand;
                m_pending.clear();
            } else {
                // Two-character escapes. Only the ones that matter here are
                // acted on; the rest are consumed so they do not print.
                m_state = State::Ground;
            }
            break;

        case State::ControlSequence:
            if (c.unicode() >= 0x40 && c.unicode() <= 0x7e) {
                const bool question = m_pending.startsWith('?');
                const QByteArray body = question ? m_pending.mid(1) : m_pending;

                QList<int> parameters;
                const QList<QByteArray> pieces = body.split(';');
                for (const QByteArray& piece : pieces)
                    parameters.append(piece.toInt());

                handleControlSequence(static_cast<char>(c.unicode()), parameters, question);
                m_state = State::Ground;
                m_pending.clear();
            } else {
                m_pending.append(static_cast<char>(c.unicode()));
            }
            break;

        case State::OperatingSystemCommand:
            // Terminated by BEL or ST. Only the window title is read from it.
            if (c == QLatin1Char('\a') || c == QLatin1Char('\x1b')) {
                const QByteArray body = m_pending;
                const int semicolon = body.indexOf(';');
                if (semicolon >= 0) {
                    const int kind = body.left(semicolon).toInt();
                    if (kind == 0 || kind == 2) {
                        m_title = QString::fromUtf8(body.mid(semicolon + 1));
                        emit titleChanged();
                    }
                }
                m_state = State::Ground;
                m_pending.clear();
            } else {
                m_pending.append(static_cast<char>(c.unicode()));
            }
            break;
        }
    }

    emit changed();
}

} // namespace mole
