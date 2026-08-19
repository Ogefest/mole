# ADR-0057: A volume records the scan that built it, and an older one answers "not known"

- **Date:** 2026-08-20
- **Status:** Accepted

## Context

An indexed volume recorded that it had been scanned and when, and nothing about
what kind of scan it was: `IndexVolume` carried `id`, `rootUri`, `label`,
`lastScan` and `fileCount`. So nothing could answer whether an index has
metadata in it, or whether its archives were opened, and a re-scan could not
default to the choices the first scan was given — which is how a tree ends up
indexed one way on Monday and another way on Tuesday.

Two indexes over two trees, one of which can answer `image.camera` and one of
which cannot, are different things and were indistinguishable. That is the first
column anybody looking at a list of their indexes wants.

There was a proxy and it was not enough. `IndexDatabase::factKeys(volumeId)`
answers which facts a volume's files were recorded as stating, so a volume with
none was probably scanned without the reader. Probably: it cannot tell *scanned
without metadata* from *scanned with metadata over files that carry none* — a
tree of `.tar.gz` files answers the same either way — and it says nothing at all
about archives, because an archive that could not be opened and an archive nobody
looked inside both leave no rows.

## Decision

The three `ScanOptions` booleans are stored on the volume row, added by migration
5, and `IndexVolume` carries them back out as `std::optional<ScanOptions>`.

**Written at `commitScan()`, not at `beginScan()`.** Only a finished scan's
options describe what is in the volume. A scan that is abandoned or killed leaves
the volume exactly as it was, options included, the way it already leaves
`last_scan` alone — that is the property the generation swap exists for and it
does not gain an exception.

**All three columns are nullable, and absent means *not known*.** A volume
written before this migration has no recorded options, and the honest answer for
one is that it cannot be told, not that it has no metadata. They are written
together by one finished scan, so the whole thing is optional rather than three
tri-states: a volume either remembers what it was asked for or it does not.

## Reason

The alternatives, and what disqualified them:

- **Keep inferring from `factKeys()`.** It cannot distinguish the two cases above
  and cannot speak about archives at all. An answer that is right most of the
  time is worse here than no answer, because a list of indexes is read as fact.
- **Default the new columns to 0 rather than leaving them null.** One line
  shorter and it tells every existing user that the index they built with
  metadata last week has none. A wrong answer displayed confidently is the
  failure mode this whole epic exists to remove.
- **Three separate tri-states.** Nothing can write one option without the other
  two, so a shape that allows two known and one unknown describes a state that
  cannot occur.
- **Record the options at `beginScan()` so a running scan can be described.**
  It would mean a killed scan leaves its options behind on a volume whose
  contents came from the previous one — exactly the kind of half-applied state
  the generation swap was built to make impossible.

## Consequences

`commitScan()` takes what the scan was asked for, so every caller states it.
That is four lines across the tests and one in `ScanTask`, and it means the
volume row cannot silently drift from the scan that wrote it.

A list of indexes can now say which of them can answer a question about a camera
— that is what MOLE-230 and MOLE-231 are built on. Anything reading the options
has to handle *not known* as a third answer rather than folding it into "no",
and the interface has to say so in words rather than leaving a blank column.

One rescan replaces the unknown with the truth, so the state is temporary for
anybody who keeps using their index, and permanent only for a volume nobody ever
scans again.
