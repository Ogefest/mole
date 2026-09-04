# ADR-0020: An upload in progress wears a different name

- **Date:** 2026-08-10
- **Status:** Accepted. Extends [ADR-0014](0014-remote-files-stream-rather-than-stage.md),
  which stands.

## Context

ADR-0014 stopped staging remote uploads locally: the bytes now go to the server
as the caller writes them, which is what makes a copy of a file larger than the
local disk possible at all. It named the price and paid it — an upload that
*fails* leaves part of a file on the server, so the backend deletes what it
managed to write.

That answer covers every failure the process is alive to see. It covers none of
the ones it is not.

A process killed outright does not get to delete anything. `SIGKILL` runs no
destructor, no error path, no cleanup: whatever is on the server at that instant
stays there. The machine losing power, the OOM killer, a `kill -9` from someone
tidying up a stuck window — all of them leave exactly what had arrived so far,
under the name the transfer was going under.

And the name the transfer was going under was the name the user asked for. So a
hundred-megabyte file interrupted at forty megabytes became a forty-megabyte
file called `backup.tar`, indistinguishable from a backup that was simply that
size. Nothing downstream can tell: not a sync comparing sizes, not a checksum
nobody has, not a person looking at a listing. It is the same fault
[ADR-0016](0016-a-copy-is-weighed-at-the-destination.md) is about, arriving from
the other direction.

## Decision

**The protection cannot be an action taken afterwards, because there is no
afterwards. It has to be the name the bytes were travelling under all along.**

An upload is written to `<name>.mole-partial` and renamed onto `<name>` once
every byte has arrived. What a kill leaves behind is therefore always visibly
unfinished, whatever moment it lands in, and nothing had to survive the kill for
that to be true.

The rename is one server-side operation — `SSH_FXP_RENAME`, `RNFR`/`RNTO`,
WebDAV `MOVE` — and it is the only instant at which the file appears under its
real name.

Three things fall out of it, and each one is a decision rather than a detail:

- **The destination is checked, not assumed.** A copy clears the way before it
  starts, so the name ought to be free; ought is not is, and something may have
  arrived during the minutes the upload took. A rename that quietly replaced it
  would destroy data this transfer was never asked to touch. *Could not find
  out* is treated as *not free*, because guessing here is guessing about
  somebody else's file.
- **A commit that fails is an upload that failed**, however well the transfer
  went. Every byte arriving and the file not being there is not a success with a
  caveat; a caller told otherwise is being told a lie.
- **A failed upload still leaves nothing.** Bytes under a name nothing will ever
  open are litter, not a result, so the working name is removed on every failing
  path — including a rename that did not happen.

This applies to SFTP, FTP and WebDAV. **S3 is deliberately left alone**: a single
`PUT` is atomic at the object level and a multipart upload does not exist as an
object until `CompleteMultipartUpload`, so an S3 write killed half way through
leaves no object at all. Adding a rename there would mean copying the object to
a second key, which is the expensive way to buy something the protocol already
gives away.

## Reason

**A hidden name — `.mole-partial-<name>` — rather than a suffix.** Rejected
because it hides the wreckage from exactly the person who needs to see it. The
point is that somebody looking at a listing can tell what happened; a leftover
nobody notices is a leftover nobody deletes, and on a shared server it
accumulates.

**A bookkeeping file recording uploads in flight, swept on the next start.**
Rejected because it is the same bet that just lost: it needs the process to
survive long enough to write it, and would itself be truncated by the kill it
exists to survive. The name needs nothing to survive — it is already on the
server, in the only place the fault happens.

**Uploading to a temporary directory and moving files out of it.** Rejected
because a rename across directories is not guaranteed atomic on every backend,
and because it invents a second place for a file to be lost.

**Leaving it, and checking sizes afterwards.** Rejected: the size is often not
known before the transfer (a stream has no length), and a check that happens
after the process died is a check that does not happen.

## Consequences

- Every remote upload costs one extra round trip: a `stat` of the destination
  and a rename, against a transfer that has just carried the whole file. On a
  directory of ten thousand small files this is not free, and it is the one
  place to look if bulk writes get slower.
- A `.mole-partial` file on a server is now a diagnosis rather than a mystery:
  something was killed mid-transfer, and the file it was writing is that one
  minus its suffix. **Removing them is a manual job today** — see
  [TODO.md](../../TODO.md).
- A destination that fills up now fails at the rename rather than the write, so
  the error text a user sees for a full disk changes shape.
- Anything that lists a remote directory will show these. That is intended, and
  it is why the suffix is visible rather than hidden.
