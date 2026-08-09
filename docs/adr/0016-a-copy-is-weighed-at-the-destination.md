# ADR-0016: A copy is weighed at the destination before it is called done

- **Date:** 2026-08-09
- **Status:** Accepted

## Context

Everything a copy knew about its own success came from its own side of the wire.
Bytes were handed to a backend, the backend did not complain, the stream closed
without an error, and the file was counted as copied. Every check added recently
-- the short-transfer check in
[ADR-0012](0012-a-log-you-can-turn-up.md), the stream that refuses to call an
early end an end of file -- is still our account of what we sent, not the
destination's account of what it holds.

The failures being chased were all of the same shape: a transfer that stopped
early and reported success. It is worth asking the destination.

## Decision

**When every file has been written, each one is weighed where it landed**, and a
file whose size there is not the number of bytes that went into it fails the
copy. A file that is not there at all fails it too.

**By listing each destination directory once, not by asking about each file.** A
`stat` is a round trip, and on SFTP a `stat` *is* a listing of the parent -- so
ten thousand files in one directory would mean ten thousand listings of it. One
listing answers for everything that landed in it.

**Before a move deletes anything.** A move whose destination lost bytes must not
be the thing that destroys the only good copy.

**A check that cannot be run is not a failure.** If the directory cannot be
listed, that is logged and the copy stands on the evidence it already has.
Reporting a copy as failed because the checking failed is its own kind of lie.

## Reason

**Why sizes and not checksums.** A checksum would mean reading back every byte
that was just written -- doubling the cost of every copy -- and for a remote
drive it proves what the drive says about bytes it just accepted, not what is on
its disk. The size is what the destination independently reports, it is free
alongside a listing that is happening anyway, and it catches the whole class of
failure actually seen: transfers that stop early.

**Why at the end rather than after each file.** One listing per directory instead
of one per file, and it keeps the check off the path of the copy itself.

**Why not optional.** A check that has to be switched on is a check that is off
when it matters. The cost is one listing per destination directory, against a
copy that has just moved the contents of them.

## Consequences

- A copy that quietly lost bytes now fails, with both numbers in the message:
  "4000 bytes were sent but 1000 arrived".
- A move keeps the original when the check fails.
- One extra listing per destination directory, and a "Verified" count on the
  task alongside the file count.
- The check is only as good as the sizes a backend reports in a listing. Every
  backend here reports them; one that did not would make this vacuous rather
  than wrong.
