# ADR-0029: A move deletes only what it finished moving

- **Date:** 2026-08-10
- **Status:** Accepted. Beside [ADR-0016](0016-a-copy-is-weighed-at-the-destination.md)
  and [ADR-0027](0027-a-read-that-ends-early-is-not-a-file-that-shrank.md), which
  decide whether a copy arrived; this one decides what may then be deleted.

## Context

A move is a copy followed by a delete, and the delete is the only operation in
this application that cannot be undone. `TransferTask` already refused to delete
anything when a transfer had recorded a failure. Two cases got past that rule,
both found by writing the tests for MOLE-8 rather than by anybody losing a file.

**A skipped file is not a failure.** With `Conflict::Skip`, a file whose name is
already taken at the destination is left alone and the job is reported as a
success — correctly, since skipping is what was asked for. Nothing was copied,
and the move then deleted the source. The file is gone, and the file now standing
under its name at the destination is a different file. The rename path, used when
both ends are the same drive, never had the fault: it skips the source and moves
on.

**A directory can be asked to move inside itself.** `mole:///work` moved into
`mole:///work/inner` plans finitely — the walk happens before a byte moves, so it
never meets what the copy is writing — and then deletes `/work`, which by then
contains the only copy of everything that was in it. Within one drive the rename
path refuses this by accident, because `rename(2)` returns `EINVAL`; across two
mounts of the same drive nothing refused it at all. Two mounts of one drive is
not a contrivance: a bookmarked folder and the disk it lives on are both mounts,
and nothing above the task knows they are the same place.

## Decision

**A source is deleted only when every job belonging to it was transferred.** Each
job carries the index of the requested source it came from; any job that ends
skipped or failed marks that source unfinished, and unfinished sources are left
alone. The existing rule — no deletes at all when anything failed — stays as the
outer guard.

**A directory whose destination is inside it, or is it, is refused before the
plan is built**, with `NotSupported` and a message naming the directory. It
applies to a copy as well as to a move: a directory that contains a copy of
itself is nobody's intention, and the next copy would contain that one.

**Containment is judged by uri, not by which backend object was passed in.**
Scheme, authority and a path prefix ending at a separator. Comparing the two
`FileSystemPtr`s would miss exactly the case that loses the data, because two
mounts of one drive are two objects.

## Alternatives

**Delete the source of a skipped file anyway**, on the grounds that the user
asked for a move and a file of that name now exists at the destination. Rejected:
the file at the destination is a *different file*, and the one being deleted is
the only copy. Nothing about "skip" says "discard".

**Detect the loop while walking**, by refusing to descend into the destination.
That keeps the copy from including itself but still leaves the move deleting the
source afterwards, and it answers a question about the plan with a check in the
walker, which every other caller of the walker would then pay for.

**Compare canonical paths through the backend**, so two different drives that
happen to share a path prefix are not confused. There is no such call in
`IFileSystem`, and adding one for this would make every backend implement a
resolver. The uri is the identity the rest of the application already uses.

## Consequences

A move of several sources where one is skipped now deletes the others and keeps
that one, which is what the counts already said happened. A directory can no
longer be dropped into its own subtree, and says why. Both are covered in
`tests/core/tst_MoveIsPermanent.cpp`, which fails on either fault being backed
out.
