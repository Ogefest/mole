# ADR-0092: A symbolic link is copied as a link, and nothing is followed into one

- **Date:** 2026-09-03
- **Status:** Accepted

## Context

There was no written rule for what Mole does with a symbolic link, and the four
code paths that meet one each did something different:

- `TransferTask::planJobs()` asked `stat()`, and `isDir` is true for a link to a
  directory because Qt answers it through the link. So a linked folder was
  planned as a directory job: `makeDirectory()` at the far end and nothing put
  inside it, counted as transferred — and on a move the source link was then
  deleted. A link to a *file* was opened through the link and its target's bytes
  were copied, turning a reference into a duplicate; on a large target, into a
  full disk.
- `SyncPlan::walk()` recursed through a linked directory with no check and no
  cycle guard, so `a/loop -> a` planned a `CreateDirectory` at every level until
  the kernel refused the path — 123 steps in the test that now covers it.
- `DirectoryWalker` declined to descend into a link, but offered a
  `followSymlinks` option whose cycle guard was keyed on the uri. Every pass
  through a loop produces a new uri, so the guard detected nothing. No caller
  ever set it; the header's claim that "symlink loops are solved once" was true
  only because the option was dead.
- `FindDuplicatesTask` already left links out and counted them, which MOLE-341
  settled: a link has its target's size, hash and bytes, so it was confirmed as a
  duplicate of its own target and offering to delete "the copy" freed nothing or
  broke the link.

And two questions about the same name were answered differently: `list()` has
reported a dangling link since MOLE-333, while `stat()` used
`QFileInfo::exists()`, which resolves the link — so a link to nothing was "no
such file" when selected on its own and copied fine one level inside a tree.

## Decision

**A link is copied as a link.** One rule, in copy, move and sync:

- The link's target is read as the drive stores it and a link with that same text
  is made at the destination. Relative stays relative, absolute stays absolute,
  and a target that is not there is copied like any other — the link is the
  thing being copied, not what it points at.
- Nothing is followed into a link. A walk reports it and does not descend, in
  every walker and every plan, with no option to do otherwise.
- A destination that cannot hold a link refuses that one entry by name and says
  why, and the rest of the transfer goes. A move therefore leaves the link where
  it is, because a move deletes nothing while anything failed.

Two calls on `IFileSystem` carry it — `readLink()` and `makeLink()` — advertised
by `VfsCapability::Symlink`, with `VfsError::NotALink` for a name that is not
one. Local disk and the in-memory drive implement both; a mounted archive
implements `readLink()` alone — a link member can be read as a link and nothing
in an archive can be written — so extracting a tree of links puts links on the
disk rather than empty files. The protocol backends implement neither, so they
refuse links with a reason, which is what they were silently getting wrong
before. The shared conformance suite runs the round trip against any backend that
claims the capability and can be written to.

## Reason

**Why copy the link and not what it points at.** It is the only answer that
preserves what the user has. Following the link duplicates data (a 40 GB target
behind a 60-byte name), breaks the relationship the link existed to express, and
on a move destroys it — and it silently changes the size of what was copied,
which is why a full disk was one of the reported symptoms. `cp -P`, `rsync -l`
and `tar` all copy the link; a file manager that quietly does otherwise is the
odd one out. The alternative of following links behind an option was rejected
outright: it is a switch whose wrong setting costs a disk, and there is no
evidence anybody wants it.

**Why the target text is stored unresolved.** A relative link is relative on
purpose — it keeps pointing at its neighbour wherever the tree goes. Resolving
it on the way, which `QFileInfo::symLinkTarget()` does, pins the copy to the
machine it came from and turns a portable tree into one that only works here.
That is why `readLink()` uses `std::filesystem::read_symlink` and why the
conformance suite asserts the round trip with a relative target.

**Why a capability and a refusal, rather than a fallback.** The tempting
fallback is to copy the target's bytes when the destination has no links: it is
never what the user asked for, it is unbounded in size, and it is invisible —
exactly the fault this record exists to end. Refusing one entry by name is the
shape MOLE-333 already established for a pipe or a socket, and the shape the
name rules use for a name the destination will not take: one entry's failure,
never the run's, and never a silent substitution.

**Why a dangling link is copied like any other.** MOLE-333 refused it as "a link
to nothing", which was right when a copy meant reading through the link, and
there was nothing to read. Once the link itself is what travels there is nothing
to fail, and the exception would have to be written down and tested rather than
falling out. It also made `stat()` and `list()` agree about a name that is
plainly there.

**Why sync plans a `Link` step and not a `Copy`.** A `Copy` is executed by
streaming bytes, which is the very thing being ruled out. A link that is already
present at the destination is left alone with a reason instead: comparing a link
with whatever stands there is a question Mole cannot answer usefully yet, and
overwriting the far end on a guess is the worse of the two mistakes.

**Why `followSymlinks` is gone rather than fixed.** Fixing it means a node
identity every backend has to provide — an inode on local disk, and nothing at
all over SFTP or in a bucket. That is a real feature with a real cost, and it was
sitting behind a dead option whose guard did not work. Deleting it makes the
walker's promise true, and a future decision to follow links starts from an
honest place. ADR-0066's reasoning about `doNotQueryFrom()` applies here too: a
claim in a header that nothing tests is worse than no claim.

## Consequences

Copying a tree of links now reproduces the tree of links, so a copy onto a drive
that has none reports one failure per link rather than arriving wrong. That is
more visible failure than before and it is the point; a mirror to a bucket of a
tree that is mostly links will say so loudly.

`Job` inside `TransferTask` gained a `Kind` — `File`, `Directory` or `Link` —
replacing the `isDirectory` flag, because a link is neither of the other two and
a second bool would have allowed a state that means nothing.

Links are not compared. A sync that finds one already at the destination skips
it, so a link whose target changed in the source is not updated at the far end.
Doing better means reading both link targets and calling a difference in text a
difference in content, which is a small feature nobody has asked for; it is
recorded in TODO.md rather than guessed at here.

Nothing changes for the protocol backends' own behaviour: none of them ever made
a link and none of them claims to now. What changes is that they say so.
