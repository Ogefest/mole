# Operations

The `F4` menu separates two kinds of thing, because they were once one heap and
finding anything meant reading all of it:

- **Operations** — do something to the files in front of you and hand you back to the
  listing.
- **Workflows** — open a tab that is a tool you then work in.

The question that decides which is which: does it *do something to the files*, or does
it *hand you a tool*? Anything that needs a tab of its own to be useful is a workflow.

The **File** section above them is a third thing: what you open from nothing. A browser,
a second browser, a search — the four entries that have a keyboard shortcut of their
own. Everything else is opened *onto* something, from Operations or Workflows, which is
why there is no *New Preview tab* offering a preview of no file. See
[ADR-0032](../adr/0032-a-feature-says-whether-a-new-tab-of-it-means-anything.md).

Everything in any of them is also reachable by typing — see [the palette](palette.md).

## Compressing

![The compress dialog](images/13-compress.png)

`Operations → Compress…` packs the ticked files and folders — or the row under the
cursor when nothing is ticked, and the folder you are in when there is no row at all —
into a new archive beside them. The dialog lists exactly what is going in, so it can be
checked before anything happens, and suggests a name from what is selected.

Zip, tar.gz, tar.xz, 7z, or a bare xz. Zip is the default because it is the one anyone
can open anywhere without being told how. A bare `.xz` is a single compressed stream
with no container, so it takes one file and no folders — the dialog says so rather than
failing when you press Ok.

Changing the kind changes the suffix and keeps the name you typed.

A zip can also be protected with a password, encrypted with AES-256. The box is offered
only for zip: a tar has no notion of a password, gzip and xz encrypt nothing, and the
7z writer here cannot encrypt either. Rather than accepting a password and quietly
ignoring it, those formats say why they cannot take one.

**Delete the originals when finished** does the second half of "archive this and get
it off my disk". It happens after the archive is written and closed, so there is never
a moment with the files gone and no archive; if anything could not be read the
originals are kept, because the archive is not a complete copy of them. The box is off
every time the dialog opens — archiving something once is not a standing instruction.

It runs in the background like every other job that takes time, and it can be
cancelled. A cancelled or failed compression leaves nothing behind: an archive that
exists is one that finished. One unreadable file is recorded and skipped rather than
sinking the whole archive.

It writes a new archive and nothing else — archive mounts stay read-only.

## What an operation is aimed at

Every dialog that deletes, overwrites or packs lists the files by name rather than
counting them, and the list is taken at the moment the question is asked:

![The delete confirmation](images/14-delete.png)

A count only confirms what you already believed. The list is there to catch the case
the dialog exists for — that ticks left over from another folder, or a cursor that is
not where you think it is, have aimed the operation at something else. Deleting
duplicates lists full locations instead of names, because inside a duplicate group
every name is the same.

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

## Duplicates

![Duplicates](images/19-duplicates.png)

`Find duplicates` from a folder looks for files that are the same, and lets you choose
what *the same* means: identical contents, or the same size, or the same name. Identical
contents is the default and does the work in the cheapest order — same size first, then
the first 16 kB, then a hash of the whole file — so almost nothing gets hashed.

What comes back is groups. The controls above them tick a whole answer at once — keep the
newest of each group, or the oldest, or the copy nearest the top of the tree — and
nothing is deleted until that is confirmed, with the files named.

## Syncing two folders

![A sync plan](images/15-sync.png)

`Sync folders` makes one folder resemble another, on any two drives. It **plans first and
shows the plan**: what would be copied, what would be replaced, what would be deleted, and
what it adds up to. Nothing happens until that is agreed to, and if the plan contains
deletions the confirmation says how many and is red.

Two directions and two temperaments: *update*, which only ever adds and replaces, and
*mirror*, which also removes whatever is not on the other side. Compare by size and
timestamp, or by contents when a timestamp cannot be trusted. Include and exclude
patterns, and an option to leave a file alone when the copy on the far side is newer —
the setting that stops a sync undoing today's work.

## Alerts

![Alerts](images/16-alerts.png)

An alert is a question about a folder or a drive asked repeatedly: *is this over ten
gigabytes*, *has this stopped growing*, *has this changed at all*. Each one has a target,
a metric and a bound, and each says when it was last checked and what the answer was.

They exist because the useful version of "the disk filled up" is being told beforehand.
An alert that goes outside its bounds says so in the window rather than only in this
list, and one that comes back inside them says that too — a warning nobody is told about
is not a warning, and one that never clears is one people learn to ignore.

## Saved reports

![Saved reports](images/17-reports.png)

Every analysis is kept, and this is where they are. Folders on the left, that folder's
runs on the right, because a report is worth far more as a series than as a snapshot:
one says how big a folder is, several say what is happening to it. Put a report on a
repeat from the analysis itself and the series builds without anybody remembering to.

## Jobs, watching and drives

![Scheduled jobs](images/08-automation.png)

Long jobs run in the background and report in the strip along the bottom; anything that
should happen regularly can be scheduled, and anything worth being told about can be
watched.

![A copy in flight](images/24-transfer-running.png)

While work is going the strip carries what it is, how far along, how fast, and — once the
speed has settled enough to be worth quoting — how much longer. Everything keeps working
while it runs: the strip is a report, not a modal.

![Drives](images/11-drives.png)

A local disk, an archive, an in-memory scratch space and a network share are all mounts
of the same interface, so every operation works the same on all of them.
