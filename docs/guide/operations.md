# Operations

The `F4` menu separates two kinds of thing, because they were once one heap and
finding anything meant reading all of it:

- **Operations** — do something to the files in front of you and hand you back to the
  listing.
- **Workflows** — open a tab that is a tool you then work in.

The question that decides which is which: does it *do something to the files*, or does
it *hand you a tool*? Anything that needs a tab of its own to be useful is a workflow.

Everything in either is also reachable by typing — see [the palette](palette.md).

## Compressing

![The compress dialog](images/13-compress.png)

`Operations → Compress…` packs the ticked files and folders — or the row under the
cursor when nothing is ticked, and the folder you are in when there is no row at all —
into a new archive beside them. The dialog lists exactly what is going in, so it can be
checked before anything happens, and suggests a name from what is selected.

Zip, tar.gz or tar.xz. Zip is the default because it is the one anyone can open
anywhere without being told how.

A zip can also be protected with a password, encrypted with AES-256. The box is offered
only for zip: a tar has no notion of a password and gzip and xz encrypt nothing, so
rather than accepting one and quietly ignoring it, those formats say why they cannot.

It runs in the background like every other job that takes time, and it can be
cancelled. A cancelled or failed compression leaves nothing behind: an archive that
exists is one that finished. One unreadable file is recorded and skipped rather than
sinking the whole archive.

It writes a new archive and nothing else — archive mounts stay read-only.

## Renaming in bulk

![Bulk rename](images/07b-bulk-rename.png)

Rules on the left in the order they apply; every file's before-and-after on the right,
updating as you type. The preview is the feature: Apply refuses outright while any row
would collide, because a batch that half-succeeds is worse than one that never ran.

## A terminal, here

![The terminal panel](images/10-terminal.png)

`` Ctrl+` `` opens a shell in the folder you are looking at, along the bottom. It takes
the keyboard when it opens, so you can start typing; every key goes to the shell,
including the ones the window would otherwise claim. `Ctrl+D` ends the shell and the
panel goes with it, as it does anywhere else.

Navigating afterwards does not drag the shell along — a shell has its own idea of where
it is, and fighting that is worse than leaving it alone.

## Analysing a folder

![An analysis](images/07-analysis.png)

`Ctrl+Shift+A` walks a tree once and reports what is in it: what is big, what is old,
what the extensions are, which subfolder is responsible for the bulk. Reports are kept,
so two of them can be compared later and the listing marks folders that have one.

For the quick question — *which of these five folders is the big one* — use
`Ctrl+Shift+S` from [browsing](browsing.md) instead. It answers in the listing without
opening anything.

## Jobs, watching and drives

![Scheduled jobs](images/08-automation.png)

Long jobs run in the background and report in the strip along the bottom; anything that
should happen regularly can be scheduled, and anything worth being told about can be
watched.

![Drives](images/11-drives.png)

A local disk, an archive, an in-memory scratch space and a network share are all mounts
of the same interface, so every operation works the same on all of them.
