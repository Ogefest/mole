# ADR-0087: A drive is asked to replace, rather than told how

- **Date:** 2026-09-03
- **Status:** Accepted. Extends [ADR-0021](0021-the-working-name-is-not-only-for-servers.md),
  which stands.

## Context

ADR-0020 gave an unfinished write a working name and ADR-0021 extended it to the
local disk, so that an interrupted copy leaves the file it was replacing alone.
Both were about the *write*. Neither said anything about the moment the finished
write is put in place, and two things in that moment turned out to undo them.

The first was in `TransferTask::resolveConflict()`. With `Conflict::Overwrite` it
removed the destination and *then* opened the write. So the working name was
handed a destination that had already been deleted: `PartialFile` asks whether
the target existed when the write began, found nothing, and concluded it was not
replacing anything. A copy that failed half way — a dropped connection, a full
disk, a cancel — left neither the old file nor the new one, which is the case
ADR-0021 was written against in its own words: *"a copy over a file of the same
name, which is what a re-run of a failed copy is"*. `SyncTask::copyOne()` did not
pre-delete and relied on the backend replacing on commit, so the product's two
copy engines disagreed about the same situation.

The second was in `commitPartialWrite()` itself. It put a finished write over an
existing destination by calling `remove()` and then `rename()`, because
`IFileSystem::rename()` refuses an occupied name — and it has to, since a rename
that silently destroyed a file nobody mentioned is how the only copy of something
goes. Between those two calls there is an instant at which the name has nothing
at it at all. `rename(2)` has replaced a file with a file in one step since the
seventies; the interface simply had no way to say so.

## Decision

**`IFileSystem` gains one method: `replace(from, to)`.** It puts `from` at `to`,
replacing whatever is already there, and it is a separate method rather than a
flag on `rename()` because the caller saying *which* it means is the whole point.
`rename()` goes on refusing an occupied destination.

The default implementation is what every caller was doing by hand — remove the
destination, then rename onto the free name — so a backend that says nothing gets
exactly the behaviour it has today. `LocalFileSystem` overrides it with
`std::filesystem::rename`, which the standard requires to behave as POSIX
`rename` does — replacing the destination rather than refusing it — and which
MSVC, the compiler the Windows build uses, implements with `MoveFileEx` and
`MOVEFILE_REPLACE_EXISTING`. One step, with no instant in between.
Across two kinds — a directory arriving over a file — no filesystem can do it in
one step, and there the local backend falls back to the interface's two, because
there is no atomicity available to lose.

**And `resolveConflict()` stops removing what the arrival can be put over.** For a
file going over a file it leaves the destination standing and lets the write
replace it on commit, which every backend's `openWrite()` already has to honour
for Sync. It removes first only when the thing in the way is of the other kind.
The same-backend move shortcut renames rather than writes, so it is told which
case it is in and calls `replace()` instead of `rename()`.

## Reason

**Why a second method and not `rename(from, to, allowReplace)`.** A boolean at a
call site reads as a detail; a different method name reads as a decision. The
interface's own history is the argument: `commitPartialWrite()` already carried a
`mayReplace` flag whose meaning had to be explained in a paragraph, and the
paragraph is there precisely because a flag cannot say it. There is also a
mechanical reason — this interface warns, in as many words, that a default
argument binds to the static type, and a decorator that declared the override
without it would compile and quietly pass the wrong value to the drive.

**Why the default is the two-step rather than `NotSupported`.** Every protocol
backend already does exactly this and cannot do better: SFTP and WebDAV's `MOVE`
refuse an occupied name outright, and one or two FTP servers do something worse
than refuse. Making them all implement a method to keep the behaviour they had
would be six identical copies of the same six lines, and the seventh backend to
arrive would get it wrong. The one backend that can do better overrides it; that
is what a default is for.

**Why `std::filesystem::rename` and not `::rename` under an `#ifdef`.** It is the
same call on POSIX and the right call on Windows, and Mole has no Windows machine
to verify a hand-written `MoveFileExW` on (MOLE-253). One line that both
platforms' standard libraries are on the hook for beats two that only one of them
is ever exercised.

**Why not simply let `rename()` replace when the destination exists.** That is
the behaviour `LocalFileSystem::rename()` was deliberately given a guard against,
and the guard is load-bearing: a rename is also what a user typing a new name in
the pane performs, and there it must refuse rather than overwrite a neighbour.

**Why the kinds are checked before the call rather than read out of the error.**
`rename(2)` reports a directory standing where a file is arriving as `EISDIR`,
`ENOTDIR` or `ENOTEMPTY` depending on which way round it is, and Windows reports
it as access denied. Falling back on an error code means falling back on the
platform's opinion, and a wrong guess would retry — as two destructive calls — a
case that had genuinely failed.

## Consequences

An interrupted overwrite now keeps the file it was replacing, on every drive: the
transfer no longer deletes it, and the backend puts the replacement over it only
once every byte has arrived. On a local disk there is additionally no instant at
which the name is unoccupied, so a process killed at the worst possible moment
finds a whole file there rather than none.

The conformance suite asks every backend for `replace()` and holds the outcome
rather than the method, so a drive that can do it in one step and one that cannot
are both correct and both checked. `tst_WrappedDriveAnswers` holds the new method
like the rest: a decorator that forgets to forward it fails, which matters here
more than usual, because the default is plausible enough to pass for an answer.

A backend author has one more method to know about, and the price of not knowing
is only that a replace costs two calls instead of one. Nothing above the VFS
layer learns which drive can do which: the callers ask to replace, and the drive
answers with what it has.
