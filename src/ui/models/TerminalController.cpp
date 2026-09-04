#include "ui/models/TerminalController.h"

#include "core/vfs/VfsUri.h"

#include <QDir>

namespace mole {

TerminalController::TerminalController(QObject* parent)
    : QObject(parent)
{
    connect(&m_pty, &Pty::output, this, [this](const QByteArray& data) { m_screen.feed(data); });
    connect(&m_pty, &Pty::finished, this, [this](int exitCode) {
        // A shell that ended on its own -- Ctrl+D, or `exit` -- takes the panel
        // with it, because that is what closing a terminal means everywhere
        // else. Reopening starts a fresh one in the current folder.
        if (exitCode == 0) {
            m_errorText.clear();
            emit runningChanged();
            setVisible(false);
            return;
        }

        // A shell that died of something keeps the panel open, or the reason
        // would disappear along with it.
        m_errorText
            = QStringLiteral("The shell exited with code %1. Press Enter to start another.").arg(exitCode);
        emit runningChanged();
    });
    connect(&m_screen, &TerminalScreen::changed, this, &TerminalController::screenChanged);
    connect(&m_screen, &TerminalScreen::titleChanged, this, &TerminalController::screenChanged);
}

bool TerminalController::isAvailable() const
{
#ifdef Q_OS_UNIX
    return true;
#else
    return false;
#endif
}

void TerminalController::setVisible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;
    emit visibleChanged();
}

QString TerminalController::rowText(int index) const
{
    return m_screen.rowText(index);
}

QStringList TerminalController::ansiPalette()
{
    // The xterm sixteen: eight normal, eight bright. Terminals name a colour by
    // index, so the mapping has to live somewhere, and these values are the ones
    // the panel has always drawn with -- chosen to sit with the rest of the window
    // so output looks the way its author intended.
    return { QStringLiteral("#1b2029"), QStringLiteral("#e5534b"), QStringLiteral("#57ab5a"),
        QStringLiteral("#d9a441"), QStringLiteral("#4c9aff"), QStringLiteral("#c792ea"),
        QStringLiteral("#5bc8d6"), QStringLiteral("#c9d1e0"), QStringLiteral("#5c6472"),
        QStringLiteral("#ff7b72"), QStringLiteral("#7ee787"), QStringLiteral("#f0c674"),
        QStringLiteral("#7cc4ff"), QStringLiteral("#d2a8ff"), QStringLiteral("#86d9e8"),
        QStringLiteral("#f0f6fc") };
}

QVariantList TerminalController::screenRows() const
{
    QVariantList rows;
    rows.reserve(m_screen.rows());
    for (int row = 0; row < m_screen.rows(); ++row)
        rows.append(QVariant::fromValue(rowSpans(row)));
    return rows;
}

QVariantList TerminalController::rowSpans(int index) const
{
    QVariantList spans;
    const QList<TerminalCell> line = m_screen.row(index);
    if (line.isEmpty())
        return spans;

    // Runs of identical styling. A row of eighty individually styled items
    // would cost more to lay out than the panel is worth, and a typical line
    // has one or two runs.
    QString text;
    int foreground = line.first().foreground;
    int background = line.first().background;
    bool bold = line.first().bold;
    bool inverse = line.first().inverse;

    const auto flush = [&]() {
        while (text.endsWith(QLatin1Char(' ')) && background < 0)
            text.chop(1); // trailing blanks with no background are not content
        if (text.isEmpty())
            return;
        spans.append(QVariantMap { { QStringLiteral("text"), text },
            { QStringLiteral("foreground"), foreground }, { QStringLiteral("background"), background },
            { QStringLiteral("bold"), bold }, { QStringLiteral("inverse"), inverse } });
        text.clear();
    };

    for (const TerminalCell& cell : line) {
        if (cell.foreground != foreground || cell.background != background || cell.bold != bold
            || cell.inverse != inverse) {
            flush();
            foreground = cell.foreground;
            background = cell.background;
            bold = cell.bold;
            inverse = cell.inverse;
        }
        text.append(cell.character);
    }
    flush();
    return spans;
}

void TerminalController::open(const QString& directory)
{
    m_errorText.clear();

    // A uri from the browser, a path from anywhere else. A virtual drive has no
    // directory for a process to start in, so the panel says so rather than
    // opening a shell somewhere unrelated.
    QString path = directory;
    if (path.contains(QStringLiteral("://"))) {
        const VfsUri uri = VfsUri::fromString(path);
        if (uri.scheme() != QLatin1String("file")) {
            m_errorText = QStringLiteral("A shell cannot open inside %1. Terminals need a real "
                                         "directory on disk.")
                              .arg(uri.scheme());
            emit runningChanged();
            return;
        }
        path = uri.path();
    }
    if (path.isEmpty())
        path = QDir::homePath();

    if (m_pty.isRunning() && m_workingDirectory == path)
        return;
    if (m_pty.isRunning())
        m_pty.stop();

    m_screen.clear();
    m_workingDirectory = path;

    Pty::Options options;
    // A development and screenshot hook, in the shape every other MOLE_* variable
    // here has: the arguments the shell is started with instead of the bare `-i`.
    // The screenshot harness sets it to "--norc --noprofile -i" so the pictures
    // carry a prompt belonging to the project rather than to whoever ran the
    // suite; it used to write a wrapper script and exec bash out of it, which is
    // a second way of starting a shell that behaves differently from this one.
    // See MOLE-255 for why the prompt matters and MOLE-363 for the wrapper.
    const QString extraArguments = QString::fromLocal8Bit(qgetenv("MOLE_TERMINAL_ARGUMENTS"));
    if (!extraArguments.isEmpty())
        options.arguments = extraArguments.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    QString error;
    if (!m_pty.start(path, {}, options, &error))
        m_errorText = error;

    m_pty.resize(m_screen.columns(), m_screen.rows());
    emit runningChanged();
    emit screenChanged();
}

void TerminalController::close()
{
    m_pty.stop();
    emit runningChanged();
}

void TerminalController::sendKey(int key, int modifiers, const QString& text)
{
    if (!m_pty.isRunning()) {
        // Enter on a dead shell starts another, which is what pressing a key at
        // a finished prompt should obviously do.
        m_errorText.clear();
        open(m_workingDirectory);
        return;
    }
    m_pty.write(TerminalScreen::encodeKey(key, modifiers, text));
}

void TerminalController::sendText(const QString& text)
{
    if (m_pty.isRunning())
        m_pty.write(text.toUtf8());
}

void TerminalController::setSize(int columns, int rows)
{
    if (columns == m_screen.columns() && rows == m_screen.rows())
        return;
    m_screen.resize(columns, rows);
    m_pty.resize(columns, rows);
    emit screenChanged();
}

void TerminalController::followTo(const QString& directory)
{
    if (!m_pty.isRunning()) {
        open(directory);
        return;
    }

    QString path = directory;
    if (path.contains(QStringLiteral("://"))) {
        const VfsUri uri = VfsUri::fromString(path);
        if (uri.scheme() != QLatin1String("file"))
            return; // nowhere for a shell to go
        path = uri.path();
    }
    if (path.isEmpty())
        return;

    // Typed in rather than imposed: a shell's working directory is its own, and
    // this is the only way to change it. Quoted, because a path can contain
    // anything at all.
    QString quoted = path;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    m_pty.write(QStringLiteral("cd '%1'\n").arg(quoted).toUtf8());
    m_workingDirectory = path;
    emit runningChanged();
}

} // namespace mole
