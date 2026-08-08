#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/terminal/Pty.h"
#include "core/terminal/TerminalScreen.h"

#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// The terminal: what the parser understands, and what it admits it does not.
class TestTerminal : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void printsPlainText();
    void wrapsAtTheRightEdge();
    void handlesCarriageReturnAndBackspace();
    void tabsToEightColumnStops();
    void scrollsWhenTheCursorLeavesTheBottom();

    void movesTheCursor();
    void erasesToTheEndOfTheLine();
    void erasesTheWholeScreen();
    void appliesColour();
    void readsTheWindowTitle();

    void survivesASequenceSplitAcrossReads();
    void survivesAMultiByteCharacterSplitAcrossReads();
    void admitsWhenItDoesNotUnderstandSomething();
    void aFullScreenProgramWorksWhenTheEmulatorIsComplete();

    void runsAShellAndReportsItsOutput();

private:
    std::unique_ptr<TerminalScreen> m_screen;
};

void TestTerminal::init()
{
    m_screen = std::make_unique<TerminalScreen>();
    m_screen->resize(20, 5);
}

void TestTerminal::printsPlainText()
{
    m_screen->feed("hello");
    QCOMPARE(m_screen->rowText(0), QStringLiteral("hello"));
    QCOMPARE(m_screen->cursorColumn(), 5);
}

void TestTerminal::wrapsAtTheRightEdge()
{
    m_screen->resize(5, 3);
    m_screen->feed("abcdefg");
    QCOMPARE(m_screen->rowText(0), QStringLiteral("abcde"));
    QCOMPARE(m_screen->rowText(1), QStringLiteral("fg"));
}

void TestTerminal::handlesCarriageReturnAndBackspace()
{
    // A shell redrawing its prompt relies on both, constantly.
    m_screen->feed("hello\rworld");
    QCOMPARE(m_screen->rowText(0), QStringLiteral("world"));

    m_screen->feed("\b\bXY");
    QCOMPARE(m_screen->rowText(0), QStringLiteral("worXY"));
}

void TestTerminal::tabsToEightColumnStops()
{
    m_screen->feed("ab\tc");
    QCOMPARE(m_screen->rowText(0), QStringLiteral("ab      c"));
}

void TestTerminal::scrollsWhenTheCursorLeavesTheBottom()
{
    m_screen->resize(10, 3);
    // "\r\n", which is what actually comes off a pseudo-terminal: a bare
    // newline moves down a row and leaves the column alone, and the driver adds
    // the carriage return.
    m_screen->feed("one\r\ntwo\r\nthree\r\nfour");

    // The top line has gone and everything moved up, which is what a terminal
    // does rather than stopping at the bottom.
    QCOMPARE(m_screen->rowText(0), QStringLiteral("two"));
    QCOMPARE(m_screen->rowText(2), QStringLiteral("four"));
}

void TestTerminal::movesTheCursor()
{
    m_screen->feed("\x1b[2;3Hhi");
    QCOMPARE(m_screen->rowText(1), QStringLiteral("  hi"));

    m_screen->feed("\x1b[1;1Hx");
    QCOMPARE(m_screen->rowText(0).at(0), QLatin1Char('x'));
}

void TestTerminal::erasesToTheEndOfTheLine()
{
    m_screen->feed("abcdef\r\x1b[3C\x1b[K");
    // Erasing from the cursor leaves what is behind it, which is how a shell
    // shortens a line it is redrawing.
    QCOMPARE(m_screen->rowText(0), QStringLiteral("abc"));
}

void TestTerminal::erasesTheWholeScreen()
{
    m_screen->feed("one\r\ntwo\x1b[2J");
    QCOMPARE(m_screen->rowText(0), QString());
    QCOMPARE(m_screen->rowText(1), QString());
}

void TestTerminal::appliesColour()
{
    m_screen->feed("\x1b[31mred\x1b[0mplain");

    const QList<TerminalCell> line = m_screen->row(0);
    QCOMPARE(line.at(0).foreground, 1);
    QCOMPARE(line.at(2).foreground, 1);
    // Reset put it back, rather than colouring the rest of the session.
    QCOMPARE(line.at(3).foreground, -1);

    m_screen->feed("\r\x1b[38;5;208mx");
    QCOMPARE(m_screen->row(0).at(0).foreground, 208);
}

