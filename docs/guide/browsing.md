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

## A folder that is a git checkout

![Git state in a pane](images/01e-git-state.png)

Open a folder inside a checkout and a band appears above the listing. A folder that is
not in one looks exactly as it did before — the band is absent rather than empty, so
nothing takes height off the listing to say there is nothing to say.

The band reads left to right: **which branch**, **how much has changed**, and **the
commit you are on** — its short id, its subject, and how long ago it was made. The
subject gives way first and elides; the band stays one line.

Two of those are worth spelling out.

**When git is part-way through something, the state replaces the branch** — `rebasing`,
`merging`, `cherry-picking`, `bisecting`. During any of those the branch name is either
the old one or a detached head, and both readings are wrong about what is going on. A
detached HEAD says `detached at a1b2c3d`, because an empty branch name reads as a fault
in Mole rather than as a fact about the checkout.

**`2 ahead, 1 behind` is measured against the last fetch**, not against the remote as it
is now — nothing here touches a network. A branch with no upstream configured shows no
counter at all, rather than `0 ahead, 0 behind`, which would read as up to date when the
truth is that there is nothing to compare against.

### What the letters on the rows mean

| | |
|---|---|
| `M` | changed since the last commit |
| `A` | added, and staged |
| `??` | git has never been told about it |
| `R` | renamed |
| `D` | deleted |
| `U` | conflicted — a merge or a rebase stopped here |
| `•` | a folder: something inside it has changed, however deep |

A path can be several of those at once, and the row shows the most urgent. The letter is
the signal and the colour only agrees with it, so nothing here depends on telling two
colours apart.

**Files your `.gitignore` covers carry no letter and are not counted.** They are still in
the listing — Mole shows you your files — but a build directory would otherwise bury
every real change under thousands of marks.

One thing the letters cannot show: a file git calls **deleted** has no row, because a
listing shows what is on disk. The deletion is still in the count, and the folder that
held it carries the `•`.

### Keeping up, and what it needs

The band and the letters refresh themselves. An operation you run in Mole, and a commit,
checkout or pull you run in the terminal panel or in another window, are both noticed;
a burst of writes is collected into one re-read rather than one per file. A refresh
changes the marks and never the rows, so your cursor and anything you have ticked stay
where you put them.

**Mole shows git state and does not change it.** There is no staging, committing,
checking out or fetching here, and none is planned — see
[TODO.md](../../TODO.md) for why.

**Local drives only.** libgit2 needs a real path on a real filesystem, so a checkout
reached over SFTP or in a mounted archive shows no band. And all of this needs Mole to
have been built with libgit2: without it the window behaves exactly as it did before any
of this existed, which is to say there is no band and no letters.

## Breadcrumbs

![Breadcrumbs](images/01b-breadcrumbs.png)

The path is clickable all the way up. `Ctrl+G` turns it into a field you can type
into, and `Ctrl+←`/`Ctrl+→` walk the history you have already been through.
