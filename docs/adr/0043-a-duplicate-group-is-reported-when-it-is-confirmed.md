# ADR-0043: A duplicate group is reported when it is confirmed, and the list stays in order as it fills

- **Date:** 2026-08-18
- **Status:** Accepted

## Context

`FindDuplicatesTask` emitted its results once, at the end, with the complete
list, and `groups()` was documented *valid once finished*. On a large tree that
is a spinner and a silence for a long time, and then everything at once.

The information to do better had already been paid for. `IDuplicateStrategy` is
expressed as ordered stages, each narrowing the candidates, and it already
exposed `stageNames()` and `stageReadsContent(int)` — described in the header as
*used for progress and to explain what a scan is doing*. Nothing used them.

## Decision

**A group goes out when it is confirmed, not when the walk ends.** A group is
confirmed once it has agreed at every stage; nothing later in the scan can
withdraw it or add to it.

The mechanism is which stages run over everything and which run per bucket:

- The cheap stages stay breadth-first, over all candidates. Until one of them has
  run there are no buckets to work on, and running them over the lot is what
  tells the last stage exactly how many files it has to read.
- **The last stage runs bucket by bucket.** A bucket that survives it is a group
  nothing later can change, so it is announced then and there.

That is the whole of what makes results arrive early, and it costs nothing: the
expensive stage still only ever sees what survived the cheap ones, which is the
property the staged design exists for.

Nothing partial is ever announced. A row that appears and then vanishes is worse
than a row that appears late — it teaches people not to believe the list.

## Why inserted in place rather than appended and re-sorted

`groups()` returns largest reclaimable first, *because that is the order anybody
clearing space wants them in*. Arriving progressively and staying sorted are in
tension, and this is the resolution.

**A group is inserted in its place as it is confirmed, and rows move.** The task
hands the position out with the group, so the tab does not work it out twice.

Appending and re-sorting at the end was the alternative, and it loses for a
reason specific to what this change is for. Progressive results exist so somebody
can start reading the list *during* a long scan. Appending would leave the list in
arrival order for exactly that window — the whole of the scan — and then
rearrange it wholesale at the end, under the eyes of the person who had been
reading it. Insert-in-place moves rows too, but each move is local, and the
invariant the ordering was for — the biggest win is at the top — holds at every
instant instead of only at the last one.

Ticks are unaffected either way: a selection is keyed by uri, not by row.

## A scan that was stopped

Cancelling keeps every group already confirmed and takes none of them back. Each
of them agreed at every stage, and the scan stopping does not make that less
true — it only means there may be more.

It also says so. `wasCancelled` separates *nothing matched* from *stopped before
anything was found*: the first is a claim about the tree that only a scan which
ran to the end is entitled to make, and offering "try a different strategy" after
a scan that searched a tenth of a NAS would be answering a question nobody asked.

## Consequences

- `groupsReady(QList<DuplicateGroup>)` is replaced by
  `groupFound(DuplicateGroup, int position)`. One channel rather than two, so the
  list arriving and the list at the end cannot disagree.
- `groups()` is readable while the scan runs and is sorted at every instant.
- The status text says which stage is running and over how many files — "whole
  file: 87 of 412 files" — which is something somebody can decide to wait for.
- A single-stage strategy has no cheap stage to narrow with, so its one bucket is
  everything and its groups are all confirmed together. That is honest rather
  than a gap: with one stage nothing can be settled before the pass is over.
