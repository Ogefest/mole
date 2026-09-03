# ADR-0088: A merge is a move of the children

- **Date:** 2026-09-03
- **Status:** Accepted. Extends [ADR-0029](0029-a-move-deletes-only-what-it-finished-moving.md),
  which stands.

## Context

`ARCHITECTURE.md` has said from the beginning that an existing target directory
is a merge rather than a conflict, and the copy path does exactly that. The move
path did not, in two different ways, and which of them a user got depended on
something they cannot see.

Across two drives, `resolveConflict()` reported the merge by setting `*skip`, and
`transferOne()` turned that into `Outcome::Skipped`. ADR-0029's rule — any job
that ends skipped or failed marks its source unfinished, and an unfinished source
is not deleted — then applied to the merge as well as to a real skip. So moving
`A/` onto an existing `dest/A/` copied every file, reported "N files
transferred", recorded no failures, and deleted nothing. A move that behaved as a
copy, silently.

Within one drive the rename shortcut called `resolveConflict()` with
`isDirectory=false` for every source, so the merge branch was unreachable there
and a directory fell into the conflict switch instead: `Fail` recorded "already
exists" where the other path merged, `Skip` skipped the whole tree, and
`Overwrite` recursively deleted the existing destination folder — including
everything in it the source does not have — and renamed the source onto the name.

## Decision

**A merge is its own outcome.** `Outcome::Merged` is neither a transfer nor a
skip: nothing was copied for the directory itself, and nothing about it says the
source has not moved. It does not mark the source unfinished, so a merge that
went cleanly finishes as a move and the source folder goes.

**A merge is a move of the children, so a skipped child is what stays.** When a
source only partly arrived — which, after the outer guard, can only mean a child
was skipped — the entries that did arrive are removed one at a time, deepest
first and non-recursively, and a directory that still holds something keeps it.
Removing the tree would take the skipped file with it; leaving the tree would
mean nothing moved at all.

**The rename shortcut is not asked to merge.** It stats each source, so it knows
what each one is, and when any of them would land on a directory of the same name
the whole request goes to the generic path.

## Reason

**Why the whole request and not that one source.** The plan is built from the
request rather than from what is left of it, so a source the shortcut had already
renamed would be planned a second time and reported missing. Handing one source
over would mean the shortcut and the plan sharing a notion of "already done",
which is a second piece of state for two paths that already disagreed once.
The cost is a batch containing one merging folder losing the shortcut for the
rest, which is slower and not wrong.

**Why per-child deletion rather than leaving the whole source.** ADR-0029 already
says, in its own consequences, that "a move of several sources where one is
skipped now deletes the others and keeps that one". A merge *is* that selection,
one level down: the user pointed at a folder and every file in it got its own
answer. Keeping the whole tree because one file was skipped would be a different
rule for the same situation, decided by whether the files were selected
individually or by their parent.

**Why the safety rule is untouched.** Nothing that did not arrive is deleted, and
nothing at all is deleted when anything failed — the outer guard is still the
first question asked. This decision only refines *what* gets removed for a source
that partly arrived, from "nothing" to "what arrived".

**Why not make the shortcut merge.** A rename cannot merge; it can only replace.
Teaching it to fall back to a per-file rename of the directory's contents would
be a second copy engine inside the fast path, and the generic path already knows
how to do it.

## Consequences

Moving a folder onto a folder of the same name now finishes, and finishes the
same way whether the two ends happen to share a `FileSystemPtr`. `Overwrite` no
longer deletes what the destination folder holds and the source does not, which
was the one outcome here that lost data nobody had mentioned.

A move that merges pays a stat per source before the shortcut is taken, and gives
the shortcut up entirely when any source would merge. Both are cheap next to what
the generic path then does.

`tests/core/tst_MoveIsPermanent.cpp` holds all three: the merge that finishes, the
merge with a skipped child, and the merge within one drive that keeps what the
destination already had.
