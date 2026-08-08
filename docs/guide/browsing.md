# Browsing

![A listing](images/01-browser.png)

Directories sort first, then files. The row under the cursor is what every operation
acts on when nothing is ticked — the way a commander behaves — and ticking things with
`Insert` or `Space` makes them the target instead.

## The keyboard

Nothing here needs a mouse.

| | |
|---|---|
| arrows, `Home`, `End`, `PgUp`, `PgDn` | move the cursor |
| `Enter` | open the folder, or launch the file |
| `Backspace` | up one level |
| `Insert` | tick and move on · `Space` tick · `*` invert · `Ctrl+A` all |
| `Tab` | the other pane |
| `Ctrl+T` | a new tab · `Ctrl+W` closes one · `Ctrl+1`…`9` jumps to one |
| `Ctrl+G` | type a path instead of clicking to it |
| `Ctrl+D` | bookmark this folder |
| any letter | filter the listing — no shortcut needed, and it touches no drive |

`F3` previews the file under the cursor. On a folder it opens it, because a folder has
nothing to preview and a key that does nothing is indistinguishable from one that is
broken.

## Two panes, or a grid

![Two panes](images/05-dual-pane.png)

`F5` copies and `F6` moves between them, which is what two panes are for. Both refuse
politely rather than mysteriously when both sides are showing the same folder.

![A grid](images/06-grid.png)

The same listing as icons, for the times when what a file looks like matters more than
its name.

## How big is that folder?

![Folder sizes](images/01b-folder-sizes.png)

`Ctrl+Shift+S` measures the ticked folders — or every folder in the listing when
nothing is ticked — and writes each total into its row as the walk finishes it. It
runs in the background, it can be cancelled, and sorting by size then puts the measured
folders in the order you would expect.

A measurement describes the tree as it was when it was taken, so refreshing the listing
clears it. A stale number is worse than an empty cell.

## When a folder is slow

![A slow folder](images/01d-slow-folder.png)

A listing that takes more than a second says so, in the middle of the pane, rather
than looking like an empty folder. Below a second it says nothing, because a spinner
that flashes is worse than no spinner at all.

## What the rows tell you

![Marks in the listing](images/01c-listing-tags.png)

A folder that has been analysed, or that something is watching, is marked as such —
so the listing carries what is already known about a place rather than making you go
and look.

## Breadcrumbs

![Breadcrumbs](images/01b-breadcrumbs.png)

The path is clickable all the way up. `Ctrl+G` turns it into a field you can type
into, and `Ctrl+←`/`Ctrl+→` walk the history you have already been through.
