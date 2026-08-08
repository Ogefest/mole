# ADR-0009: Packing can delete the originals, after the archive exists

- **Date:** 2026-08-09
- **Status:** Accepted

## Context

"Archive this and get it off my disk" is one operation to a person and two to a file
manager. Doing it by hand means packing, then finding the same selection again, then
deleting it — and the middle step is where the wrong thing gets deleted.

The reason not to offer it is obvious: it is the only feature here that deletes data
as a *side effect* of something else. So the question is not whether it can be
offered but under what rules it can be trusted.

## Decision

**A checkbox on the compress dialog, off every time it opens.** Never remembered.
Somebody who archived something once and dropped the originals has not thereby asked
to do it again next time.

**The deletion happens after the archive is written and closed**, never as part of
writing it. There is no window in which the originals are gone and the archive is not
yet there.

**Nothing is deleted if anything could not be read.** One unreadable file inside a
packed folder means the archive is not a complete copy of it, and the originals are
the only place the missing file exists. The archive is kept, the sources are kept, and
the status says which it was.

**Nothing is deleted if the job was cancelled**, even if the archive survived.

**A source that contains the archive is never deleted.** Packing the folder you are
standing in writes the archive inside it, so deleting that source would take the
archive with it — turning "keep the archive, drop the files" into keeping nothing at
all. This is the case that makes the feature dangerous, and it is reported rather than
skipped in silence.

**A deletion that fails is reported, not fatal.** The archive is written and correct,
which is the part that cannot be repeated; a file that would not delete is something
somebody can retry.

## Reason

Every rule above is the same rule: the archive is the only copy once the originals go,
so nothing goes until the archive is provably a complete copy. "Provably" is doing the
work — the task already records unreadable entries, so it can answer the question
rather than assume the answer.

Moving to a trash instead of deleting was considered. It would soften every case above,
and it was rejected for now because it is a larger decision than this one: it belongs
to *all* deletion in the application, not to compression, and half of it — a trash that
some drives have and others do not — is worse than none.

## Consequences

- `CompressTask::Request::removeSourcesWhenDone`, with `removedSources()` reporting
  exactly what went, so the shell can announce each removal and other panes stop
  showing files that are not there.
- Three tests cover the three refusals — unreadable, self-containing, and not asked
  for — because the guards are the feature. Removing either guard makes its test fail
  by deleting real data in the temporary tree, which is how they were checked.
- The dialog says what will happen while the box is ticked, in the same place it lists
  what is going in — see [ADR-0008](0008-naming-what-an-operation-touches.md).
