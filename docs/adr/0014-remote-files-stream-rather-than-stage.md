# ADR-0014: A remote file is streamed, not staged in a temporary file first

- **Date:** 2026-08-09
- **Amended:** 2026-08-10 — FTP writes stream now. See *Amendment* at the end.
- **Status:** Accepted

## Context

Every network backend read a remote file by downloading the whole thing into a
temporary file and handing back that file, and wrote one by collecting the whole
payload into a temporary file and sending it on `close()`. Both were deliberate.
The staged read bought random access for the preview layer; the staged write is
genuinely required by S3, which must sign a length it does not yet know, and by
WebDAV servers that handle a chunked `PUT` badly.

The arithmetic stops working at scale, and the files in question are backups:
94 GB, 20 GB, 9 GB, several at 4 GB. Copying a 94 GB file to a drive with plenty
of room needed 94 GB of free local scratch space, on a machine with 84 GB free --
so the copy was not slow, it was impossible. Between two remote drives it needed
that twice, once on the way in and once on the way out. Nothing reported progress
while it happened, because as far as the copy was concerned nothing had started
yet. And a preview of a large remote file downloaded all of it in order to show
the first page.

## Decision

**SFTP reads and writes stream.** A read above 64 MiB returns a device that
fetches from the server as the caller reads; a write returns a device that sends
as the caller writes. Both are still fetched and sent in spans, for the reason
in [ADR-0013](0013-a-large-sftp-read-arrives-in-spans.md).

**A read at or below 64 MiB is still fetched whole into a temporary file.**
Random access is free that way, the temporary file is small, and it is the path
the preview layer mostly travels.

**The transfer runs on a thread of its own, with a bounded buffer between it and
the caller** -- 8 MiB, which is the entire memory cost of a copy of any size.
libcurl pushes bytes at a write callback at the server's pace and a `QIODevice`
is asked for them at the caller's; the buffer is what lets each go at its own
speed. A full buffer blocks the transfer, an empty one blocks the reader, and
abandoning either end stops the transfer rather than waiting for it.

**A streamed read still knows how long the file is**, asking the backend when the
caller did not say. A device that cannot say where it ends breaks everything that
asks, and a stream that ends early must be an error rather than an end of file.

**S3, WebDAV and FTP keep staging**, because for the first two the protocol
requires it, and rewriting a backend that is not in the way would be change for
its own sake.

**A streamed upload that fails deletes what it wrote.** A staged upload left
nothing behind when it failed; a streamed one has already put part of a file on
the server, and a partial file that looks finished is the worst thing a copy can
leave. The target is always one the copy created -- a copy clears the way before
it starts -- so removing it takes nothing that was not ours.

## Reason

**Why a thread rather than libcurl's multi interface.** The multi interface would
avoid the thread, but it means reimplementing what `CurlPool::perform` already
does -- the error buffer, the byte accounting, the stall guard, the logging from
[ADR-0012](0012-a-log-you-can-turn-up.md) -- in a second place that would then
have to be kept in step. One thread per open stream, blocked most of the time, is
a smaller price than two transports.

**Why not stream everything, including small files.** Random access over a stream
means re-fetching whenever something seeks backwards. The preview layer seeks,
and it mostly opens small files. Below the threshold the staged copy is both
faster and simpler, and costs tens of megabytes of temporary space at most.

**Why 64 MiB.** Small enough that the temporary file is never an obstacle -- the
worst case is a directory of them -- and large enough that ordinary documents,
which is what gets previewed, stay on the simple path.

**Why the size is asked for when the caller does not know it.** The alternative
is a sequential device, and then `atEnd()` is false before the first read and
true after the buffer empties, which silently turns "copy this file" into "copy
nothing" for every caller written against a file. Paying one `stat` is better
than a class of bug that reports success.

## Consequences

- A copy of any size now costs about 8 MiB of memory per direction and no
  temporary space at all. Measured on a 1.2 GB file: not one byte of `/tmp`.
- A large copy shows progress from the first second, because bytes now move
  through the copy loop rather than into a temporary file first.
- A preview of a huge remote file reads the first page and stops.
- A remote-to-remote copy no longer stages the file twice, so SFTP to SFTP is
  possible at any size.
- `TransferTask` no longer decides "the file has ended" from `atEnd()`. It reads
  into a buffer and distinguishes 0 from -1, because a device being filled from a
  network has no other way to tell the end of a file from a failed read.
- An interrupted upload leaves nothing on the server, but only because the
  backend deletes it. A process that dies outright cannot do that; uploading to a
  temporary name and renaming on success would cover it and is in TODO.md.
- One thread per open remote stream, blocked on a condition variable almost all
  of the time.

## Amendment, 2026-08-10 (MOLE-34)

**FTP writes stream.** The paragraph above says S3, WebDAV and FTP keep staging,
"because for the first two the protocol requires it, and rewriting a backend that is
not in the way would be change for its own sake". The first half still holds. The
second was a decision to defer, not a decision to stage, and it has expired: FTP was
the last backend that could not write a file bigger than the local disk, and being the
last one is itself the reason to finish.

It uses the same `StreamingUpload` as SFTP, under the same working name, with `APPE`
for anything past the first span. The span is a ceiling rather than a working figure:
the span loop exists for SFTP's re-key fault, which FTP does not have, and a login and
a data channel per span would buy nothing here.

**FTP reads still stage,** and that is now the only place a file bigger than the disk
is still a problem. Streaming a read needs ranged fetches, and what libcurl's
`CURLOPT_RANGE` does to an FTP transfer — whether the end of the range is honoured or
only the `REST` offset — decides whether a span can overrun into the next one and
deliver the same bytes twice. That is not a thing to settle by reading documentation
and hoping, and there is no FTP server in the build environment to settle it against,
so it is tracked as its own task rather than guessed at. A read that quietly duplicates
a span is worse than one that needs scratch space.
