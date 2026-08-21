# ADR-0073: A theme is sixteen values, and the choice is a preference

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

[ADR-0072](0072-colour-lives-in-one-place-and-a-view-names-a-token.md) put colour
in one place: sixteen named tokens on `mole::Palette`, and a view names a token
rather than a value. That was the whole point of it — when colour became a choice,
the views would not change.

It now is one. Mole ships four themes and the user picks one: `Midnight`, which is
what Mole has always looked like, `Slate`, and the two light ones, `Paper` and
`Workbench`. This record covers the mechanism and the two dark themes.
MOLE-280 adds the light pair, which is not a palette swap —
several things in the tree were written for a dark ground and have to follow a
theme's *polarity* rather than its values.

## Decision

**A theme is a name and sixteen values, and nothing else.** `Palette::Tokens` is
the struct; one function per theme returns one, and a table in `Palette.cpp` maps
the names to those functions. `Palette::setTheme(name)` repaints all sixteen and
announces it once; `Palette::theme()` says which is in force. Adding a theme is a
function and a line in that table, and touches no view and no QML.

**An unknown name opens on the default rather than on nothing**, with one line in
the log saying which name was not recognised. A preferences file can outlive the
build that wrote it, and a window that failed to paint is not a better answer than
a window painted in Midnight.

**The chooser is checkable entries in `View`**, one per theme, registered by the
same loop-over-a-table shape the four layout modes already use: a `checked` lambda
comparing against `Palette::theme()` and a `trigger` that sets it. `separatorBefore`
on the first of the group. They are numbered from 100 rather than joining the
existing numbers, because those already collide twice — `Dual pane` and
`Show hidden files` are both 20, `Grid of icons` and `Refresh` are both 30 — and a
third collision is not the way to add a group.

**The choice is a preference, not part of the session.** One dotted key,
`ui.theme`, read once at startup and written when it changes.
`AppController::setTheme()` is the only thing that writes it.

**The default is `Midnight`, and that is a decision about the guide rather than
about the colours.** The walkthrough runs with `MOLE_PREFERENCES_PATH` pointed at a
test file, so it starts having remembered nothing and gets the default — which is
what lets all 54 of the guide's pictures go on being pictures of the window they
were taken of.

## Reason

**`View` rather than a heading of its own.** `MenuAction::Section::View` was
described in the SDK as "how the current tab looks", and a theme is window-wide,
so this stretches that wording. It does not contradict the reasoning behind it:
ADR-0003 exists to stop the menu growing a heading per feature family — "a menu
with eleven top-level headings is not navigation, it is a search problem" — and
`View` is where anybody looking for *how this looks* would look first. A fifth
top-level heading holding one entry is the outcome that ADR would have refused.
The enum's comment now says so rather than leaving the next reader to wonder.

**`Preferences` rather than `SessionStore`.** The session file's own header draws
the line: it is about what was open, not what was chosen. A theme is not part of a
window's contents, it outlives every tab in it, and `Preferences::setValue`
already does nothing when the value is unchanged, so a chooser that assigns on
every trigger does not rewrite the file each time.

**`Palette` knows nothing about `Preferences`.** The wiring is in
`AppController`, which is where every other store is opened. That keeps `Palette` a
plain value holder constructible in a test with no filesystem — `tst_Palette` does
exactly that — and keeps there being one writer for `ui.theme`. Two writers is how
a remembered choice and a chosen one stop agreeing.

**Sixteen values per theme rather than a base plus modifiers.** Deriving Slate from
Midnight by shifting hue and lightness was considered, and would have made a theme
three numbers instead of sixteen. It was rejected because the interesting part of a
theme is exactly where it *stops* being a uniform shift: `Slate`'s accent is a
soft teal where Midnight's is a bright blue, and its `ok`, `warn` and `bad` are
desaturated further than its greys are. A formula that can express that is not
shorter than the table.

**`Slate` is deliberately the cheap second theme.** Same polarity, so no control
has to be reconsidered and nothing in the tree that assumed a dark ground is
wrong. It exists to prove the mechanism carries a theme, before the light pair
proves what the mechanism does not carry.

## Consequences

- The command palette needed nothing at all. `CommandPaletteModel` builds from
  `ActionRegistry::buildModel()`, so a registered menu entry is already a row:
  `Ctrl+R`, `slate`, Enter.
- Choosing a theme repaints a running window, including any popup open at the
  time, because every binding reads a notifying token. That is the property
  ADR-0072's `CONSTANT` warning is about, and `tst_Palette` holds it.
- `Material.theme` is still `Material.Dark`, stated once in `Main.qml`. Both
  themes here are dark, so nothing yet forces it to be a question; MOLE-280 is
  where it becomes one.
- A preferences file naming `Paper` today opens on `Midnight` and says so in the
  log. When MOLE-280 lands, the same file opens on `Paper`. That is the intended
  behaviour of an unknown name and not a migration.
