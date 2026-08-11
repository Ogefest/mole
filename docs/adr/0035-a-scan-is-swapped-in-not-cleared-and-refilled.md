# ADR-0035: A scan is swapped in, not cleared and refilled

- **Date:** 2026-08-11
- **Status:** Accepted

## Context

`ScanTask` rebuilt an indexed tree by emptying it first. `IndexDatabase::clearVolume()`
deleted every row the volume had, and then the walk put them back one batch at a
time. The comment above it said what it was for — *a rescan replaces the previous
contents rather than merging, so deleted files do not linger* — and that intent
is right.

The method was not. Between the delete and the last batch, the index held only
the part of the tree already re-walked, and a search over that volume was
answered from it: no error, no warning, no *still scanning*, just fewer results
than there are files. Rescanning a 4 TB tree takes hours, and the trees people
index are the large ones — so the window in which the index lies confidently is
not an edge case, it is the normal condition of a volume being kept current.

A cancelled or failed rescan was the same fault with a different ending: the
rows were gone and nothing put them back. So was one killed with the process,
where no error path runs at all.

## Decision

**A scan builds its rows beside the ones already there and swaps them in when it
finishes.**

Every row in `files` carries the `generation` of the scan that wrote it; every
row in `volumes` carries the one `generation` that is that volume's contents,
and a `next_generation` counter that hands out the next. A search joins the two
and reads only the rows where they agree.

- `beginScan()` takes the next generation and returns it.
- `insertBatch()` stamps its rows with it. They are in the table and in no
  answer.
- `commitScan()` drops every other generation, points the volume at this one and
  records `last_scan` — **in one transaction**, so a search sees the whole of the
  previous scan or the whole of the new one.
- `abandonScan()` throws away what an unfinished scan wrote, and refuses to touch
  the generation the volume is currently serving.

`clearVolume()` and `markVolumeScanned()` are gone: the first cannot be called
without reintroducing the fault, and the second was only ever the second half of
a commit.

## Reason

**A shadow table swapped in at the end** was the other shape that works, and it
was rejected for what it costs everywhere else. It doubles the schema — every
index, every query and every future column exists twice — and the swap is a
rename of two tables rather than one `UPDATE`. A column on the row buys the same
guarantee and is invisible to everything that does not care.

**Serving the half-built index with a warning** was rejected because there is
nothing useful to warn about. The user cannot tell which absent file is absent
because it was deleted and which because the walk has not reached it, and a
search that is sometimes complete is one nobody can rely on. (This is not the
same question as ADR-0005's *partly indexed folder*, where the answer really is
to put the provenance on the row: there the two sources are a live walk and an
old scan, and both are true. Here the second source is a scan that has not
happened yet.)

**Not deleting anything until the end, and de-duplicating on read** was rejected
because it makes every search pay for the arrangement, forever, to avoid one
`DELETE` that runs once per scan.

The sweep in `beginScan()` — dropping rows of any generation that is not the
visible one — is there for the scan killed with the process. Nothing runs
afterwards to clean up after it, its rows can never become visible, and without
the sweep a database would grow by one dead scan every time one is killed.

## Consequences

- The index schema is version 2. Both new columns default to nought, which is
  what every row and volume in an existing index already reads as, so an index
  built before this migration stays visible in full and its next rescan is the
  first to take a generation. `tst_IndexDatabase` builds a version 1 database by
  hand and opens it, because a migration cannot be tested against a database the
  migration built.
- A rescan now costs disk: both copies of a tree are in the file between
  `beginScan()` and `commitScan()`. For the sizes this is for, that is the price
  of the index never being wrong, and the peak is bounded by one scan.
- `fileCount()` reports what a search can reach, not what is in the table. A
  count that included a scan in progress would say the index holds files nothing
  can find.
- Two scans of the same volume at once are safe in the sense that matters — no
  search ever sees a half — but the later `beginScan()` sweeps away what the
  earlier one has written so far, and whichever commits last is the volume's
  contents. Nothing in the application starts two, and making that impossible is
  a job for whatever ends up scheduling scans.
- `last_scan` moves only on a completed scan, so *when this volume was last
  indexed* is now true rather than *when a scan was last attempted*.
