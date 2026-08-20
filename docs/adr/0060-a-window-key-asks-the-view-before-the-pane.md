# ADR-0060: A window key asks the view before the pane

- **Date:** 2026-08-20
- **Status:** Accepted. Revises [ADR-0019](0019-the-keys-that-belong-to-the-window.md),
  which stands otherwise.

## Context

ADR-0019 moved `F3`, `Ctrl+↑`, `Ctrl+←` and `Ctrl+→` off the focused item and
onto the window, and routed all four through the active tab's `activePane`. That
was right for the fault it was about — a key that only worked when the listing
happened to hold the keyboard — and it left one path to the answer.

Only a browser tab has an `activePane`. A search tab has a cursor over files and
no pane, so `F3` there resolved to nothing: the key was advertised in the guide,
the results list carried a handler for it that a window shortcut is matched
before, and pressing it did nothing at all. The same is true of every future tab
whose subject is a list of files it does not own — a set's members, a duplicate
group, whatever a plugin brings.

The window already knows how to ask a view for a named capability without
learning what the view is. `moveCursorBy()`, `activateCurrentRow()` and
`goUpOneFolder()` answer the arrows, `Enter` and `Backspace`;
`focusPathBar()` answers `Ctrl+G`; `focusFilter()` answers the filter action;
`focusActivePane()` puts the keyboard back after a tab switch. `F3` is the one
navigation key that was never given that door.

## Decision

**A window shortcut asks the current tab's view for a named function first, and
falls back to resolving through `activePane`.**

For `F3` that is `previewCurrentRow()`. A view that has one answers the key
itself; a view that has none — the browser — is unchanged and keeps resolving
through its pane.

Two things follow from that, and both are the point rather than a detail:

- **A tab is never given an `activePane` in order to make a key work.** The
  property is a browser's idea of which of its two panes is current, and a tab
  that has no panes claiming to have one switches on everything else that is
  gated on the property existing — *Show hidden files*, *Filter this folder*,
  *Refresh*, *Index this folder* — one of which reads a location it does not
  have. A named function costs four lines and switches on nothing.
- **A key the window owns is handled in exactly one place.** The unreachable
  `F3` handlers in `FilePane` and in the results list are gone. A `Shortcut` is
  matched before the key reaches any item, so a second handler is not a fallback;
  it is dead code that reads as a fallback, which is how ADR-0019's fault came
  about the first time.

## Reason

**Why not extend `activePane` to every tab that has a cursor.** It is the
cheapest change and the worst one: the property does not mean "where my cursor
is", it means "which of my panes is current", and four menu actions read it as
the second thing. Making a search tab answer it would enable *Index this folder*
on a tab with no folder.

**Why not a new interface for "a thing with a cursor".** There is already a
convention — a function on the view, found by name, absent when the view cannot
answer — and it costs nothing to join. An interface would have to be implemented
in C++ by every feature, including the ones that only want the key to work.

**Why the view rather than the controller.** The cursor over a set's members or
a search's results lives in the QML list that draws them; the controller holds
the model, not the position in it. Asking the controller would mean moving the
cursor into C++ for the sake of one key.

**Why the view is asked first rather than the pane.** A browser has no
`previewCurrentRow()`, so the order is only visible for a view that has both —
and if one ever does, its own function is the more specific answer.

## Consequences

- `F3` works in a search tab, and any view can have it by declaring one
  function. The same door is what a set's member list uses (MOLE-205).
- The fallback order is load-bearing and is stated in the shortcut's own comment:
  a browser must keep resolving through `previewCurrent()`, because the pane's
  cursor is the tab's idea of where it is and the view does not own it.
- ADR-0019's reason *"the pane keeps its handler as the path taken when it does
  hold the keyboard"* no longer holds. It never did — the handler could not run —
  and the risk it was guarding against is real but is answered by having one
  mechanism rather than two.
- The next window key that wants to work outside a browser has a pattern to
  follow, and no reason to add a property to get it.
