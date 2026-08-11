# ADR-0038: A mixed list says which rows are remembered

- **Date:** 2026-08-11
- **Status:** Accepted
- Supersedes the *partial coverage counts as no coverage* rule of
  [ADR-0005](0005-which-engine-answers-a-search.md); the rest of that record,
  including the toggle and the rule that the engine is always named, stands.

## Context

A folder with an indexed subtree in it was treated as if nothing had been
indexed. ADR-0005 said so plainly, and gave a good reason:

> the result would be one list of which some rows are current and some are as
> old as the last scan, with nothing on the row to say which — an answer nobody
> can reason about.

Two things make that worth revisiting. The first is that partial coverage is the
**ordinary** case, not an edge one: people index the big slow tree, not the disk
it sits on, so the folder above an indexed archive is exactly what somebody
searches. The second is that the objection contains its own answer — *nothing on
the row to say which*. Put it on the row.

## Decision

**Both halves answer, and every row says which half it came from.**

- The index answers first, for whatever it holds under the folder. Those rows
  appear at once, each marked as coming from a scan, carrying the date of the
  scan that recorded it — per volume, because two volumes under one folder were
  scanned at different times.
- The walk then goes over the whole folder, indexed subtree included, and
  **supersedes** what the index said:
  - a path it finds replaces the row already there, in place, and that row stops
    being marked as remembered;
  - once it has listed a directory it knows everything in it, so a row the index
    claimed there and the walk did not match is **withdrawn** — deleted since
    the scan, or no longer matching.
- The status line accounts for both halves while it runs — *420 from the index,
  scanned 3 days ago · 1,204 matches / 88,000 scanned* — and says *every row
  current* when the walk finishes having left nothing remembered.

**The marking is the feature, not a decoration on it.** A build that mixed the
two halves without saying which is which would be the thing ADR-0005 refused,
and every test in `tst_MixedSearch` asserts a row's provenance rather than only
a count.

Fully covered and not covered at all are untouched: the first is answered by the
index alone as before, the second is a walk with no marking anywhere.

## Reason

**The walk covers the indexed subtree too, rather than skipping it.** Skipping
is the faster shape and it was rejected: with no overlap the index's rows are
never checked, so a file deleted after the scan stays on the list for the whole
search and the supersede rule has nothing to act on. What the index buys here is
**time to the first answer**, not less work — the list is useful in a
millisecond and true by the end. When the folder is fully covered there is no
walk at all, and that is where not doing the work still lives.

**Provenance beside the row rather than on `FileEntry`.** Where a row came from
is a fact about how this list was built, not about the file; a backend has no
opinion on it and neither should the type every backend returns.

**Withdrawn rather than erased.** The results model holds rows as offsets into
the list of entries, because copying entries around made a streaming search
quadratic. Erasing one entry would strand every offset after it, so a taken-back
row is marked and skipped instead.

**No staleness heuristic.** Nothing here tries to guess whether an index is too
old to be worth showing. A folder that changes often is one somebody indexes
often, or does not index at all and searches directly; making the scan cheap and
regular is MOLE-155's business, and a threshold invented here would be a second
answer to the same question.

## Consequences

- `DirectoryWalker` gained a second callback: a directory, once its whole
  listing has been through the visitor. Only a caller reconciling an older
  record of a directory needs it, and it is what makes *this file is gone* a
  thing the walk can say the moment it can say it.
- A search over a partly indexed folder runs two tasks in sequence — the index,
  then the walk primed with what it found. The index task is instant by
  construction, so nothing waits on anything for long.
- A directory the walk cannot read leaves its indexed rows as the scan left
  them. The status line says so at the end rather than claiming the list is
  current, because that claim is the one thing this must never get wrong.
- Cancelling keeps what is on screen and says the search was stopped. Some rows
  may still be marked as remembered, which is exactly the true statement about a
  list the walk never finished.
