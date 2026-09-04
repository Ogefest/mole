# ADR-0030: A source that cannot be read is not a source that is empty

- **Date:** 2026-08-10
- **Status:** Accepted. Beside
  [ADR-0029](0029-a-move-deletes-only-what-it-finished-moving.md), which decides
  what a move may delete; this one decides what a mirror may.

## Context

`SyncPlan` compares two trees a directory at a time. Both sides went through one
helper, and that helper answered a failed listing and an empty directory with the
same empty map. The comment above it said so on purpose:

> An empty result and an unreadable directory are deliberately the same here: a
> destination that does not exist yet reads as empty, which is exactly right for
> a first sync.

That is right about the destination and catastrophic about the source. A mirror
works out what to delete by asking what the source has and removing everything at
the far end that is not in the answer. When the answer is "nothing" because the
listing *failed* — a network hiccup, a permission, a drive gone to sleep, a
credential re-locked — the plan is to delete the entire destination, which is
frequently the only remaining copy.

Writing the test for it produced exactly that: three files at the destination, a
source whose listing was refused, and a plan with three deletions in it. Nothing
in the application would have stopped them.

## Decision

**A listing tells the caller whether it could be read.** The helper returns the
entries and a `readable` flag rather than only the entries.

**A source directory that could not be read produces no steps at all** — not a
copy, and above all not a deletion. The walk does not descend into it, so nothing
underneath it produces steps either. The rest of the tree is planned and carried
out as usual: one unreadable directory is not a reason to abandon a sync of
everything else.

**A destination directory that could not be read still reads as empty.** That is
the case the original comment was about, and it stays: a target that does not
exist yet is the ordinary first sync, and treating it as unknown would make the
first sync of anything impossible.

**Every unreadable directory is named in the plan, and the task reports each one
as a failure.** A sync that quietly skipped part of the source would leave
somebody believing the two trees now match — which is the same lie as the
deletion, arriving later.

**Cancellation counts as unreadable.** A cancelled listing is not a picture of
the source either, and a plan built from one must not act on the difference.

## Reason

**Abandon the whole plan when any directory fails.** Safe, and unusable: one
locked folder anywhere in a large tree would mean no backup at all that night,
and the failure would be indistinguishable from a broken configuration.

**Keep the deletions but require confirmation.** The interface is not always
there — the console runner and the scheduler both build plans — and a
confirmation that appears every time is a confirmation nobody reads.

**Compare against the previous run's listing** to notice that a directory used to
have contents. That is a database and a whole class of staleness bugs, to answer
a question the backend can already answer honestly.

## Consequences

A mirror is now safe against the failure mode that matters most, and says what it
could not see. `SyncPlan::unreadable()` is part of the plan, so the preview can
show it as well. Covered in `tests/core/tst_Sync.cpp` by two cases — a source root
that cannot be listed and one directory inside it that cannot — both of which plan
deletions again the moment the guard is backed out.
