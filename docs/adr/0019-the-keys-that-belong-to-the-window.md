# ADR-0019: The keys that belong to the window

- **Date:** 2026-08-10
- **Status:** Accepted. Revises [ADR-0002](0002-window-shortcuts-versus-focused-views.md), which stands otherwise.

## Context

ADR-0002 drew a line: *anything that depends on which pane has focus is handled
inside `FilePane`*. That rule put `F3` and `Ctrl+↑` on the focused item, and the
consequence was that both stopped working whenever the keyboard was anywhere
else. Clicking a drive in the sidebar was enough. Clicking the listing brought
them back.

It reads as though something else is catching the keys. It is the opposite:
nothing catches them at all. `mole.tools.preview` carries `shortcut = "F3"`,
which is only what the menu prints beside it, and there was no `Shortcut`
declaring it — so the documentation and the menu both advertised a key that was
bound to nothing outside one item.

The rule was right about the mechanism and drew the boundary one step too wide.

## Decision

**Depends on the active pane is not the same as depends on the focus.**

`AppController::currentFile()` and `currentLocation()` resolve their target
through the active tab's `activePane` — the tab's own idea of which pane is
current — and never ask what holds the keyboard. That is why *Preview this file*
in the menu has always worked from anywhere.

So the keys whose target is unambiguous without the focus become window
shortcuts, routed through the same resolution:

| | |
|---|---|
| `F3` | preview the file under the pane's cursor, or open it if it is a folder |
| `Ctrl+↑` | up a level |
| `Ctrl+←` `Ctrl+→` | back and forward |

And these stay in `FilePane`, unchanged: cursor movement, type-to-filter,
`Insert` and `Space` selection, `Enter`, `Backspace`. Every one of them is about
the thing holding the keyboard and means nothing without it.

`F2`, `F5`, `F6`, `F7` and `F8` also stay. They need a *source* pane and a
*destination* pane, so unlike `F3` they genuinely depend on which pane is
focused — in a dual-pane window "copy" is meaningless until you know which side
you are copying from.

Separately, and regardless of any of the above: **a control that takes the
keyboard in order to navigate hands it back.** The pane's own toolbar buttons
already did; the sidebar did not.

## Reason

**Why not leave them in the pane and simply return focus everywhere.** That
would fix the reported case and leave the keys one stray focus away from
breaking again — a menu, a dialog, a notification. A key that works only when
the focus happens to be right is a key that will one day stop working.

**Why not make everything a window shortcut.** Then typing a letter to filter
would fight with any single-key shortcut, and cursor keys would move a list the
reader is not looking at. The distinction the routed keys pass and the others
fail is whether the target can be named without asking what has focus.

**Why route through the pane rather than duplicating the logic.** `previewCurrent()`
asks the pane whether the cursor is on a directory and opens it if so, which is
exactly what `FilePane` did. One behaviour, one implementation; the pane keeps
its handler as the path taken when it does hold the keyboard, and the shortcut
takes precedence when it fires.

## Consequences

- The keys work from the sidebar, from a toolbar button, and from anywhere else
  that is not meant to keep the keyboard.
- `README.md` and the in-app list now describe what is bound. Before this they
  described what somebody intended.
- A key added to a menu action's `shortcut` is still only a label. That trap is
  unchanged, and it is the one this fault came through — the next person to add
  a function key has to declare it as well as name it.
- The terminal panel is untouched: it holds every key while it has the keyboard,
  as ADR-0002 case 3 requires, and a window shortcut that fired through it would
  be a regression of that.
