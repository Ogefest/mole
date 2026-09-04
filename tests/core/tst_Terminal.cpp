#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/terminal/Pty.h"
#include "core/terminal/TerminalScreen.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_UNIX
#include <sys/wait.h>
#endif

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
    void reportsTheShellsRealExitCodeAndLeavesNoZombie();
    void aLongPasteArrivesWhole();

    // ---- the built-in parser, whatever this build found -------------------
    void aSavedCursorRestoredIntoAShrunkenScreenWritesNothingOutside();
    void aCursorMoveWithAHugeParameterStaysOnTheScreen();
    void twoScreensDoNotShareAHalfDecodedCharacter();
    void aCharacterSetDesignatorPrintsNothing();
    void anOscEndedByStringTerminatorPrintsNothing();

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

void TestTerminal::reportsTheShellsRealExitCodeAndLeavesNoZombie()
{
#ifndef Q_OS_UNIX
    QSKIP("no pseudo-terminal on this platform");
#else
    // Two faults in one shape. stop() sent SIGHUP and did a single
    // waitpid(WNOHANG) into a status it threw away; readReady() then waited again
    // on EOF, got ECHILD for a child the first call had reaped, left status at 0
    // and reported "exited with code 0" for a shell that died with 127. When the
    // first call was too early instead, nothing ever waited again and the child
    // stayed a zombie for the life of the process -- one per closed panel.
    // See MOLE-363.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    Pty pty;
    QString error;
    Pty::Options options;
    options.arguments = { QStringLiteral("-c"), QStringLiteral("exit 3") };
    QVERIFY2(pty.start(directory.path(), QStringLiteral("/bin/sh"), options, &error), qPrintable(error));

    int reported = -1;
    connect(&pty, &Pty::finished, this, [&reported](int code) { reported = code; });
    QVERIFY(waitFor([&reported] { return reported >= 0; }, 10000));
    QCOMPARE(reported, 3);
    QCOMPARE(pty.exitCode(), 3);

    // And nothing is left to reap. waitpid over every child answers -1 with
    // ECHILD when there are none, which is the whole assertion: a zombie would
    // answer with its pid.
    int status = 0;
    const pid_t any = ::waitpid(-1, &status, WNOHANG);
    QVERIFY2(any <= 0, "a shell that exited was left as a zombie");
#endif
}

void TestTerminal::aLongPasteArrivesWhole()
{
#ifndef Q_OS_UNIX
    QSKIP("no pseudo-terminal on this platform");
#else
    // write() broke out of its loop on the first EAGAIN and discarded the rest,
    // so a paste beyond a few kilobytes arrived truncated -- and a truncated
    // paste into a shell is a command nobody typed.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    Pty pty;
    QString error;
    Pty::Options options;
    // `wc -c` counts what reached it, which is the number this is about. Echo off
    // so the answer is not buried in 64 KiB of its own input; the line discipline
    // is left alone, because raw mode has no end-of-input character and `wc`
    // would never answer at all.
    options.arguments = { QStringLiteral("-c"), QStringLiteral("stty -echo; wc -c") };
    QVERIFY2(pty.start(directory.path(), QStringLiteral("/bin/sh"), options, &error), qPrintable(error));

    QByteArray seen;
    connect(&pty, &Pty::output, this, [&seen](const QByteArray& data) { seen.append(data); });
    // Once the shell has run stty it is ready for the paste, and `stty -echo`
    // silences the prompt -- so the condition is that nothing more arrives.
    QTest::qWait(200);

    // Well past a pty's buffer, which is a few kilobytes. In lines, because a
    // canonical line discipline holds a line at a time and 64 KiB with no break
    // in it is not a paste any terminal would take.
    constexpr int kLines = 1024;
    constexpr int kLineBytes = 64; // 63 characters and the newline
    QByteArray paste;
    paste.reserve(kLines * kLineBytes);
    for (int i = 0; i < kLines; ++i)
        paste.append(QByteArray(kLineBytes - 1, 'x')).append('\n');
    QCOMPARE(paste.size(), kLines * kLineBytes);

    seen.clear();
    pty.write(paste);
    // End of input at the start of a line, so wc answers.
    pty.write(QByteArray(1, '\x04'));

    QVERIFY2(waitFor([&seen] { return seen.simplified().contains(' ') || seen.contains('\n'); }, 20000),
        qPrintable(QString::fromUtf8(seen)));
    QVERIFY2(seen.contains(QByteArray::number(kLines * kLineBytes)),
        qPrintable(QStringLiteral("wc counted: %1").arg(QString::fromUtf8(seen.simplified()))));
#endif
}

// ------------------------- the built-in parser, whatever this build found

