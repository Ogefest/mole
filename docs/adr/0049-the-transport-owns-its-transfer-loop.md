# ADR-0049: The transport owns its transfer loop, so a guard is Mole's decision

Date: 2026-08-19

Status: accepted

## Context

`CurlPool::perform` called `curl_easy_perform`, which blocks until libcurl
decides the transfer is over. Every mechanism Mole had for bounding a transfer
was therefore an attempt to influence a loop it did not own — and on SFTP all
three attempts missed:

- `CURLOPT_LOW_SPEED_LIMIT`/`TIME` never reached a verdict on that protocol at
  all.
- `net::StallWatch` returned *stop* from the progress callback at the time it was
  set to, and `curl_easy_perform` went on running. What it actually did was
  relabel the error afterwards, once something else had ended the transfer.
- The something else was `TCP_USER_TIMEOUT` on the socket. **The kernel decided
  when a Mole transfer gave up, and by how much.**

That was measured on MOLE-108 against a link cut in the middle of a download: the
progress callback fired twice a second throughout, with the byte count frozen, and
returning *stop* from it did not end the call. **The callback firing is the
important half of that observation** — libcurl was returning to its own loop and
consulting Mole all along. Being told to stop simply did not end the wait.

## Decision

`CurlPool::perform` drives the transfer itself: `curl_multi_add_handle`, then a
loop of `curl_multi_perform` and `curl_multi_poll` with a poll timeout of two
hundred milliseconds, ending when the handle reports done, when the cancel token
is pulled, or when the guard says nothing has moved for long enough. The result
comes from `CURLMSG_DONE` rather than from a return value.

Nothing else changes: the signature is the same, no backend is touched, and the
`Response` is still built from `curl_easy_getinfo` on the same handle.

Two decisions become Mole's, on every protocol:

- **A cancellation takes effect at the next poll**, rather than whenever libcurl
  next consults the progress callback and acts on what it says.
- **The guard is `net::StallWatch` consulted in that loop**, against
  `CURLINFO_SIZE_DOWNLOAD_T` and `CURLINFO_SIZE_UPLOAD_T` on the handle, so
  "nothing has arrived for `stallSeconds`" ends the transfer at `stallSeconds`
  whatever the protocol is doing and whatever the socket is doing.

The progress callback stays for what it is good at, which is being called often —
a cancellation noticed there is felt before the next poll comes round. It stops
being the only way out.

## Reason

**This is not a change of engine.** `curl_easy_perform` is itself a wrapper over
the multi interface, and the point is neither speed nor concurrency. It is that a
loop Mole owns does not have to be listened to: it stops waiting, removes the
handle and returns, whatever libcurl's state machine believes.

**Why not keep arranging it from outside.** Because the arrangement was three
clocks over one download, none of them the transfer's, and the one that actually
fired was the kernel's. A guard whose figure is honoured only when a socket option
happens to agree is not a guard; MOLE-108 spent most of its length discovering
which of the three was in charge, which is the cost of not owning the loop.

**Why the counters rather than the callback's last word.** The handle knows what
it has carried. Reading it in the loop makes the guard independent of how often
libcurl chooses to call anybody, and it covers uploads for nothing — an upload
stalls the same way a download does, and this loop does not need to know which it
is driving.

## Consequences

- **`CURLOPT_LOW_SPEED_LIMIT`/`TIME` are gone.** They were libcurl's own attempt
  at this guard, and a second guard underneath the real one could only disagree
  with it.
- **`TCP_USER_TIMEOUT` goes back to being a backstop.** Twenty seconds, and
  nothing decides on it any more: it stops a socket the kernel is retransmitting
  into from being held for the fifteen minutes `tcp_retries2` allows, which
  matters for the file descriptor rather than for the answer anybody gets.
- **A handle whose transfer was abandoned is closed rather than pooled.** Its
  connection is mid-message and the next caller would inherit it; `Lease::abandon()`
  reaches the same outcome `useOwnConnection()` already had.
- **A multi handle per transfer**, created and destroyed around each one. Measured
  against the four live backend suites and the interference tier with no change in
  any figure worth reporting. **That measurement was wrong — see the amendment
  below.**
- **The guard is now testable without a server.** `ScriptedHttpServer` grew a
  reply that sends part of a body and then holds the connection open sending
  nothing — the neighbour of `hangUpAfter`, and the harder case, because a
  connection that hangs up is a failure the client is told about and one that goes
  quiet is not. Against a two-second guard the transfer ends in about two seconds
  and reports a timeout; the same scenario with the token pulled returns in well
  under a second.
- **StreamingDownload's budget still answers a different question** — how long the
  whole transfer may go without progress, across however many connections. This
  guard is how patient one connection is. ADR-0013's second amendment is where
  conflating the two is written up.

## Amendment, 2026-09-04 (MOLE-369)

**"No change in any figure worth reporting" was measured on fixtures too small to
show the change.** In libcurl the connection cache belongs to the *multi* handle:
`curl_multi_add_handle()` points the easy handle's cache at the multi's, and
`curl_multi_cleanup()` closes every connection in it. So a multi per transfer
means a **connection** per transfer. The connection each transfer used was closed
before its lease was even returned to the pool, and the idle `CURL*` handles the
pool kept carried nothing at all.

The claim the code made all along was the opposite. `CurlTransport.h` promised "a
warm connection to come back to"; ARCHITECTURE.md said the pool "keeps libcurl's
connection cache, which is what stops an SFTP drive renegotiating SSH for every
listing". Neither was true from the day this loop landed.

What it cost, on the two protocols where a connection is expensive: every SFTP
operation renegotiated SSH, at the 0.58 s per handshake ADR-0013 measured — and
one operation is rarely one transfer, because a `stat()` is a listing of the
parent and an `openWrite()` is stat, spans, stat, rename. Four or five handshakes
for one small upload, and a folder of ten thousand files is hours of them. Every
WebDAV request paid the `CURLAUTH_ANY` 401 round trip again, auth state being per
connection and the handle being `curl_easy_reset` on every `take()`. The live
suites did not show it because their fixtures are kilobytes: a handshake is
invisible beside nothing.

**The decision stands; the cache moves.** Owning the loop is what makes stopping
Mole's decision, and that is unchanged. A `CURLSH` share handle per `CurlPool`,
with `CURL_LOCK_DATA_CONNECT`, holds the connection cache outside any multi —
plus the DNS and TLS session caches, since a drive is one host and neither
carries a credential. Its lock callbacks are required: a share is used from every
thread holding a lease and libcurl does no locking of its own.

`Response::connectionsOpened` carries `CURLINFO_NUM_CONNECTS` so this is
assertable rather than something to read out of `MOLE_LOG=net`.
`tst_CurlTransport::aSecondTransferOnOnePoolReusesTheConnection` holds it
offline — and needed `ScriptedHttpServer` to learn keep-alive, because a server
that closes every connection cannot tell a client that reuses them from one that
does not. `tst_SftpFileSystem::aSecondListingCostsNoHandshake` holds it against a
real server, by timing, because the handshake is not visible from above and its
cost is the whole point.

**And three comments this loop left behind have been corrected**, since a comment
describing the design before a change is worse than none: `StallWatch`'s header
said the stall was decided "in the progress callback" when the loop had taken it
over, `ProgressWatch` carried a switched-off `StallWatch` and a `stalled` flag
nothing read, and `BufferedUpload`'s reason cited WebDAV being "unreliable about
chunked PUT" when WebDAV has streamed through a chunked PUT since MOLE-34.
