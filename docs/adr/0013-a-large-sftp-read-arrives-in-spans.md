# ADR-0013: A large SFTP read arrives in spans, each over its own connection

- **Date:** 2026-08-09
- **Status:** Accepted

## Context

Copying large files off an SFTP drive did not work. The bytes arrived at full
speed and then stopped dead, a little short of a gibibyte, with the connection
still open and the server still there. Two minutes later the stall guard gave up
and the read failed; before the guard was reached the interface simply sat
there. Whether it presented as "a timeout" or as "only part of the file" was a
matter of which layer the caller happened to be looking at.

What was measured against a real server -- OpenSSH 9.6, a 1.2 GB file, `curl
8.5.0` built against `libssh 0.10.6`:

| what was done | result |
|---|---|
| `curl`, one transfer, fresh process | 1,211,049,311 bytes, complete, 53 MB/s |
| `curl`, a listing and then the file | stops at 1,072,635,904 |
| `curl`, a small file and then the file | stops at 1,072,635,904, the same byte |
| Mole, pooled connection | stops at 1,071,513,600 |
| Mole, `CURLOPT_FRESH_CONNECT` | stops at 1,071,529,984 |
| Mole, a brand-new easy handle | stops at 1,071,529,984 |
| `scp`, any size | fine, always |

So it is not the connection cache and not the handle cache; a fresh everything
still stops. Every failure lands just short of 2^30 bytes, which is where an
OpenSSH server re-keys the session when the negotiated cipher has a block size
under sixteen bytes -- `chacha20-poly1305` here. `libssh` says nothing when it
happens: the trace ends with the transfer running and resumes with the stall
guard firing. OpenSSH's own client re-keys mid-transfer without trouble, which
is why `scp` is untroubled and why the fault belongs to the pairing rather than
to either end alone.

Enabling TCP keepalive was tried, on the theory that the server had stopped
hearing from us. It changes nothing, and could not: the socket is not idle, it
is mid-transfer.

## Decision

**A read larger than one span is fetched a span at a time -- 256 MiB each, by
byte range, each over a connection of its own -- and appended to the same local
copy.** No connection is asked to carry enough for the re-key to arrive.

**A read that fits in one span is fetched exactly as before**, over a pooled
connection, in one transfer.

**`IFileSystem::openRead` gained an `expectedSize` hint** so a backend can tell
those two apart. It is documented as a hint, every backend except SFTP ignores
it, and -1 means "no idea". `TransferTask` and `SyncTask` pass what their plan
already measured; everything else says nothing and gets the careful path.

**Unknown counts as large.** A caller that did not say is reading one file for a
preview or a checksum, where an extra handshake is not worth saving.

## Reason

**Why spans rather than resuming after the stall.** Resuming would work -- the
short-transfer check added in [ADR-0012](0012-a-log-you-can-turn-up.md) makes
the trigger available -- but each attempt would first have to sit through the
stall guard. Two minutes per gibibyte is most of an hour on a twenty-gigabyte
file, spent waiting for something already known to be dead. Spans never reach
the fault, so nothing has to be recovered from.

**Why not simply a fresh connection per transfer.** It was tried and does not
work: a fresh connection stops in the same place. Even had it worked, an SSH
handshake costs about 0.58 s here, measured -- ruinous across ten thousand small
files, which is why the size hint exists rather than a blanket rule.

**Why 256 MiB.** Far enough below the fault that a server re-keying earlier is
still covered, and large enough that the handshake is noise: about half a second
per quarter gigabyte, under 1% of the transfer it carries.

**Why a hint on `openRead` rather than a `stat` inside the backend.** An SFTP
`stat` means listing the parent directory, which on a large directory costs more
than the transfer being set up. The callers that read in bulk have the number
already.

**Why not report it upstream and wait.** It should be reported. But the fault is
in a library shipped by the distribution, in the pairing of that library with
the most common SSH server there is, and a file manager whose answer to "copy
this file" is "wait for the next Ubuntu" is not a file manager.

## Consequences

- Large reads work. The 1.2 GB file that could not be read at all arrives in
  five spans in 65 seconds.
- A large read now shows five entries in the network log rather than one. This is
  visible in `MOLE_LOG=net` and nowhere else.
- Servers that ignore byte ranges are handled: a span that comes back longer than
  it was asked for means the whole file arrived, and the loop stops.
- **Uploads are not covered.** There is no range for a write, so a large upload
  gets a connection nobody has used and nothing more; past the re-key it will
  stall exactly as reads did. Recorded in TODO.md.
- **Staging is not addressed either.** `openRead` still downloads the whole file
  to a temporary file before the copy begins, so copying a 94 GB file needs 94 GB
  of free space in the temporary directory whatever the destination is. The span
  loop makes that staging survivable, not unnecessary. Also in TODO.md.

Both of those were taken up immediately afterwards, in
[ADR-0014](0014-remote-files-stream-rather-than-stage.md): reads and writes now
stream, and the span loop is how each of them fetches and sends.

## Amendment, 2026-08-19 (MOLE-99)

**A span that stalls is resumed from the byte it reached.** This reverses the
"Why spans rather than resuming after the stall" reasoning above; everything else
in this record still holds, and the span loop stays.

### What the spans did not cover

The span size was chosen as "far enough below the fault that a server re-keying
earlier is still covered". That is not true of a server whose `RekeyLimit` is
exactly `256M`: the re-key point falls *inside* the first span, the read dies
there, and every retry stops in the same place, 640 KiB short of 256 MiB. The
file cannot be read at all. Uploading the same file to the same server works, so
it is the read path alone.

**No span size is safe for every server.** The client cannot know what the
server's `RekeyLimit` is, and a smaller span only moves the wall — it does not
remove it, and it costs a handshake more often for every server that would not
have re-keyed anyway.

### The decision, and its price

A span that carried bytes and then stopped is fetched again from where it got to.
The original reasoning against this was that each attempt has to sit through the
stall guard first — two minutes per gibibyte, most of an hour on a twenty-gigabyte
file. That reasoning was right about the cost and wrong about the alternative:
the alternative is not "spans, which never meet the fault", it is "the file
cannot be read".

The price is real and measured. A gibibyte from the server whose limit is `256M`
arrives at 1.8 MiB/s, against 14.9 MiB/s from the server that does not re-key.
That is four stall-guard waits, and it is what the tier's own five-minute
per-function watchdog now has to be told about.

### What keeps it bounded

**Only a span that carried bytes is resumed.** One that carried none has nothing
to resume from and no reason to expect better, so the read fails there. Without
that rule the two cases are indistinguishable — a server that re-keys is one that
stops after delivering, and a link that has gone away is one that delivers
nothing — and a dead link would cost a stall-guard wait per attempt for ever.
With it, a dead link costs one stall and one attempt that gets nowhere.

The total is therefore bounded by the file: every resume must make progress, so
there can be no more of them than there are bytes.