void TestTerminal::readsTheWindowTitle()
{
    QSignalSpy titles(m_screen.get(), &TerminalScreen::titleChanged);
    m_screen->feed("\x1b]0;~/projects\a");

    QCOMPARE(m_screen->title(), QStringLiteral("~/projects"));
    QCOMPARE(titles.count(), 1);
    // And the title itself was not printed to the screen.
    QCOMPARE(m_screen->rowText(0), QString());
}

void TestTerminal::survivesASequenceSplitAcrossReads()
{
    // A read boundary can fall anywhere, including the middle of an escape
    // sequence. Handling each read independently would print the fragments.
    m_screen->feed("\x1b[3");
    m_screen->feed("1mred");

    QCOMPARE(m_screen->rowText(0), QStringLiteral("red"));
    QCOMPARE(m_screen->row(0).at(0).foreground, 1);
}

void TestTerminal::survivesAMultiByteCharacterSplitAcrossReads()
{
    const QByteArray utf8 = QString::fromUtf8("Kraków").toUtf8();
    // Split inside the two bytes of "ó".
    const int cut = utf8.indexOf('\xc3');
    QVERIFY(cut > 0);

    m_screen->feed(utf8.left(cut + 1));
    m_screen->feed(utf8.mid(cut + 1));

    QCOMPARE(m_screen->rowText(0), QString::fromUtf8("Kraków"));
}

void TestTerminal::admitsWhenItDoesNotUnderstandSomething()
{
    QVERIFY(!m_screen->sawUnsupported());

    // The alternate screen, which every full-screen program switches to.
    m_screen->feed("\x1b[?1049h");

    if (TerminalScreen::isComplete()) {
        // libvterm is a real terminal emulator: there is nothing here it cannot
        // draw, so the panel has nothing to warn about.
        QVERIFY2(!m_screen->sawUnsupported(), "a complete emulator has no unsupported sequences to admit to");
    } else {
        // The built-in parser cannot, and says so. Drawing something plausible
        // instead is how a panel ends up subtly wrong in a way nobody can
        // explain.
        QVERIFY2(m_screen->sawUnsupported(), "the fallback parser has to admit what it cannot draw");
    }
}

void TestTerminal::runsAShellAndReportsItsOutput()
{
#ifndef Q_OS_UNIX
    QSKIP("no pseudo-terminal on this platform");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    Pty pty;
    QString error;
    QVERIFY2(pty.start(directory.path(), QStringLiteral("/bin/sh"), &error), qPrintable(error));
    QVERIFY(pty.isRunning());

    QByteArray seen;
    connect(&pty, &Pty::output, this, [&seen](const QByteArray& data) { seen.append(data); });

    pty.resize(80, 24);
    pty.write("echo mole-marker\n");

    // A real shell on a real pty. Without the pty it would not even print a
    // prompt, which is the whole reason this is not a pipe.
    QVERIFY(waitFor([&seen] { return seen.contains("mole-marker"); }, 10000));

    pty.stop();
    QVERIFY(!pty.isRunning());
#endif
}

void TestTerminal::aFullScreenProgramWorksWhenTheEmulatorIsComplete()
{
    if (!TerminalScreen::isComplete())
        QSKIP("this build has the fallback parser, which does not do full-screen programs");

    m_screen->resize(20, 5);
    m_screen->feed("scrollback\r\n");

    // Switch to the alternate screen, draw on it, and switch back. A real
    // emulator restores what was underneath; an approximation loses it, which
    // is precisely what makes running an editor in a panel unusable.
    m_screen->feed("\x1b[?1049h");
    m_screen->feed("\x1b[2J\x1b[1;1Hfull screen");
    QCOMPARE(m_screen->rowText(0), QStringLiteral("full screen"));

    m_screen->feed("\x1b[?1049l");
    QCOMPARE(m_screen->rowText(0), QStringLiteral("scrollback"));
}

MOLE_TEST_MAIN(TestTerminal)
#include "tst_Terminal.moc"