void TestTerminal::aSavedCursorRestoredIntoAShrunkenScreenWritesNothingOutside()
{
    // The memory-safety one, and it is reachable by `cat` of the wrong file.
    // `CSI s` saves the cursor; a resize to fewer rows clamped the live cursor and
    // left the saved one alone; `CSI u` restored a row past the end of the grid;
    // and the next `CSI K` indexed m_grid[m_cursorRow] straight through
    // eraseInLine() with no check -- an assert in a debug build and memory
    // corruption in a release one. cellAt() is bounds-checked and this path was
    // not. See MOLE-363.
    TerminalScreen screen(TerminalScreen::Emulator::BuiltIn);
    screen.resize(20, 10);

    // Park the cursor near the bottom and save it.
    screen.feed("\x1b[9;1H");
    screen.feed("\x1b[s");
    QCOMPARE(screen.cursorRow(), 8);

    // The splitter moves and the screen is three rows tall.
    screen.resize(20, 3);

    // Restore, then erase. Under asan the second of these was the crash.
    screen.feed("\x1b[u");
    QVERIFY2(screen.cursorRow() < screen.rows(), "a restored cursor is outside the screen");
    QVERIFY(screen.cursorRow() >= 0);
    screen.feed("\x1b[K");
    screen.feed("\x1b[J");

    // And the screen still works afterwards.
    screen.feed("\x1b[1;1Hafter");
    QCOMPARE(screen.rowText(0), QStringLiteral("after"));
}

void TestTerminal::aCursorMoveWithAHugeParameterStaysOnTheScreen()
{
    // `m_cursorRow + parameter` is signed arithmetic, so CSI 2147483647 B
    // overflowed it into a negative row -- undefined behaviour on the way to the
    // same unchecked write.
    TerminalScreen screen(TerminalScreen::Emulator::BuiltIn);
    screen.resize(20, 5);

    screen.feed("\x1b[2147483647B");
    QVERIFY(screen.cursorRow() >= 0);
    QVERIFY(screen.cursorRow() < screen.rows());
    screen.feed("\x1b[K");

    screen.feed("\x1b[2147483647C");
    QVERIFY(screen.cursorColumn() >= 0);
    QVERIFY(screen.cursorColumn() < screen.columns());

    screen.feed("\x1b[2147483647;2147483647H");
    QVERIFY(screen.cursorRow() < screen.rows());
    QVERIFY(screen.cursorColumn() < screen.columns());

    screen.feed("\x1b[1;1Hstill here");
    QCOMPARE(screen.rowText(0), QStringLiteral("still here"));
}

void TestTerminal::twoScreensDoNotShareAHalfDecodedCharacter()
{
    // The decoder was `static thread_local`, so two panels on one thread shared
    // the partial state of a split multi-byte character and corrupted each
    // other's. Interleaved halves of one letter is the smallest form of it.
    TerminalScreen first(TerminalScreen::Emulator::BuiltIn);
    TerminalScreen second(TerminalScreen::Emulator::BuiltIn);

    const QByteArray letter = QStringLiteral("ó").toUtf8();
    QCOMPARE(letter.size(), 2);

    first.feed(letter.left(1));
    second.feed(letter.left(1));
    first.feed(letter.mid(1));
    second.feed(letter.mid(1));

    QCOMPARE(first.rowText(0), QStringLiteral("ó"));
    QCOMPARE(second.rowText(0), QStringLiteral("ó"));
}

void TestTerminal::aCharacterSetDesignatorPrintsNothing()
{
    // `ESC ( B` is what `tput sgr0` emits, so most prompts send it -- and the B
    // fell through to Ground and printed. A stray letter in front of every
    // prompt.
    TerminalScreen screen(TerminalScreen::Emulator::BuiltIn);
    screen.resize(20, 3);

    screen.feed("\x1b(B\x1b[m$ ");
    QCOMPARE(screen.rowText(0), QStringLiteral("$"));

    // And the other designators, which arrive from the same places.
    screen.clear();
    screen.feed("\x1b)0\x1b*B\x1b#8ok");
    QCOMPARE(screen.rowText(0), QStringLiteral("ok"));
}

void TestTerminal::anOscEndedByStringTerminatorPrintsNothing()
{
    // An OSC ends at BEL or at ST, which is ESC backslash. Dropping straight to
    // Ground on the ESC printed the backslash.
    TerminalScreen screen(TerminalScreen::Emulator::BuiltIn);
    screen.resize(20, 3);

    screen.feed("\x1b]0;A title\x1b\\done");
    QCOMPARE(screen.title(), QStringLiteral("A title"));
    QCOMPARE(screen.rowText(0), QStringLiteral("done"));

    // The BEL form still works, which is the one most shells use.
    screen.clear();
    screen.feed("\x1b]0;Another\aok");
    QCOMPARE(screen.title(), QStringLiteral("Another"));
    QCOMPARE(screen.rowText(0), QStringLiteral("ok"));
}

MOLE_TEST_MAIN(TestTerminal)
#include "tst_Terminal.moc"
