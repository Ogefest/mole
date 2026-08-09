# ADR-0017: The window state is three things, not two

- **Date:** 2026-08-09
- **Status:** Accepted

## Context

The session file recorded the window as a position, a size, and a boolean:
`maximized`. The shell worked that boolean out for itself, in QML, as
`root.visibility === Window.Maximized`.

A full-screen window is not a maximised one. It answered that question with
"no", so the application took its screen-sized metrics for the size somebody had
chosen by hand and wrote them over the real ones. Leaving a window full-screen
therefore lost the size it had before, and the next start opened a plain window
the size of the display.

The bug is one line of QML, but the shape underneath it is what allowed the bug:
a boolean can only distinguish two states, and there are three. Nothing in Mole
puts the window full-screen itself — the state arrives from the window manager —
so this is not a case that could have been ruled out by not offering it.

## Decision

`WindowGeometry` carries a `WindowState` of `Normal`, `Maximized` or
`FullScreen`. It is written to the session file as `windowState`, one of
`"normal"`, `"maximized"` or `"fullscreen"`.

The shell hands the window's own `QWindow::Visibility` straight to
`rememberWindowGeometry()` and the reduction to three states happens in C++, in
one place.

Only `Normal` carries a position and a size. In the other two the remembered
geometry is left as it was, because what the window reports in them is the
screen's, not the reader's.

A session file that has no `windowState` is read for the old `maximized`
boolean, and `true` becomes `Maximized`. A file with both is not ambiguous: the
new key wins. An unrecognised name reads as `Normal`.

## Reason

**Three states rather than two booleans** (`maximized` plus `fullScreen`),
because two booleans have a fourth combination that means nothing, and somebody
would eventually have to decide what "maximised and full-screen" restores as.

**A string in the file rather than a number.** The format is plain JSON on
purpose — a person can read it when something has gone wrong — and `2` is not
readable. It also survives the enum gaining a member in the middle.

**Qt's visibility passed through rather than reduced in QML.** Reducing it at
the call site is precisely what caused this: the caller decided what mattered,
and was wrong. The mapping is now in `AppController::windowStateOf()`, where a
test can reach it and where adding a state means changing one switch.

**The old key still read, rather than a format version bump.** The version
number exists and could have been raised, but raising it to add one field would
mean either refusing older files — losing everybody's tabs to fix a window
size — or writing the same fallback anyway under a different name. Reading the
old key is that fallback, stated once, in the one function that reads windows.

`Minimized` reads as `Normal`, which is what it did when this was a boolean. A
minimised window is not a state to restore into, and the metrics it reports are
the ones it will be restored to.

## Consequences

- A session written by this build cannot be read back correctly by an older one:
  it will see no `maximized` key and start in an ordinary window. Nothing is
  lost but the window state, and only when downgrading.
- The fallback branch is dead weight once no session file predates this change.
  It is cheap, and there is no way to know when that day has come, so it stays.
- Adding a fourth state — a window remembered per screen, say — is a member and
  a case rather than another boolean and another combination nobody defined.
