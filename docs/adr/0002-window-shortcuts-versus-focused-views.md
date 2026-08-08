# ADR-0002: Which side wins when a window shortcut and a focused view want the same key

- **Date:** 2026-08-08
- **Status:** Accepted

## Context

Three times now a key has gone somewhere other than where the person pressing it
expected, and each time for a different reason:

- **`F5` never reached the listing.** `Ctrl+R` was registered as
  `StandardKey.Refresh`, which on this platform also means `F5` — and `F5` is the
  commander copy key. The window shortcut swallowed it before the pane saw it.
- **`Ctrl+W` stopped closing a tab** once a preview had the keyboard. A read-only
  `TextArea` is still an editor to Qt: it accepts the shortcut-override event for
  every key in the standard editing table, `Ctrl+W` among them
  (`DeleteStartOfWord`), so Qt did not run the matching `Shortcut` — and the text
  control then discarded the key because the document is read-only.
- **`Ctrl+D` bookmarked the folder instead of ending the shell.** In a terminal
  it means end of input. It is also a window `Shortcut` bound to
  `mole.bookmarks.add`, and Qt matches shortcuts before offering the key to the
  focused item, so the terminal panel — whose own handler forwards every key to
  the shell — never saw it.

The mechanism underneath all three is the same: `Shortcut` is resolved against
the window before the focused item is given the key, unless the focused item
accepts a `ShortcutOverride` event first.

## Decision

Three cases, and which remedy belongs to each.

1. **The shortcut is too broad.** Narrow the sequence. `Ctrl+R` is written as
   `Ctrl+R` and not as `StandardKey.Refresh`, because what the platform folds
   into a standard key is not ours to decide.
2. **A view claims keys it has no use for.** The view hands them back, through
   `ViewerKeys.qml`, rather than each view growing a private copy of the shortcut
   table. This is for views that are accidentally greedy — a read-only editor.
3. **A view legitimately wants every key while it holds the keyboard.** The view
   accepts `ShortcutOverride` for everything and forwards the keys itself. This
   is the terminal: while the panel has the keyboard it *is* a terminal, and a
   terminal that let the window keep `Ctrl+C` would be useless.

A view that wants keys says which of the three it is.

## Reason

The obvious alternative is to gate the window shortcuts themselves — `enabled:`
on each `Shortcut`, driven by where the focus is. It was rejected because it puts
the knowledge in the wrong place: the shortcut table would have to know about
every view that might want a key, and that list grows with every view added. The
`ShortcutOverride` event exists precisely so the focused item can answer for
itself, and Qt asks it before deciding.

Case 2 and case 3 look like opposites and are: one hands a key back to the
window, the other takes it away. That is the point of naming them. `ViewerKeys`
cannot be reused for the terminal, and reaching for it there would have produced
a list of keys the shell is not allowed to receive — which is precisely backwards.

## Consequences

- A view that accepts the override for everything is claiming everything, so it
  must offer a way out. The terminal keeps `Ctrl+\``, handled in its own key
  handler rather than by the window, so the escape does not depend on the very
  resolution order that made this decision necessary.
- While the terminal holds the keyboard, window shortcuts do not fire. That is
  intended, not a side effect: the panel already forwarded everything else to the
  shell, and this makes the behaviour consistent instead of dependent on which
  keys Qt happened to intercept first.
- Each of these needs a test with the focus actually somewhere, which is what
  `QmlAppHarness` is for — it posts keys to the real `QQuickWindow`, so the
  delivery path under test is the one production uses. Asserting that a shortcut
  works while nothing has the keyboard proves nothing about any of this;
  `ctrlWClosesAPreviewTabWithTheTextFocused` and
  `theTerminalTakesTheKeyboardAndCtrlDEndsIt` are the two shapes to copy.
- A panel that takes the keyboard has to take it when it appears, too. Focus
  declared with `focus: true` is not the same as having it: something must call
  `forceActiveFocus()` when the panel is revealed, or the keyboard stays where it
  was. The menu does this on open, and the terminal now does it as well.
