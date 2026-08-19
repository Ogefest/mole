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
  any figure worth reporting.
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
