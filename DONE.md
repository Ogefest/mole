# Done

Finished work, newest first, with a line on how each one was resolved. Anything
still outstanding lives in [TODO.md](TODO.md).

This is not a changelog for users — it is the record of what was asked for and
what the answer turned out to be, including the ones where the first answer was
wrong.

---

## An NFS export could only be reached by mounting it outside Mole

**Asked for:** MOLE-213 — the other half of what MOLE-36 was split from, and the last task in
EPIC-20. NFS is what a Linux or BSD file server offers first and what a NAS offers beside SMB, and
Mole had no way to reach one: the route was mounting it in the operating system and browsing it as a
local path, which is what a virtual drive exists not to need.

**libnfs, a userspace client, optional at configure time exactly as `libsmbclient` is.** A build
without it ships the other drives and names the one it is missing. That part went as the ticket
described.

**The ticket asked for a context per thread, and that would have been wrong.** It named the shape
SMB had settled on — a context built on first use, kept for the life of the thread, the way
`SqliteTable` keeps its connections — and it is right for the metadata calls and wrong for the ones
that matter. **A handle belongs to the context it was opened on.** `openRead` hands back a
`QIODevice`, and the thread that reads a file is not the thread that opened it: a transfer opens on
one pool thread and streams on another. A per-thread context means the read is issued on a context
that has never heard of the handle.

So a context is **leased** instead. An operation borrows one for its own duration; an open file
borrows one for as long as it is open, which is what keeps its handle and its context together on
whatever thread does the reading. Connections are pooled per server-and-export and four are kept
idle, because mounting is two round trips and a directory walk should not pay for one per
directory. One session for the process — SMB's answer — was rejected for a stated reason rather
than by taste: SMB has no choice, since its wrappers act on a global context, and libnfs does,
since a context is self-contained. Copying the scar without the reason would have serialised every
NFS operation behind one connection.
[ADR-0050](docs/adr/0050-nfs-through-libnfs-and-a-leased-mount.md) records all of it.

**A broken connection is closed rather than returned to the pool.** libnfs keeps one TCP connection
per context, and once it is gone every later call on that context answers with the same failure —
so a failure is classified. The server answering a question about a file (`ENOENT`, `EEXIST`,
`EACCES`, …) hands the connection back; anything else abandons it. Without that a single timeout
leaves a drive broken until Mole restarts.

**The conformance suite found one fault, on the first run, and it was the interesting one.** NFS has
no open: libnfs looks a name up and hands back a handle, and a directory answers that lookup as
readily as a file does. Reading a directory therefore *succeeded*, and then failed on the first
read — which arrives as a failed copy rather than as a refused one. The fix is that the
`nfs_fstat64` which used to be skipped when the caller already knew the size is now always made,
and it is what refuses. Everything else passed first time, which is what a conformance suite is
for: the rules it enforces were paid for by other backends.

**Two library shapes are worth knowing before writing against libnfs.** `nfs_read` and `nfs_write`
take the count *before* the buffer, which compiles either way round because the buffer is `void*`.
And a directory entry carries the attributes for free through READDIRPLUS, so unlike a share
(ADR-0048) an ordinary listing needs no stat per entry — the fallback for a server offering only
plain READDIR is written and on the testbed it never runs.

**Nine cases, eight of which need no server.** The form, the pasted `server:/srv/media` line that
is already on somebody's clipboard, the path built inside the export, what does and does not share
a connection, and a drive pointed at a machine that is not answering — which comes back with an
error rather than a hang, because twenty seconds of patience is set explicitly and libnfs left to
itself waits for the kernel. The ninth is the conformance suite against the export on the testbed.

**`make test-live` is green whole again, six suites and no skips**, and `make test` is 91.

**One thing found on the way and left documented rather than fixed:** under ThreadSanitizer,
against the live export, the two threads of the conformance suite's concurrency case race inside
glibc's `tzset_internal` — the timezone cache, reached through `QDateTime::fromSecsSinceEpoch`
while stamping a listing's modified times. Nothing in the backend appears in either report, every
backend that stamps a time from two threads can reach it, and in the application the interface has
touched the timezone long before a worker does. It is in TODO.md.

---

## Mole could not open a Windows or NAS share, and the backend for it aborted

**Asked for:** MOLE-36 — SMB, split from NFS on 2026-08-19 once one backend had shown that two with
no design between them was a project rather than a ticket. The code was in the tree and offered
nowhere: the conformance suite ended in `talloc: access after free` inside libsmbclient's
allocator, about eighty milliseconds in and nowhere near the call that caused it.

**The cause was that a context's function pointers are not the entry points.** libsmbclient's plain
`smbc_*` wrappers do bookkeeping around every call that Samba's internals depend on — a talloc
stackframe — and calling `smbc_getFunctionStat(ctx)(…)` directly skips it. Samba then runs with no
stackframe, says so once and quietly, leaks into its arena, and aborts later somewhere unrelated.
The library's own message was the whole clue and it was two lines above the abort:
`no talloc stackframe at source3/lib/interface.c`.

The wrappers act on one global context, so using them means having one — and that is the design
now: **one session for the process, every operation serialised behind it.** A listing on one SMB
drive waits for a read on another, which is a real cost and
[ADR-0048](docs/adr/0048-windows-shares-through-libsmbclient.md) states it rather than hiding it.
The alternative on offer was a backend that could take the process down.

**AddressSanitizer had nothing to say**, which was itself informative: talloc manages its own
memory out of large allocations, so a sanitizer watching malloc sees a healthy heap while Samba's
arena is being corrupted. The library's own diagnostics were the only instrument that worked.

**Then the conformance suite earned its keep three more times.** An abandoned write was being left
under the name somebody asked for, so writes go under a working name and are renamed into place,
the same rule as the local disk. A rename onto an existing name was silently replacing it, where
every other backend refuses — Samba will overwrite if it is let, so the refusal is ours to make,
and a rename that quietly overwrites is how a bulk rename destroys a file nobody mentioned.

**And twice the suite itself was wrong about what it meant.** It held a read handle open across a
remove and across an overwrite; on SMB a share refuses both, which is Windows semantics rather
than a fault, and the steps in question are about removing and overwriting rather than about doing
either while somebody has the file open. Every POSIX backend had passed only because POSIX allows
it. The suite lets go of the handle now and says why.

**`make test-live` is green whole for the first time**: five suites, no skips. The SMB suite is
named in `SUITES`, and two gaps in that script turned up on the way — it never set the control
channel, so a case that cuts a connection mid-read had been skipping silently since it was
written, and QtTest's five-minute per-function watchdog is not enough for it now that the read
waits for the transfer's budget rather than for a socket to fail.

## Mole did not own its transfer loop, so the stall guard was advice

**Asked for:** MOLE-212 — written up while MOLE-108 was being measured, and by the end of that
ticket the reason was not in doubt. `CurlPool::perform` called `curl_easy_perform`, which blocks
until libcurl decides the transfer is over, so every mechanism Mole had for bounding a transfer was
an attempt to influence a loop it did not own. On SFTP all three missed: libcurl's own low-speed
guard never reached a verdict on that protocol, `StallWatch` returned *stop* and was ignored, and
what actually ended transfers was `TCP_USER_TIMEOUT` — **the kernel deciding when a Mole transfer
gives up, and by how much.**

**The important half of the MOLE-108 measurement was that the callback fired.** Twice a second,
throughout a dead link, with the byte count frozen. libcurl was returning to its own loop and
consulting Mole all along; being told to stop simply did not end the wait. That is what made this
a loop-ownership problem rather than a tuning problem.

**So `perform` drives the transfer.** `curl_multi_add_handle`, then `curl_multi_perform` and
`curl_multi_poll` on a two-hundred-millisecond tick, ending when the handle reports done, when the
cancel token is pulled, or when the guard says nothing has moved. The result comes from
`CURLMSG_DONE`. Not a change of engine — `curl_easy_perform` is itself a wrapper over the same
machinery — and no backend changed. See
[ADR-0049](docs/adr/0049-the-transport-owns-its-transfer-loop.md).

**The guard reads the handle's own counters** rather than whatever the callback was last told,
which makes it independent of how often libcurl chooses to call anybody and covers uploads for
nothing.

**Then, and only then, the old mechanisms came out.** The ticket said to remove them after the live
suites ran green with the new loop, so that one change is measured at a time — and that order
mattered: the four backend suites were run against the testbed before the removal and again after.
`CURLOPT_LOW_SPEED_LIMIT`/`TIME` are gone, and `TCP_USER_TIMEOUT` is a backstop against a held file
descriptor rather than the thing that decides.

**The guard is testable without a server now.** `ScriptedHttpServer` grew the neighbour of
`hangUpAfter`: a reply that sends part of a body and then holds the connection open sending nothing.
That is the harder case, because a connection that hangs up is a failure the client is told about
and one that goes quiet is not. Against a two-second guard the transfer ends in about two seconds
and reports a timeout rather than a cancellation; with the token pulled it returns in well under a
second.

**One detail that would have been expensive to discover:** a handle whose transfer was abandoned
must not go back to the pool, because its connection is mid-message and the next caller would read
somebody else's reply. `Lease::abandon()` reaches the outcome `useOwnConnection()` already had.

## A transfer cut off by a long outage hung instead of giving up

**Asked for:** MOLE-108 — a transfer interrupted by a total outage neither finished nor failed. It
sat there having moved 34 MB and was still sitting there 641 seconds later.

**The standing guess was wrong, and measuring it was the whole first half.** The ticket supposed
the SSH layer blocks inside a socket operation so the progress callback never runs. It runs —
twice a second, throughout, with the byte count frozen. What does not happen is libcurl's own
`CURLOPT_LOW_SPEED_LIMIT`/`TIME` reaching a verdict on SFTP at all.

Two mechanisms came out of that. `net::StallWatch` applies the guard in the progress callback,
counting movement rather than speed. And that alone does not end anything: with the callback
returning stop on time, `curl_easy_perform` still does not return, because the thread sits in the
SSH layer on a socket the kernel is faithfully retransmitting into. Keepalive does not cover it —
keepalive runs only on an idle connection, and a transfer cut off in flight has unacknowledged
data, so the bound is `tcp_retries2`, about fifteen minutes. **That is the 641 seconds.**

**Then the fix broke the opposite case**, and the planning answer was that the choice was a false
one: one number was doing two jobs. `stallSeconds` was both how patient a single connection is and
how long the whole transfer may go without progress, so a connection giving up meant the transfer
giving up.

**Separated, both behaviours are available.** `StreamingDownload` gets one clock — how long since
a byte arrived, reset by every byte — and while it is unspent a failed span is fetched again from
the offset it reached, after a pause capped at two seconds and interruptible. `TCP_USER_TIMEOUT`
becomes twenty seconds, chosen for the job it does rather than inherited from a guard answering a
different question: with retries in place, a connection that admits defeat quickly is a virtue.
[ADR-0013 carries the second amendment](docs/adr/0013-a-large-sftp-read-arrives-in-spans.md).

**The bound stopped being a count and became a budget.** The first amendment's rule — one stall
and one attempt that gets nowhere — covered a re-keying server and could not cover a link that
comes back: the single retry met the dead link, delivered nothing, and failed a read the link was
about to be able to finish. The case that used to survive a two-minute outage only ever survived it
because one TCP connection happened to outlive it, which is a one-second margin decided by a kernel
timer nobody set on purpose.

**Two tests hold it with no server**, in the shape the brief asked for: a span that fails and then
delivers is resumed until the file is whole, and a span that never delivers ends the read when the
budget is spent and not before. `aSpanThatDeliversNothingIsTheEndOfIt` was the case that changed
meaning, so it changed name.

**Both figures had to move.** A budget checked between attempts cannot bound a fetch that never
returns, and on a download almost nothing is sent — so `TCP_USER_TIMEOUT`, which bounds
unacknowledged *sent* data, may never start. With the connection's own patience left at two
minutes it swallowed the whole outage, the budget was never consulted, and a 200-second outage
still did not end the transfer. SFTP's connection patience is now twenty-five seconds, which is
safe precisely there: it is the backend whose reads have a retry loop over them.

**The instrument was measuring something else, three times over.** `blackhole` deleted the
`netem rate` both outage cases apply seconds earlier, so from the moment the outage began the link
ran at full speed and the arithmetic in those tests was fiction; a rate in force is now put back
underneath and restored after. Then a blackhole asked to clear after sixty seconds was measured
**still standing after ninety** — a detached `sleep` does not survive the ssh session that starts
it, so it is a transient systemd timer now. Then that timer fired up to a minute late, because a
transient timer defaults to `AccuracySec=1min`. And the function that scheduled it was never
defined in the copy that runs on the machine, which the script reported only as
`command not found` into `/dev/null`; a failure to schedule an undo now says so out loud, because
an unscheduled undo is a machine left damaged.

**All three live cases pass.** A minute of outage is ridden out, so is one second inside the
budget, and 200 seconds ends the transfer with a failure naming the file — 174 seconds after the
link went away, which is why the bound came down from seven minutes to four rather than to the
three that was asked for: two minutes of budget plus one attempt's worth of noticing is what the
number is made of.

## Orphaned S3 multipart uploads could not be found, and were charged for

**Asked for:** MOLE-96 — a multipart upload interrupted by the process being killed leaves its
parts on the server. `abandonMultipart()` handles every failure the process is alive to see, and
`SIGKILL` gives it nothing to see. S3 charges storage for those parts until the upload is
completed or aborted, they are not objects, and nothing that lists a bucket will ever mention
them.

**The ticket was held back by a design question**, which is why its epic parked it: not what S3
has to call, but *where a drive-level maintenance action lives*.
[ADR-0047](docs/adr/0047-a-drive-reports-what-it-is-still-holding.md) answers it — **the drive
reports what it is holding, in its own words, and the shell offers to clear it.** Two virtuals on
`IFileSystem` behind a `ReportsLeftovers` capability, the same shape that put `space()` and
`access()` on the drive rather than teaching the interface about buckets. Nothing in the shell
knows what a multipart upload is.

**Finding and removing are two steps**, because what a sweep finds is somebody's: an upload that
looks abandoned may be a copy running on another machine this minute, and nothing in the protocol
tells them apart. The age threshold is the same rule in numbers, and it is a parameter rather than
a constant — the right figure depends on how long a copy takes on the link in question.

**Two things the server taught us.** MinIO answers an empty list for a `prefix` that certainly
matches: seeded by hand, the unfiltered listing reports the upload and the filtered one does not.
So the prefix is applied in the backend instead — a filter that silently hides leftovers is the
same fault as not looking for them. And the paging carries two markers rather than one, because
two uploads of the same key can be in flight.

**Four claims, and the live one is the whole ticket.** The parser is held without a server,
including that an error document is never read as "nothing left behind" — that answer is precisely
the one that keeps somebody paying. Against real MinIO: an upload seeded raw is found, hidden by
an age threshold, removed, and gone. And with a real process: a second copy of the test binary
begins an upload, is `SIGKILL`ed, and what it left is found and removed — the scenario the earlier
kill work could not tick. It lives in the S3 suite rather than in `tst_KilledOutright` because
that suite is a core one and this needs a backend from a plugin that is not built everywhere.

## A large SFTP read died at the re-key point on a server that re-keys early

**Asked for:** MOLE-99 — found by the scale tier, which is where it was meant to be found. ADR-0013
fetches a large SFTP read in 256 MiB spans so that no connection carries enough for the server's
re-key to arrive. A server whose `RekeyLimit` is exactly `256M` puts the re-key *inside* the first
span, and then the file cannot be read at all: every attempt stops 640 KiB short of 256 MiB.

**No span size is safe for every server**, which is what makes this a design question rather than
a constant to tune. The client cannot know what the server's limit is, and a smaller span only
moves the wall while costing a handshake more often for every server that would not have re-keyed.

**So a span that carried bytes and then stopped is resumed from where it got to.** That reverses
the part of ADR-0013 that rejected resuming, and the reversal is the whole decision: the original
reasoning was right about the cost — a stall-guard wait per re-key point — and wrong about the
alternative, which is not "spans never meet the fault" but "the file cannot be read".

**Only a span that carried bytes.** One that carried none has nothing to resume from and no reason
to expect better, so the read fails there. Without that rule a dead link would cost a stall-guard
wait per attempt for ever, because a server that re-keys and a link that has gone away look the
same from here: one stops after delivering, the other delivers nothing. With it, the total is
bounded by the file — every resume must make progress, so there cannot be more of them than there
are bytes.

**Measured.** A gibibyte from the server whose limit is `256M`, previously unreadable, arrives
whole and verified byte for byte, at 1.8 MiB/s against 14.9 from the server that does not re-key.
That is four stall-guard waits and it is the price the amendment names out loud.

**The control test asserted the old contract and now asserts the new one**, with the bound beside
it: a span that delivers nothing ends the read after exactly two attempts, so a dead link cannot
become a wait without end.

**Two things that were not the code.** The tier reported the resumed download using 199 MiB of
temporary space, which it calls staging — it was this session's own experiments writing into
`/tmp`, which is the directory the tier watches. `ResourceWatch` now names the largest entry it
counted, so the next person reads a filename instead of reproducing the run with a shell watching
`/tmp`. And QtTest's own five-minute per-function watchdog fires before a legitimately ten-minute
transfer ends, which looks exactly like the hang this tier exists to tell apart from slowness; the
suite now sets it.

## Sync took a dropped connection for the end of a file

**Asked for:** MOLE-98 — noticed while writing the hostile slice through `TransferTask`.
`SyncTask::copyOne()` had the fault `TransferTask` had been fixed for, and sync is the worse
place for it.

**What it turned out to be:** three lines, and the same root as before. `QIODevice` answers "the
file ended" and "the read failed" with the same empty result, and the loop read with the
`QByteArray` overload and stopped on an empty one. So a source whose connection died half way
through was a file that finished: the destination was closed — which commits it — and the file
counted as copied.

**Why it is worse in sync than in a copy.** A failed copy is a missing file, and somebody notices.
A failed *sync* leaves a destination whose size matches the source's, so **the next run plans
nothing**. The loss is permanent and it is silent, which is what the `data-integrity` label is
for. The last test in the slice is the one that says this out loud: the first run fails, the
second run — with the drive behaving — copies the file, because it was never counted as done.

**Two more things the same function was missing**, both of which `TransferTask` already had: it
did not ask the source again when fewer bytes arrived than the plan said, so a read that ended
early was indistinguishable from a file that shrank (ADR-0027); and its short-write failure said
`short write` and nothing about why. A destination that filled up, one whose connection went away
and one whose file was pulled out from under it were all the same sentence, and which of them it
was is the only part anybody can act on.

**Five scenarios, and each guard was removed to check its own test failed.** Taking out the
`read < 0` check fails the dropped-connection case and the next-run case and nothing else; taking
out the ADR-0027 stat fails the overstated-size case; putting `short write` back fails the
disk-full case. The fifth is the one that must keep passing: a file that really did shrink is
copied as it now is, which is why the guard asks the source rather than trusting the plan.

## FTP staged a whole download, so it could not read a file bigger than the disk

**Asked for:** MOLE-127 — the mirror of MOLE-34, which fixed the write side and left this one
alone deliberately. `FtpFileSystem::openRead()` downloaded the whole file into a temporary and
handed that back.

**The ticket was a question, not a design.** `StreamingDownload` already existed and SFTP already
used it; what nobody knew was what libcurl does with `CURLOPT_RANGE` on an FTP transfer. For FTP
a range becomes `REST` plus `RETR`, and `REST` has no end — so if the end of the range is ignored,
one span keeps delivering until the file runs out and the next span re-fetches bytes already
handed over. A read that silently duplicates a span is worse than one that needs scratch space,
and the amendment to [ADR-0014](docs/adr/0014-remote-files-stream-rather-than-stage.md) said so
and left it, because there was no FTP server to settle it against. There is one now.

**Measured first, and the answer is that both ends are honoured.** A span of a hundred bytes
delivers a hundred bytes from the offset asked for; a span ending at the end of the file delivers
the tail and stops. A server honouring only `REST` would have answered the first of those with
the remaining 299 900 bytes.

**So `openRead()` took the shape the ticket named** — the SFTP one: fetched whole below 64 MB
over a pooled connection, streamed above it, one `fetchSpan()` helper with `applySettings()` on
every lease, because a span that skipped them would talk to the server differently from the one
before it.

**Three tests, and each holds a different claim.** The ranged fetch goes through plain libcurl
rather than through the backend, because it is a claim about what *servers* do and the backend is
what depends on it — a server that stopped honouring the end of a range would break streamed reads
and that is the line that would say so. A file just over the threshold is read back and compared
byte for byte, because a stream that dropped or repeated a span would still be the right length
and the wrong file. And the structural claim is held without a server, the way the write side's
is: handed a size, `openRead()` returns a stream and touches no network at all.

The first version of the ranged test asked the backend for a 300 kB file and asserted it came
back as a stream. It does not, and should not — that is below the threshold. The claim belonged
one level down.

**No backend stages a whole file in either direction any more**, which is what ADR-0014 set out
to do.

## Finding duplicates ran on one thread and was capped by its own hash

**Asked for:** a scan by content was slow, and the question was whether a faster hash — xxHash
instead of SHA-256 — would fix it.

**What it turned out to be:** two ceilings, one of them not where anybody was looking.
`FindDuplicatesTask::run()` was a single pool thread with no parallelism inside it at all, on a
twenty-core machine. And Qt 6.4 carries its own SHA-2 that does not use the processor's SHA-NI
instructions, so hashing ran at 218 MB/s — against 2 237 MB/s for OpenSSL on the same processor,
24 096 MB/s for XXH3-128 and 86 957 MB/s for `memcmp`. The scan was capped at 218 MB/s whatever
the storage was, and every SSD is faster than that.

**The answer to the question as asked is: yes for one stage and no for the other**, because the
two stages answer different questions. The head is a filter — a collision costs one extra file
read at the stage after it, which then separates them, so no false group can survive and any
hash will do. The last stage is a verdict, and what happens to a group is that all but one of it
is deleted. A fast hash is fast because it is not cryptographic: XXH3 collisions can be
constructed cheaply by whoever wrote the files, and a file manager scans folders filled from
downloads and shared drives.

**So the last stage uses no hash at all.** The files left are compared with one another, byte for
byte, and that turned out to be the faster option as well as the exact one: each file is still
read once, the work per byte is a `memcmp` instead of a digest, and files that differ stop at the
first chunk that differs where a hash always reads both to the end. See
[ADR-0046](docs/adr/0046-a-duplicate-is-proved-by-comparison-and-the-reads-are-overlapped.md).

**Bounded memory was the part that needed designing, not the comparison.** Files are streamed in
lockstep and only the chunk is held, so a group of hundred-gigabyte disk images costs what a group
of documents costs. A bucket bigger than sixteen files is compared in slices and the slices joined
by comparing one file from each — otherwise ten thousand copies of one photograph would be ten
thousand descriptors and two and a half gigabytes of chunks. **The slices keep their lone files**:
the first version discarded them, and a mutation test put back exactly that and watched
`aFileAloneInItsSliceIsNotLost` fail, which is what it is there for. Both bounds are asserted at
the drive rather than inferred, because neither shows in the groups a scan produces — a comparison
that slurped both files whole would give the same answer.

**Only the reads are overlapped.** Results are taken back in the order the work went out, so the
grouping, the ordering, the announcement of each group and every call to `Task`'s reporting
helpers stay on one thread: eight threads produce the same groups in the same order as one, which
is asserted, and there is not a lock in the task.

**Measured rather than asserted:** 1.9 GB of duplicates, warm in the page cache, on a release
build — 9 330 ms before, 520 ms after.

## The grid had no page: it put every row of the table behind one scrollbar

**Asked for:** MOLE-187 — the database viewer, and the delimited-text and Parquet viewers with
it, offered every row of a table at once.

**What it turned out to be:** the fetch was already windowed and the offset was not.
`kPageRows` was 500 and `kMaxCachedPages` 24, so the model never held more than twelve thousand
rows however far anybody scrolled — but `rowCount()` returned the whole matching count, so the
offset it fetched *at* was bounded by nothing. `SELECT … LIMIT 500 OFFSET 9000000` is answered by
stepping over nine million rows with the interface thread waiting, and one drag of a scrollbar
issues a run of them, each costing more than the last.

**A page of five thousand rows, in `TableModel`**, so all three viewers get it at once — the grid
is shared and so is the model behind it, which is what `ITableSource` was made source-agnostic
for. `rowCount()` is the rows of the page, row indices are page-relative, and the fetch window
lives inside the page, so the largest offset any query can carry is the page's start plus five
thousand. [ADR-0045](docs/adr/0045-a-grid-shows-a-page-of-a-table-and-the-page-is-a-fixed-five-thousand-rows.md)
records why it is a constant rather than a preference, and why the alternative — keeping one
scrollbar and hoping — promises a seek no source can perform.

**The word *page* meant two things one screen apart**, so the older one was renamed: a *chunk* is
the five-hundred-row fetch inside the model, a *page* is what the reader is looking at. The class
comment said "there is no cap, and no point at which the table quietly stops being the file",
which had become the opposite of true and is the comment the next reader would have trusted; the
user guide said the same thing in its own words and has been corrected with it.

**A selection is cleared by any reset of the model**, not only by a page move. Row indices are
page-relative, so a new source, a different table, a filter and a page change all make a held
block name rows nobody selected — one rule covers all four, where clearing only on a page move
would have left the other three.

**The footer is the only place a row's number within the whole table appears** — *rows 5,001–10,000
of 1,284,003* — and it reads without the total, because MOLE-186 makes that arrive after the first
frame. It is hidden when there is one page, which is why the assertion that it draws at all lives
in the walkthrough's twelve-thousand-row import: the rest of the suite's fixtures fit on one page,
and a fault in a strip nothing renders would have waited for somebody's real export.

## Opening a database counted every row in every table before it drew anything

**Asked for:** MOLE-186 — opening a SQLite file in the preview stopped the window answering for as
long as it took to count every table and view in it, and typing in the filter box repeated a full
scan per keystroke.

**What it turned out to be:** three counts of the same table and one per name, all on the thread
that draws. `SqlitePreviewController::tables()` called `rowCountOf()` once per name and the
binding read the list twice, so a file of *n* tables was counted 2*n* times before the first
frame; `TableModel::setSource()` then read `totalRows()` for a third count of the current one.
`ITableSource` states the requirement in its own header — an implementation that talks to a file
"must be usable from the interface thread, which in practice means answering a windowed query
quickly rather than scanning" — and `rows()` honoured it while the counts never did.

**None of the three makes a count cheaper.** `COUNT(*)` is a walk of the table however the file is
indexed, and that is not going to change. The point is that the window must not wait for it.

- **The count is remembered**, per table, for the life of the open file. The connection is
  read-only, so the file cannot change underneath it and the second and third answers are free.
  That alone halves the cost of opening, because it is the second binding read that doubles it.
- **The walking moved off the interface thread.** `CountTableRowsTask` opens a second read-only
  connection on a pool thread — `connectionNameFor()` already hashes the calling thread into the
  name, which is the shape the delimited importer uses, so there was no new design to make — and
  reports each count back as it lands. The table being looked at is counted first. `totalRows()`
  answers -1 until somebody has taken the count, and that travels: a blank in the picker and a
  summary strip without a row count. **Not `max(rowid)`**, which is wrong for a table that has had
  rows deleted and meaningless for a view; a blank is honest and a guess is not.
- **The filter waits for the typing to stop**, 250 ms, in `TableModel` rather than in the SQLite
  viewer — the delimited and Parquet viewers share the model, and the scan costs the same in all
  three. `CAST(<column> AS TEXT) LIKE '%…%'` over every column is a full scan no index can answer,
  and it used to be one per character.

**The -1 is a contract change, written into `ITableSource`.** A source that knows its size without
asking — an import into a store, a Parquet footer — simply always answers; one whose count is a
walk answers -1 until it has been taken somewhere the window is not. `TableModel` reports no rows
for a count it has not got, so the grid fills in when the figure arrives. MOLE-187 is what makes
the grid draw its first page without waiting for the total at all.

## Analysing a folder posted a status update for every entry it walked

**Asked for:** MOLE-188 — Analyse over a large folder stopped the whole window answering until it
finished, while compressing the same folder stayed responsive throughout. The walk was already on
the right thread; the reporting was not.

**What it turned out to be:** `Task`, not the analysis. `setStatusText()` dropped an update only
when the text matched the last one posted, and a status line carrying a running total never
matches, so every entry became a queued event — and `TaskListModel` turns each one into a
`dataChanged`, a delegate update and a text relayout. Compression posts the same shape of line and
gets away with it only because it has to read and compress each item first. Four other tasks post
per entry over the same walker, so fixing the analysis would have left the fault standing.

**The first answer was a clock on the worker thread**, exactly as the card described: at most one
post every 100 ms, latest text winning, the rest held back and flushed when the body returned.
It works for a task that keeps reporting and fails for one that stops. `tst_Walkthrough` caught
it: a task publishes a rate, an estimate and a byte count in one breath and then runs on without
saying anything more, and the estimate never reached the strip — held back by a window that only
closes when somebody calls again. A stalled transfer is that task, which makes it the case the
strip matters most in.

**So the coalescing is a box rather than a queue.** The worker writes the latest status line and
the latest reading of each metric into a small guarded box and asks for a drain; the drawing
thread empties it and stamps the clock, and a drain that arrives inside the 100 ms window comes
back when it opens. At most one wake-up is ever outstanding, so what the window has to get
through is bounded by the interval rather than by the speed of the walk, and nothing can be left
in the box: whatever is in it when the drain runs is what goes out. The flag is cleared before
the box is read, never after — the other order is precisely how the last reading of a task that
then goes quiet gets stranded.

**`report()` promised the same thing and did not do it.** Its no-op check ran inside the queued
lambda, after the event had been posted and delivered, so it saved a signal and none of the cost.
It is now taken on the worker thread, against what that thread last handed over, and the box is
keyed so four metrics arriving together cost one `metricsChanged` rather than four.

**`setProgress()` has no clock and must not grow one**, which is now written beside the box: a
percentage has a hundred and one distinct values however many files there are, so its value check
is already the bound. The two that carry running totals are the two that needed one.

## Identical contents compared sixteen kilobytes of head

**Asked for:** MOLE-191 — 16 kB is not enough to separate the files people actually have a lot
of.

**What it turned out to be:** a constant, and an accounting worth writing down. A video
container, a RAW photograph, a PDF and a disk image all carry headers larger than 16 kB, so two
different files of the same size agreed at the middle stage and both went through to the
whole-file hash — the pass that stage exists to keep small. On a folder of video the scan
degenerated towards hashing everything, which is what the strategy was built to avoid.

**It is not free and the comment says so.** The stage reads 64 times what it did, but only for
files that already share an exact size — and a file no larger than the head skips the stage
altogether, which `keyFor` already handled, so with the head at a megabyte most files in most
trees cost one read rather than two.

**The test is what the change buys, not the constant.** Two files of one size agreeing over a
header longer than 16 kB and differing inside the first megabyte are separated at the head
stage, and the stage that reads a file whole never sees them. At 16 kB that last number was two.

## Every dropdown cut its longest name in half

**Asked for:** MOLE-190 — reported against the strategy picker in the duplicates tab, where
*Identical contents* appeared in the list it is chosen from as *Identical c…*.

**What it turned out to be:** all fourteen ComboBoxes in the application, because they share the
cause. `implicitContentWidthPolicy: WidestText` sizes the *closed* control to the widest label in
the *control's* font, and that worked. The list is built by the style — each row an ItemDelegate
in the style's own default font with padding of its own — and handed the control's width. So a
row drew its text a third larger than the control had measured it, in the room the control had
needed for the smaller version. The control was 120 pixels; the row asked for 200.

**One control used everywhere**, rather than fourteen chances to get it wrong. A row is laid out
the way the closed control is, and the list is as wide as its widest row. Neither half is enough
alone — matching the font still left the row wanting 138 pixels in a 120 pixel list.

**The width is measured, not picked.** A hidden Text seeds it so the list does not visibly jump
the first time it opens, and then each row reports what it actually asked for and that wins:
`implicitWidth` is the style's answer and no measurement taken outside a row reproduces it.
TextMetrics came two pixels short, which is exactly enough to elide the longest name and nothing
else — the first fix looked right and still cut one name.

## Choosing what to keep had the least weight on the screen

**Asked for:** MOLE-72 — the last card of *Duplicates, rebuilt*, and the one held back until
progressive results landed because seeing groups arrive might change what it should be. It
named three questions rather than a design, and asked for them to be answered out loud.

**What it turned out to be:** [ADR-0044](docs/adr/0044-a-rule-says-what-it-did-and-keep-and-remove-are-both-said.md),
and the answers are these.

*What a rule applied to fifty groups looks like while you check it* — a sentence and a mark on
every row. The panel says which rule is in force and gives the count something to be a fraction
of, and each copy in a decided group reads *keeping* or *remove*, so fifty groups are checkable
by scrolling instead of by counting checkboxes. A group with nothing ticked says *not decided*,
because before a choice is made every copy really is equally kept and marking them all would be
noise. And the rule stops claiming to be the rule the moment a tick is edited by hand.

*Whether a rule belongs per group* — yes as an outcome, no as a second set of controls. `Keep
this one` on any row keeps that copy and ticks the rest of its group, leaving the other
forty-nine as the rule left them. Four rule buttons per group would be two hundred controls
saying what one click already says.

*Whether keep or remove is the honest verb* — both, in different places, because they are
different facts. The rule is stated as keep, because that is how the decision is made. The tick
is stated as remove, because that is what it does. Saying only one of them was the actual
fault: pressing **Newest** under a heading reading **Keep** put a tick against every file except
the newest, and nothing said which reading a tick carried. The fourth button was *Nothing*,
which under *Keep* meant the opposite of what it did; it is *Everything* now.

**Two of my own assertions were wrong before this landed.** The height check written for MOLE-69
compared the tab body against a hand-picked tolerance, which MOLE-72's taller panel broke — a
tolerance nobody can derive is a claim nobody is checking, and it is arithmetic now: the body
gives up exactly the panel and the column's spacing. And it read the heights once, before the
column had re-laid out, so it now waits for the condition.

## A duplicates result could not become anything except a deletion

**Asked for:** MOLE-71 — the view found copies and offered one thing to do with them. Finding
duplicates is *locating*, and what to do with what was found is a separate question whose answer
is not always "delete".

**What it turned out to be:** cheaper than it looked, because the mechanism was already there.
Operations take a list of uris and the shell asks the current tab for it by name — `targetUris()`
— so a result that can become a file set inherits copy, move, compress, rename and analyse
without the view growing a verb for any of them.

**Half of it was already true and nothing held it.** `DuplicatesController::targetUris()` existed
and worked; there was no test saying so, which is the same as it being able to stop working
silently. It is now asserted against the list a set is built from, so the two cannot drift.

## Duplicate groups appeared only once the whole scan had finished

**Asked for:** MOLE-70 — a scan showed nothing until it ended and then showed everything, and the
information to do better had already been paid for.

**What it turned out to be:** a question of which stages run over everything and which run per
bucket. The cheap stages stay breadth-first, because until one has run there are no buckets and
because running them over the lot is what tells the last stage how many files it has to read.
The last stage runs bucket by bucket, and a bucket surviving it is a group nothing later can
change — so it goes out then. The expensive stage still only sees what the cheap ones left,
which is the whole point of the staged design.

**The ordering question was the one the ticket asked to be answered rather than assumed**, and
[ADR-0043](docs/adr/0043-a-duplicate-group-is-reported-when-it-is-confirmed.md) answers it:
inserted in place, not appended and re-sorted at the end. Appending would leave the list in
arrival order for exactly the window this change exists to create — the whole of a long scan —
and then rearrange it under the eyes of the person reading it.

**A stopped scan keeps what it confirmed and says it was stopped.** *Nothing matched* is a claim
about a tree that only a scan which ran to the end may make, and offering "try a different
strategy" after searching a tenth of a NAS would be answering a question nobody asked.

Both halves are tested from the data rather than from a clock: the order the signals were
announced in for the first, and a stop asked for on the scan's own thread the moment a group is
confirmed for the second. The obvious way to write the second — cancel from the test thread when
the tab shows a group — was tried first and quietly proved nothing: the window wakes on a 15 ms
tick, by which time a scan of twenty small files has long finished.

## A duplicates tab was mostly empty space

**Asked for:** MOLE-69 — most of the tab was void, and the emptiness was worst at the first moment
somebody opened it, which is when they are deciding whether the feature is worth using.

**What it turned out to be:** one binding. The group list was the only item in the column with
`Layout.fillHeight`, and it carried `visible: groupCount > 0` — and a `ColumnLayout` drops an
invisible item from the layout altogether rather than reserving its height, so before a scan
nothing claimed the space and everything collapsed upward.

The body is one item that always claims the height now, with a panel inside it for each of the
four states. Two things also moved to where somebody would read them: the roots being searched
get a row each instead of a `\n`-joined label elided in the middle, which hid which drive each
was on; and what the chosen strategy costs is said in the empty state rather than only as 11px
grey at the bottom of a panel, where somebody about to start a scan on a NAS was least likely to
see it.

**The first version of its test proved nothing.** It wrote files into the suite's own temporary
tree and pointed the window's scan at them — but the application mounts its own fixture, so the
scan answered with nothing while looking exactly like a scan that found nothing. The assertion
that caught it was the one expecting a group.

## A file git called deleted had no row, so nothing marked it

**Asked for:** MOLE-184 — five of git's six letters land on a row somebody can see. `D` does not,
because a listing shows what is on disk.

**What it turned out to be:** the listing goes on showing what is on disk, and the count in the
band becomes something to open.
[ADR-0042](docs/adr/0042-a-deletion-is-reachable-from-the-band.md) records why a synthesised row
lost, and it is not that it would have been hard to draw — `FileListModel::Provenance` and
ADR-0038 already have the machinery. It is that a row is something a reader can cursor onto,
tick, sort, filter and press `F5` or `F8` at, and each of those needs an answer for a file with
no bytes. Six answers, and a listing that can be asked to copy something that does not exist, for
one letter that would anyway be undoing a deletion somebody made on purpose.

**A deleted entry goes to the folder that held it, with the cursor on nothing.** The folder is the
true half of the answer and the cursor is the false half: dropping it on whatever happens to sort
first would point at a different file while looking exactly like success.

**The test is the invariant, not the feature.** The band is the only door to these paths, so the
number it shows and the number of entries the list holds have to be the same number — asserted
against a work tree carrying one of each of the six states, conflict included, built through a
rebase that really stops on one rather than through an index written by hand.

## The guide did not mention git, and read-only was not written down

**Asked for:** MOLE-107 — the guide said nothing about git and the read-only boundary was
nowhere on record, which means the next person to look would read it as work somebody forgot
to do.

**What it turned out to be:** a section in `browsing.md`, one picture, three entries in
[TODO.md](TODO.md) and a bullet in the README.

**The picture had to be earned, not staged.** The guide's own rule is that every picture was
taken by the suite immediately after asserting that the state it shows is real, so the
walkthrough gained a step that builds a checkout in its fixture, edits one file, leaves another
untracked, asserts the branch, the count, the commit subject and the letter on each row — and
photographs that. Only the one picture was copied in; `make guide-images` rewrites every one of
them because the sidebar shows the machine's real free space, which is what TODO.md already
says to do about it.

**Three things are now decisions with reasons rather than absences.**

*Mole shows git state and does not change it.* Not pending: a file manager that shows git state
is useful to everybody with a checkout, and one that half-implements a git client is a worse
`git` and a worse Mole at once — it would need conflict resolution, hunk selection and
credential handling before it was worth reaching for, and anybody who wants those has better
already. The boundary is what keeps this small enough to be correct.

*A repository on a remote drive shows nothing*, and the fix is not to stretch this feature
across a network. The honest form is a git backend behind `IFileSystemFactory` — a drive that
speaks git — which composes with everything here and needs none of it changed.

*There is no diff or log viewer*, because both are previews of a file or a commit and would be
an `IPreviewProvider`. The band answers "what state is this folder in"; reading a diff is a
different question with a good answer one keystroke away in the terminal panel.

**Two things were stale and are now not.** The TODO note said `make guide-images` rewrites
"forty-five pictures" when there were forty-nine, so it no longer carries a number that drifts.
And the README's list of what works today had no git in it.

**The guide says what happens without libgit2**, because that is the question somebody
packaging this will have: the window behaves exactly as it did before any of this existed. It
is asserted on every change rather than promised — the whole suite runs in a second
configuration with `MOLE_WITH_GIT2=OFF`.

## The band said which branch, not whether it was behind

**Asked for:** MOLE-106 — which branch was on the band; whether it is behind the one it
tracks was not, and that is the fact that decides whether to pull before starting work. Nor
what the last commit was, which is how somebody recognises where they left off.

**What it turned out to be:** four more fields on `RepositoryHead`, filled by the same
`head()` call that already read the branch. Both facts are reference and object reads on a
repository that is open, so they cost no work tree walk and appear as promptly as the branch —
which is why they belong there rather than with the status query.

**"No upstream" and "level with the upstream" are different answers and both show nothing.**
The counts are nought in each case, so a flag carries the difference: `hasUpstream`. A bare
`0/0` reads as up to date when the truth is that there is nothing to compare against, and
"level" needs no counter because there is no decision to take from it. Three tests, because
those are three states that a single integer pair cannot distinguish.

**The band must not imply a network round trip it did not make.** These counts are against the
remote-tracking reference *as it was last fetched* — nothing in this class talks to a network.
So the words are "2 ahead, 1 behind" rather than anything suggesting a check just happened.

**The subject is the part that gives way.** The commit line is short id, subject and age; the
subject takes the space that is left and elides, because a band that grew a second line would
take that height off the listing for good. Asserted by measuring: the band's height with a
subject far wider than the pane equals its height with the word "first", *and* the label's
implicit width exceeds its actual width — otherwise the test would pass on a window wide enough
to fit the lot and prove nothing.

**A repository with no commits draws no commit line at all**, which falls out of an invalid
date rather than an empty-string check. `git init` and nothing else is a branch that exists as
a name with nothing to point at: the band still says `main`, because that is true, and says
nothing about a commit, because there is not one.

**Two things the fixtures needed.** An upstream without a network or a second repository:
git configures a local branch as an upstream with `remote = .`, and `git_branch_upstream`
resolves it the same way, so `GitFixture::setUpstream()` is four lines. And the commit date the
fixture stamps is a fixed instant rather than "now", which was already true and undocumented —
the first version of the date test compared against a window around the current time and
failed. It is now `GitFixture::kCommitTime`, so a test asserting on it compares against a named
constant instead of carrying the same magic number a second time.

## Status read once was status that went stale

**Asked for:** MOLE-105 — a listing that says a file is unchanged after Mole itself has
copied over it is worse than one that says nothing. A wrong answer is the failure mode this
feature has; a missing answer is not. Make the status refresh when it stops being true.

**What it turned out to be:** three sources of staleness, one timer, and a watcher — plus one
instruction in the brief that could not be followed as written.

**Writes are already announced, so there is one place to listen.** Every operation Mole
performs posts to the `EventBus` — a copy, a move, a delete, a rename, a new folder — because
a second pane on the same folder has to hear about it. The pane subscribes to the same events
and asks one question of each: is that path inside the work tree in view? Not *is it the folder
on screen* — copying into `src/` while looking at the checkout root changes the count and the
roll-up on the folder row, and a per-operation hook would have been a hook per operation to
forget.

**`VfsCapability::Watch` turned out to be a capability bit with nothing behind it.** The brief
said to use it where available and fall back to re-reading on pane activation. There is no
watch method on `IFileSystem` at all and no backend implements one, so "where available" is
nowhere. What replaced it is a `QFileSystemWatcher` on the repository's own directory and on
its loose branch tips — which is where a commit, a checkout or a pull leaves its mark, and none
of those announces anything. That is honest for this feature specifically because it is local
drives only, which is exactly where a filesystem watcher works. The activation fallback was
then not built: with the watcher in place it would be an unconditional walk per pane switch,
which is the cost the floor exists to avoid.

**The floor is four hundred milliseconds, and it is started rather than restarted.** Copying
two hundred files finishes two hundred tasks; a walk per task would cost more than the copy it
is describing. Four hundred milliseconds is longer than the gap between two files of a bulk
copy, so a burst collapses into one walk, and short enough that a single `F5` looks immediate.
Started-not-restarted matters: restarting on each event would mean a copy running for minutes
showed nothing at all until it finished. The test counts walks for two hundred files rather
than watching a clock.

**The cache is forgotten at once and the walk is scheduled after.** If the answer were dropped
only when the walk ran, a second pane navigating into the checkout in between would find the
stale answer sitting in the cache and believe it.

**A refresh changes the annotations and not the rows**, which is what keeps the cursor and the
ticked rows where the user put them — asserted with a cursor on the third row and two rows
ticked while a walk lands.

**A latent hazard in the test fixture surfaced as a crash**, and it was worth the hour. Panes
were parented to the test object, so every pane from every case survived `cleanup()` while the
`TaskManager` it holds was deleted after each one. That was harmless while a pane was inert;
a pane that watches a directory and holds a timer is not inert, and one woke up to submit a
task to freed memory. Order-dependent, so it passed alone and crashed in the suite — the worst
kind. Panes are now owned per case and destroyed before the services they hold.

**One assertion in the brief could not be met as written, and the truth was more interesting.**
"Deleting a tracked file marks it deleted rather than leaving it marked as it was" — the row
does carry `D`, for as long as the row is there. A pane on its own does not reload itself, so
the assertion holds; once the listing is reloaded the row is gone, because a listing shows what
is on disk. Both halves are asserted, which is the only honest version of MOLE-184.

## Every index search iterated a destroyed list, and nothing said so for six days

**Asked for:** MOLE-185 — found by running `make asan` while finishing MOLE-105, which is
six days later than it should have been run. It had been red since MOLE-102 landed, and the
reason it went unnoticed is simply that the sanitizer build was not run for MOLE-102, -103 or
-104. `make test` was green throughout, which is exactly the trap: undefined behaviour that
reads as working.

**What it turned out to be:** two faults, one of them serious.

**The serious one is a dangling reference in every index search.**

```cpp
for (const SearchPredicate& predicate : planSearch(query, SearchSource::Index).pushedDown())
```

`planSearch()` returns a plan by value, `pushedDown()` returns a reference into it, and a
range-`for` extends the lifetime of the range expression's *result* rather than of the
temporaries that produced it. So the plan died at the end of that line and the loop walked a
`QList` whose storage was gone. It appeared to work because the freed stack still held the old
bytes; what it can do instead is drop a predicate or match on rubbish, and which of those
depends on what the compiler put in that slot next. Named the plan, which is the whole fix.
This predates the git epic entirely.

**The other is libgit2's per-thread error text**, allocated the first time a call on a task
pool thread fails — which happens constantly, because discovery walking up from a folder in no
work tree *is* a failed call. libgit2 frees that state when the thread exits or when
`git_libgit2_shutdown()` runs on the same thread, and a pool thread still alive at process exit
satisfies neither. Nothing of ours to free, and no API to force it: `git_error_clear()` drops
the message and keeps the buffer. Suppressed by module, like Qt's scene graph already is.

**Which made the opt-in suppression list worth deleting.** It was applied by name, so that a
suite driving one of those stacks got the suppressions deliberately rather than by accident.
That reasoning stopped holding the moment git state was read on every navigation: the suites
that drive libgit2 became most of them, and the list turned into a trap that goes red for
whoever adds the next suite, for a reason they did not cause. Every entry in the file is scoped
to a third-party module, so applying it everywhere hides no leak of ours — the opt-in was
protecting nothing. `lsan-qt.supp` is now `lsan.supp`, since it is no longer only about Qt.

**The lesson is the process one, and it is worth writing down**: `make test` green is not the
same as green. Three commits went in over a subsystem that runs library code on pool threads
without the sanitizer build being run once.

## The listing did not say which files had changed

**Asked for:** MOLE-104 — the band said three files changed; the listing did not say which
three, which is the half of this that gets used while actually working. Mark the rows.

**What it turned out to be:** no new mechanism, exactly as the brief said. `setAnnotations()`
already carries a bitmask keyed by uri from outside the model, and git status is a fourth
annotation of the same kind — one `dataChanged` over the rows, nothing inserted, removed or
reordered, so a refresh cannot move the cursor or lose a tick. The git bits are carried in the
same word as the report and alert bits, shifted rather than re-listed, so "modified" has one
definition in the code base and there is no translation table to keep in step.

**Two things the fixtures taught us, and both changed the code.**

**A rename is reported under the path that no longer exists.** `git_status_foreach_ext` hands
the callback one path per entry, and for a rename that path is the *source* — so the letter
landed on a row nothing draws while the file somebody can see went unmarked. The fix is to
read the entry's deltas instead, where both paths are available, and mark the destination.
Which meant moving from the callback form to `git_status_list_new`, and that costs nothing in
cancellation reach: `foreach_ext` is itself that same call followed by a loop over the
finished list, so a token polled in its callback never interrupted the stat pass either. That
is now written down in [TODO.md](TODO.md) rather than left as a comforting comment — an
abandoned walk stops *carrying* its answer, not working.

**A deleted file has no row to mark.** Five of the six states land on a row somebody can see.
`D` cannot: a listing shows what is on disk, and the file is not. The deletion is still
counted on the band and still rolls up onto the folder that held it, which is where it is
visible — but the file itself is absent, as it is in any other file manager. Marking it means
synthesising a row for something that is not there, and that is a statement about what a
listing *is* rather than a marker: a synthesised row is one a user can tick, sort and press
`F8` at, and every one of those needs an answer. Opened as MOLE-184 with both candidate
answers written down, and the test asserts the truthful behaviour rather than being quietly
dropped.

**The colour is keyed off the letter, not off the bitmask.** A path can be several states at
once, and the letter already resolved that to the one worth showing — conflict, deletion,
rename, addition, untracked, edit, in that order, because structure beats "modified", which is
the thing most likely to be true anyway. Deriving the colour from the same letter is what stops
the two disagreeing. The letter is the signal and the colour only agrees with it, which is
what [ADR-0010](docs/adr/0010-telling-the-two-buttons-apart.md) asks for.

**A directory carries only the roll-up**, a dot rather than a letter, because none of the six
aggregates into anything true — "something below here has changed" is the whole of what can
honestly be said. It is computed in the walk, where the map is already built, and asserted at
every level down to the file, since any of those folders can be the one on screen.

**The markers survive sorting and filtering, and that is the point of the shape.** Annotations
are keyed by uri rather than by row, so a row that moves takes its mark with it. Asserted by
reordering twice and filtering down to one row.

## Nothing walked the work tree, so nothing knew what had changed

**Asked for:** MOLE-103 — the band named the branch, which is the fact that costs nothing.
Everything else worth knowing about a checkout comes from one walk of the work tree, and
nothing walked it. Build the walk and put its summary on the band.

**What it turned out to be:** `Repository::readStatus()` over `git_status_foreach_ext`, a
`ReadStatusTask` to run it on a worker, and a `RepositoryStatusCache` keyed by work tree
root. The cache is the part worth explaining, because the obvious place for it was wrong: a
field on `Repository` would have been read under the lock a walk holds for its whole
duration, so a pane asking what had changed would have waited for the walk — on the UI
thread, which is the one rule this application does not bend. A cache with its own mutex,
never held for longer than a hash lookup, is what makes one walk serve every folder.

**One walk per checkout is kept in three places**, because a stat of the whole work tree per
folder navigated into would make this the most expensive thing in the window. The controller
does not submit when the cache already has the answer; it does not submit a second time when
a walk of the same work tree is already in flight; and the task itself checks again when it
starts, because it can sit in the queue behind a copy long enough for somebody else's walk
to finish. The test counts submissions through `taskAppended` rather than reading the task
list afterwards — a finished task can be retired out of that list, and a test that passes
because the evidence was tidied away is not a test.

**Cancellation is one line in the callback and a decision about the result.** Returning
non-zero from the per-path callback aborts the walk, so the token is polled there. What the
abandoned walk answers with is the interesting half: nothing, not what it had reached. Half a
walk would mark half a listing correctly and the rest as clean, and a listing that calls a
changed file unchanged is worse than one that says nothing at all — that is the failure mode
this whole feature has.

**Navigating out of a checkout abandons its walk; navigating inside one does not.** The
distinction needed a member the task could not supply: which work tree the walk in flight is
of, remembered when it is submitted, because the task cannot answer that until it has run and
the decision has to be made before then. Moving from `src/` to `tests/` is still waiting on
the same walk, and cancelling it there would only mean starting it again.

**Directories are rolled up in the walk rather than by whoever draws the listing.** git
answers with paths to files; a listing shows folders. Without the roll-up, opening a checkout
at its root shows a clean-looking list of directories over a tree full of edits. A folder
carries only *something inside here changed* — which of the six states is inside does not
aggregate into anything true.

**Two things libgit2 gets right and this follows.** Ignored files are excluded, so a build
directory does not bury every real change under thousands of marks. And untracked directories
are not recursed into: git reports a wholly untracked folder as the folder, which is the row a
listing actually has, so a new directory of a hundred files is one change rather than a
hundred.

**The count says "clean" rather than "0 changed".** A count of nought is a sentence about
arithmetic; what somebody wants to know is whether there is anything to deal with. And the
count is absent, not zero, until the walk has answered — a band that said "clean" for the
moment before the walk landed would be telling a lie that reads exactly like the truth.

**That the walk is off the UI thread is asserted rather than assumed.** A direct connection to
the task's own signal runs on the thread that emitted it, so the test records the thread the
walk ran on and compares it with the one drawing the window. No clock, and no large fixture
needed to make the point.

## A folder that was a checkout looked exactly like one that was not

**Asked for:** MOLE-102 — the branch is the single most useful thing to know about a
directory somebody is working in, and Mole was the one window on that directory that did not
show it. A band above the listing, saying which branch and nothing else yet.

**What it turned out to be:** three pieces, and the interesting one is the third. A
`ReadRepositoryTask` on a worker, because discovery walks up the tree and opening a
repository reads its references — on a cold cache, or a directory somebody has just plugged
in, either is long enough to be felt as a window that stopped drawing. A `RepositoryInfo` per
pane holding the answer, passive the way `FileListModel` is, so all of it can be asserted
without a window. And `RepositoryBand.qml`, which is **absent rather than empty** when there
is nothing to say.

**Absent rather than empty is the half that is easy to get wrong**, so it is the half with a
measurement rather than a visibility check. The test reads how tall the listing is outside a
checkout, walks into one and asserts the listing gave height up, then walks back out and
asserts it got every pixel back. An empty strip reserving its height would pass "the band is
hidden" and fail that.

**What the band says is not always the branch.** During a rebase or a merge the branch name is
either the old one or a detached head, and both readings are wrong about what is going on, so
the state wins — in words as well as in amber, because colour is never the only signal
([ADR-0010](docs/adr/0010-telling-the-two-buttons-apart.md)). A detached HEAD says
`detached at a1b2c3d`, because an empty branch name reads as a fault in Mole rather than as a
fact about the checkout.

**Local drives only, and the rule is one line rather than a check.** A uri that is not a real
filesystem path has no local path, so an archive, an SFTP volume and a bucket all leave the
pane with nothing to say — the same shape as the sidebar drawing no capacity bar for a bucket
that cannot report one. The test that holds it puts a real checkout on disk and reaches it
through the memory drive at the same absolute path, then asserts that no read was even
submitted: a band that stayed away because the answer was thrown out on arrival would still
have walked a work tree over the network to get it.

**An answer about a folder somebody has already left must not reach the band**, so there are
two guards rather than one — the read in flight is the only one whose answer counts, and its
path still has to be the folder in view. Navigating between two checkouts in one pane is the
test, and it goes back to the first one afterwards, because a band that only ever changed
once would pass two navigations.

**The band's own test waits for the band, not for the event loop.** The git read is a task of
its own and lands after the listing, so a number of rounds of `settle()` would have been
waiting on a clock — enough on this machine and not enough on a slower one. It waits until
the strip says what it is supposed to say, and reports both texts when it does not.

## The guide did not mention dragging

**Asked for:** MOLE-89 — `operations.md` covered copying, moving, deleting and packing,
every operation as the keyboard performs it. Dragging is now a second way to do two of
them, and it is the way somebody arriving from another file manager will try first.

**What it turned out to be:** a section built around the four things that surprise people,
rather than a description of the gesture. What goes when rows are ticked and what goes when
they are not. That it is always a copy, with the reason — a move on this kind of gesture is
something the *receiver* performs, so offering one would mean trusting another application's
word before deleting. That a drop takes files and not addresses, so an image dragged out of
a web page is refused and says so. And that a file which is not on this computer takes two
drags, with one sentence on why: a gesture cannot be held open while a hundred megabytes
come over the network.

**One picture, and it is the banner rather than the gesture.** A drag in flight cannot be
photographed — `QDrag` wants a platform and a pointer — and a staged imitation of one would
be exactly the kind of picture this guide's rule exists to prevent. What the harness can
reach is the sentence the pane says while a drag is over it, so that is the picture, taken
by `tst_Walkthrough` immediately after asserting it says *3 items* and names the folder.

**Only the one picture was copied in.** `make guide-images` rewrites all forty-nine, because
the sidebar shows the machine's real free space — the rest were left as they were, which is
what [TODO.md](TODO.md) says to do.

## A file that was not on disk could not be dragged anywhere

**Asked for:** MOLE-88 — Mole's own argument is that a bucket, a NAS and an archive are the
same kind of drive as the disk. Dragging is where that stopped being true: `text/uri-list`
carries a url the receiver opens for itself, and there is nothing to open inside a zip.
MOLE-84 left those rows out of the payload on purpose; this gives them one.

**What it turned out to be:** a scratch directory `DragSource` owns, filled by a
`TransferTask`, and one rule about *when*. **A drag cannot wait.** The gesture is over long
before a hundred megabytes arrive, there is no way to start a `QDrag` once the button is up,
and blocking is not available either — this is the UI thread, and a 2 GB read over SFTP
would freeze the window with no progress and no cancel. So the first drag of a row that is
not on disk starts the fetch and says so, in the task strip like any other transfer plus a
line saying the drag will work once the files are here; the next drag carries them.

**`TransferTask` rather than `ReadFileTask`**, because it streams instead of holding the
file in memory, it expands a folder into everything underneath it, and it is what weighs the
arrival at the destination ([ADR-0016](docs/adr/0016-a-copy-is-weighed-at-the-destination.md)).
A file that leaves Mole half-copied is worse than one that could not be dragged at all,
because nothing downstream will ever question it.

**Dragging the same file twice fetches it once.** A staged copy is reused while the source's
size and modification time still match what they were when the copy was made — recorded at
staging time rather than read off the copy, because a transfer does not carry a modification
time across and comparing against one that was never set would re-fetch every time. The
stat that checks it happens only on a drag that already has a copy to reuse, so a first
drag never pays for it. A staged *folder* is reused as it stands: deciding whether a tree is
still the same tree costs what fetching it again costs.

**What is left behind now means something narrower.** Before this, every row that was not on
disk was reported as left behind; now only the ones no drive is mounted for are, because
those are the ones nothing could fetch. The tests from MOLE-84 still hold unchanged, which
is the evidence that the line moved rather than blurred — they name unmounted drives, and
those are exactly the rows that still cannot go.

## Nothing on screen took a drop

**Asked for:** MOLE-87 — `dropHere()` could copy what was dropped and there was no
`DropArea` anywhere in the application, so a file dragged over the window was refused by
the desktop before Mole ever heard about it. This is the half the user meets.

**What it turned out to be:** one `DropArea` over the whole pane rather than one per view —
a pane is one place to put something whether its rows are drawn as lines or as tiles. It
reads `dropPlan()` on entry and shows what would happen while the pointer is still moving:
*Copy 2 items · 3.4 kB → Documents*. That sentence is the point of the feature, because the
count and the destination are exactly what somebody wants confirmed before they let go over
a window with two panes in it.

**A read-only pane takes no part at all.** `enabled` is bound to the pane's `writable`, so
the desktop shows a drag that cannot be dropped there rather than Mole showing a failure
about something the user has already committed to — and the pane says *read-only* in its
status line throughout, which is the sentence that explains the cursor. The test uses a
mounted archive, because that is what a read-only drive in Mole actually is, and not a
wrapper that only declares itself one.

**A collision opens the confirmation that already exists.** `transferDialog` gained a second
way in rather than a second dialog that looks like it: a count, a size, a destination, the
names that clash and what to do about them are the same question however it was asked, and
two dialogs would be one more place for the wording to drift. Its rename-on-arrival field is
the one thing a drop does not offer, because what arrives keeps its name and renaming is a
keystroke away once it is here.

**`drop.accept(Qt.CopyAction)`, never `acceptProposedAction()`.** The proposed action of a
drag out of another file manager is frequently a move, and accepting it tells the sender its
file may be deleted. That one line is the difference between a copy and somebody else's data
loss.

**The harness gained a synthetic drag** — a `QMimeData` of urls delivered as
`QDragEnterEvent`, `QDragMoveEvent`, `QDragLeaveEvent` and `QDropEvent`, offering copy *and*
move so what Mole does with the proposal is asserted rather than assumed. One rule stays out
of reach: a drop whose `drag.source` is not null is ignored, and a synthetic drop cannot be
given a source. [TODO.md](TODO.md) says so, and says which controller-level rule holds the
damaging half of it instead.

## A dropped file had nowhere to land

**Asked for:** MOLE-86 — the other direction, and the more common one. Something has just
come out of a mail client or a download folder and belongs in the folder already open. Mole
accepted nothing, so the file went somewhere else first and was moved afterwards.

**What it turned out to be:** `dropPlan()` and `dropHere()` on the pane controller, and six
rules decided here rather than left for the window to guess at. **A drop is a copy**, even
when the source offers a move — Mole does not delete another application's file because
something was dragged out of it. **Only `file://` urls are taken**, and a payload with
nothing local in it is refused *out loud*, because the commonest way to produce one is
dragging a picture out of a web page and a drop that silently does nothing gets reported as
a bug. **Nothing is overwritten without an answer**: `Fail` is the default, `dropPlan()`
names the collisions from the listing the pane has already loaded, and the caller passes back
`skip`, `overwrite` or `stop` — the same three words `runTransfer()` already takes. **A
read-only destination refuses** in the wording a transfer already uses.

**The rule that would have been missed is the source under no mount at all.**
`VfsManager::resolve()` answers from the mount table, and Home plus the system volumes are
not the whole disk — an ordinary download folder is under nothing. The local backend is
stateless, so the answer is to construct one rather than refuse a perfectly ordinary file.
That case has its own test, and the test asserts the premise as well as the outcome: that
`resolve()` really does answer nothing for the source it then copies from.

**A drag that ends over the folder it started in does nothing, and says nothing.** Those
rows are left out by parent path, which is also what keeps a pane-to-pane drag from asking
the user about collisions with itself. It is told apart from a payload with no files in it,
because one of the two deserves a message and the other does not.

**`FaultyFileSystem` gained `readOnly()`.** A drive that declares it cannot be written to is
what a mounted archive is, and every operation that writes needs an answer ready for one.
One line in the shared wrapper rather than a fourth hand-written fake — the same argument
the wrapper was built on.

## Nothing in the window ever asked what a selection looked like from outside

**Asked for:** MOLE-85 — `DragSource` could say what a selection looks like to the rest of
the desktop, and no gesture anywhere reached it. Press a row, move the pointer, and the
files are on their way to whatever is underneath.

**What it turned out to be:** a `DragHandler` with `target: null` on both delegates, which
is the whole trick. The row is not dragged out of the layout and the handler moves nothing —
it reports that Qt's own threshold has been crossed, and until it is, the delegate keeps the
click and the double click it already had. Both are load-bearing and both are what a
gesture added on top breaks first, so both are asserted in the same file.

**What goes is decided in the controller, not in the markup.** `dragTargets(row)` answers
the ticked rows when `row` is one of them and that row alone when it is not — the same
selection `targets()` reads, so the two cannot disagree about what is ticked. What differs
is the fallback, and deliberately: F5 acts on the row under the *cursor*, a drag on the row
under the *pointer*. Dragging an unticked row while ten are ticked must send one file, and
must leave the ten ticked and the cursor where it was. A drag is not a selection.

**The harness had no pointer at all, and now has one.** Press, move, release and a
`dragFrom()` that steps past the platform's drag distance rather than jumping — a single
large move can arrive as a teleport that no handler reads as a drag. Delivered to the
`QQuickWindow` through `QTest`, the same path `key()` uses, so it works offscreen where
`xdotool` never did. Rows are found through the view's own `itemAtIndex()`, which is what
knows where a row ended up after layout and item reuse.

**The `QDrag` itself is four lines in the shell**, with the window as its source and
`exec(Qt::CopyAction)`, plus a badge saying how many when it is more than one — a cursor
with nothing attached says nothing about whether forty files are moving. That is the part no
test can reach, which is exactly why the seam was put above it.

## A selection had no form anything outside Mole could take

**Asked for:** MOLE-84 — the first piece of dragging, and the one with no gesture in it.
`grep -rn "QDrag\|DropArea\|text/uri-list" src/` found nothing, so handing a file to a
browser's upload box meant leaving Mole and finding the file a second time in another
manager.

**What it turned out to be:** `src/ui/DragSource`, one `QMimeData` carrying `text/uri-list`,
built the way `FileLauncher` hands a single file to the desktop — including the seam. The
step that gives the payload to the platform is a hook, so `src/ui` constructs no `QDrag` and
`tests/ui` stays on a `QCoreApplication`, headless, like every other binary there. A `QDrag`
wants a real window as its source, a pointer to follow and a platform to block on; none of
that exists in a test and the platform half exists on no runner.

**`Qt::CopyAction` and nothing else, which is the decision rather than a default.** A move
is something the *receiver* performs — it takes the bytes and then trusts the source to
delete. Offering one would make Mole's behaviour depend on which modifier a user held while
releasing over a window that need not report what it did. The worst case of a misunderstood
copy is a duplicate the user can see; the worst case of a misunderstood move is a file that
is gone.
[ADR-0040](docs/adr/0040-what-leaves-the-window-is-a-path-and-it-leaves-as-a-copy.md)
records it with the two escape hatches that lost: a lazy `QMimeData` filling itself from
`retrieveData()` — which runs on the UI thread at the moment the receiver asks, so a 2 GB
SFTP read becomes a hang with no progress and no cancel — and `XdndDirectSave0`, which is
X11-only, unimplemented by most receivers and dead on Wayland.

**Rows that are not on disk are left out, and counted out loud.** A `text/uri-list` names a
file the receiver opens for itself and there is nothing to open inside a zip. Where that
empties the selection nothing starts and the reason is reported; where it only thins it, the
drag goes with what is left and how many stayed behind is reported too. A drag has no result
to inspect afterwards, so silence there is indistinguishable from a broken pointer — and
half a selection leaving quietly is the one outcome this must not have. Giving those rows a
path of their own is MOLE-88.

## The guide described two searches and a form with three criteria

**Asked for:** MOLE-157 — the page described two tabs, three criteria and an index that was
either used or not. After this epic it had to describe one search that can be asked a dozen
things, an index that answers some of them, a content search that reads files, and results
that come from two places at once and say which.

**What it turned out to be:** the page rewritten around the four questions the project
answers rather than around the two tabs that no longer exist — where, what you know about
the file, what is in it, and what about the things that are not files. Each of the earlier
tickets had already left its own paragraph behind; this is the pass that made them one
argument instead of seven additions.

**The paragraph the ticket really wanted is the one about not indexing contents.** A
camera, a lens and a date taken are a few dozen bytes; the photograph is eight megabytes.
An index of what a file says about itself sits beside your files; an index of what is in
them is the files again. It is in the guide in the user's terms, in one paragraph, because
that is the question that gets asked again otherwise — and the sentence that the index can
be deleted without losing anything but time is the other half of the same point.

**Two new pictures, generated rather than taken.** A folder answered by both halves with
the provenance visible, and a content search with its progress. Both come from walkthrough
tests that assert what the picture is supposed to show before taking it, so a picture
cannot quietly stop matching the application.

**All forty-eight are regenerated, and that is not only the new two.** The search form grew
a query line and a coverage sentence, and the result rows grew a marker — both of which
appear in more pictures than the search's own. Regenerating the lot is right for an epic
that changed what a search looks like; it would not be right for a two-picture change, and
[TODO.md](TODO.md) says why.

## Every criterion was a widget, and a search run twice was a form filled in twice

**Asked for:** MOLE-156 — a proposal rather than a request. For somebody who reaches for
`Ctrl+F` twenty times a day the fastest interface is a line of text, and every search tool
of the last twenty years converged on the same answer.

**What it turned out to be:** a parser, a printer, and a rule that neither the line nor the
form is the master. Typing in the line moves the fields; changing a field rewrites the
line. That is the whole point: the line teaches the form's vocabulary to somebody who
started with the mouse, and the form explains the line to somebody who started by typing.
One flag guards the loop, because without it the two chase each other while somebody is
still typing.

**The syntax is the field names and nothing clever.** Bare words are a name substring,
`key:value` with the four comparisons, `-` to negate, quotes for a space, commas for a
list, and slashes for a pattern — which is the one place a pattern is guessed at, and the
reason the name field has an explicit mode everywhere else. The metadata keys are keys like
any other, which is what makes the vocabulary one thing rather than two.

**Everything is `and`, and the parser's own header says so** — no `or`, no brackets, no
precedence. A general boolean language is a different feature with a different interface,
and stating its absence where somebody would add it is the point of writing it down.

**A query nobody can read does not run.** `size>10Q` is a complaint with the word marked,
and `extn:pdf` asks whether `ext` was meant rather than quietly becoming a name search.
The vocabulary check lives with the criteria rather than in the parser, because it grows
with the index: a camera is a key on a volume that recorded one and not on one that did
not.

**Round-tripping is a test rather than a hope.** Parse, print, parse gives the same terms
for every example in the ticket — a drift there would have the line and the form
disagreeing about what was asked.

## Re-indexing walked the whole tree again, and nothing ran a scan on a clock

**Asked for:** MOLE-155 — a re-scan wrote every entry whether or not anything had changed,
which on a 4 TB tree is hours to learn that nothing much has moved. And a scan was only ever
started by hand, so an index was as fresh as the last time somebody remembered.

**What it turned out to be:** a carry-forward and a job kind. A folder whose modification
time has not moved has the same children, so its subtree is copied into the new generation
rather than re-walked — which composes with MOLE-146's rule exactly as the ticket said it
would: an incremental scan touches less, so the swap at the end is smaller and not
different.

**The property that makes it correct rather than only fast:** nothing is ever carried
forward that the walk did not just see in a listing. A deleted folder is not in its
parent's listing, so nothing carries it, so it goes — whatever any timestamp says.

**Two honesty rules, and one of them was found by the test.** A drive that dates none of
its folders is walked in full and says so. And a folder is only trusted when its recorded
time is *strictly older* than the last scan: a folder changed in the same second the scan
read it carries the scan's own timestamp and would look unchanged for ever after. That
window is small, permanent and impossible to notice from outside, and the first version had
it — the test that adds a file to a folder and re-scans caught it immediately.

**The fixture was lying, in the same shape a backend can.** `MemoryFileSystem` did not move
a folder's own time when something was added to or removed from it, which is exactly what
an incremental scan reads. A fixture that did not would have made this look right here and
be wrong on a disk, so it moves now — and it grew a `setModified` so a test can date a tree
that was, unavoidably, built a moment ago.

**Scheduling goes through the automation that exists**, not a second scheduler: one job
kind, registered beside the analysis one, incremental by default. It survives a restart and
catches up on a missed run because that is what the scheduler already does; the test holds
it for this job type. That is deliberately where freshness is decided — nothing judges an
index too old to use, because a folder that changes often is one somebody indexes often.

## A file inside a zip could be found by no means at all

**Asked for:** MOLE-154 — `report.pdf` inside `backup.zip` did not appear in a search, was
not in the index, and the only way to know it was there was to remember the archive. On a
disk with years of zipped-up projects, that is where a great deal of what somebody is
looking for actually lives.

**What it turned out to be:** the scan looking, and one column. Every piece was already
here — a plugin mounts an archive as a drive, and a uri inside one is an ordinary uri the
preview, the operations and the file sets already understand. Core has no idea what an
archive is, so the rows come from whoever does: `ScanTask` takes a container reader the
same way it takes a fact reader, and the plugin layer builds one out of whichever mounted
factory claims that kind of file. A build without the archive plugin has no such factory
and indexes nothing, silently, which is what the ticket asked for.

**The column is the part that was actually broken.** A hit's uri was rebuilt from the
volume's scheme and authority plus the stored path — true of every file in a walked tree
and false of a file inside an archive, which lives on an `archive://` authority naming the
container. The first version of this found the member and handed back
`file:///buried-treasure.txt`, a path to a file that does not exist. Schema 4 adds a
nullable `uri`, used only by rows not addressed the way their volume is, and the test holds
both kinds side by side.

**The bounds are in one place with a comment each.** Twenty thousand entries per container,
because one file holding a million would make every search over that volume answer for it.
Nothing over thirty-two megabytes on a drive where listing means fetching. And a container
inside a container is a row and is never opened — following one is a recursion with no
floor and a bad failure mode.

**A content search reaches inside too.** The reader resolves per uri rather than being
captured once, mounting a container on demand, so a member is a file like any other to the
`Content` cost class — bounded by the same ceiling, which applies to what comes out rather
than to what is stored.

**One of the two failures on the way was in the test, and read exactly like a product
bug.** Every hit came back with an empty uri, which looked like the new column not being
written. It was a range-`for` over `Result::value()` of a temporary: the Result dies at the
end of the full expression and the loop reads memory that has gone. Every other loop of
that shape in the repository ranges over a named local, which is why this had never bitten
before.

## The form could not say that an index would let you ask more

**Asked for:** MOLE-153 — once the index answers questions the filesystem cannot answer
cheaply, some of what the form can ask depends on where it is being asked. The author named
this as the hard part, and it is.

**What it turned out to be:** three rules and one sentence. Never hide a criterion because
the scope cannot answer it — the section is always there, greyed with a reason, because a
field nobody can see is a capability nobody discovers. Never silently ignore one either —
asking for a camera over an unindexed folder **stops** the search, because that question
does not mean *everything*, it means it could not be put; the two ways out are *index this
folder* and *search only the indexed part*, both one click. And the plain search stays
plain: a name, Return, nothing else, which is the regression this was most likely to cause
and the one the tests hold hardest.

**The sentence is the whole interface problem solved in one place.** *indexed 3 days ago,
with what the files say about themselves* · *part of this folder is indexed, names only;
the rest is walked* · *not indexed — names, sizes, dates and contents only*. It is what
makes a greyed field read as inapplicable rather than as broken.

**Which fields exist follows the keys, not a list written here.** `IndexDatabase::factKeys()`
answers what a volume was actually recorded as stating, on the same generation join a
search makes — so a scan in progress cannot offer a field for facts nothing can yet find,
and a plugin that records a new fact gets a field without anybody editing the form. The
test proves it with a key nothing in this application has ever heard of.

**Narrowing says what it left out.** A search that quietly shrinks its own scope is the
same fault as one that quietly widens it, so the status line names the folder that is no
longer being searched.

## The index knew nothing about what was inside the files it listed

**Asked for:** MOLE-152 — a name, a path, an extension, a size and a date. So *the
photographs from that camera*, *the documents this person wrote*, *the videos longer than
an hour* could not be asked at all, at any price.

**What it turned out to be:** one narrow table and a key on a fact. `file_facts(file_id,
key, text, num)` with an index for each way of asking, because the fields come from readers
plugins may add and a migration per EXIF tag is not a design. `FileFact` gained a
namespaced `key` and a `number`, so the reader that fills the details panel is the reader
that fills the index and the two can never disagree.
[ADR-0039](docs/adr/0039-what-a-file-says-about-itself-is-indexed.md) records the keys, the
shape and the line: metadata is indexed precisely because the contents are not, and the
asymmetry — a camera is a few dozen bytes where the photograph is eight megabytes — is the
whole argument rather than a convenience.

**It is version 3, not the version 2 the ticket asked for.** Version 2 was already spent on
the scan generations from MOLE-146. The append-only rule the migration code states in a
comment is what made that a non-event: a database written by the previous version gains an
empty table and loses nothing, and the test winds one back and opens it to hold that.

**Off by default, with the cost said in files.** Reading metadata is bounded per file and
unbounded in aggregate, so it is a choice made where the number of files is known — at the
scan — and the dialog says *one read per file* rather than calling it slow. A scan with it
off writes exactly what it wrote before, asserted down to the status line.

**The same criterion works on a drive nobody scanned**, by reading the file instead. Same
readers, so the answer cannot differ; what differs is the cost, which is what
`PredicateCost` was built to say and what the planner uses to leave it until last.

## There was no way to search for what is inside a file

**Asked for:** MOLE-151 — the missing half of a search tool. The name is what you have
forgotten; the contents are what you remember.

**What it turned out to be:** a predicate in the `Content` cost class, which MOLE-147 built
the ladder for and nothing had yet stood on the top rung of. Because the contents are
deliberately never indexed, it is a filter over candidates rather than a lookup, and every
other decision follows from that: it runs last, so *PDFs containing "invoice"* opens only
the PDFs; it is bounded, so a 40 GB disk image is not searched by accident; and it says
what it left rather than passing over it.

**Windows with an overlap, and the test that proves the overlap.** A file is read a
window at a time, the way the preview layer reads one, with a page of the previous window
carried in front of the next so a match lying across the boundary is still found. Removing
the overlap was tried on purpose: the test fails, which is what makes it a test.

**Binary is decided by the sniffer, not the suffix.** `FileType::looksLikeText` already
answers exactly this question for the preview layer, and a suffix list would be wrong
about the files people actually have — a program called `.txt` is skipped and a note
called `.dat` is searched. *Binary too* does a plain byte search, which is occasionally
the only way to find something and never the default.

**A hit says why it is a hit** — the line, trimmed, its number, and where in it the match
starts. A content search that answers with a list of names makes somebody open every one
of them to find out which it meant. The column is counted in the trimmed line, because a
position measured from an indent nobody can see marks the wrong characters.

**Several files at once.** The walk stays on one thread; the reads are what is worth
overlapping, so candidates are held in batches and opened together. Cancelling stops the
reads between windows, asserted through the token rather than by timing.

**One thing was nearly wrong in an invisible way.** The first version decided which
entries to hold back for reading by asking whether the plan reads *whole* files — which is
true of a content search and false of a type class, so a type-class search silently stopped
opening anything and matched everything that got that far. The existing test caught it. The
two questions are separate now and named separately.

## The search asked for a name, an extension and a size, and that was all

**Asked for:** MOLE-150 — three criteria and three toggles, in an application whose subject
is files at scale. Every one of the missing ones is something somebody looks for weekly.

**What it turned out to be:** nine families, all through the query model from MOLE-147, so
each is one predicate with one meaning and one test. Time, with both ends typed the way
people say them — `today`, `last 7 days`, `>30d`, `2026-03-01` — and `created` and
`accessed` where the drive reports them. A type class taken from what is inside the file
rather than from its name, so a `Dockerfile` is code and a photograph saved as `.txt` is a
picture. A name read as a substring, a shape or an expression, chosen rather than guessed
at. A path field, which is a different question. A list of folders not to descend into.
Files, folders, empty, hidden, and a depth. And the extension field takes a list, which it
never should not have.

**The one new mechanism is a reader.** A type class cannot be answered from a listing, so
the evaluator grew a way to ask for a page of the file — used only for entries that
survived every cheaper criterion, which is exactly the ladder ADR-0036 built the cost
classes for. `PredicateCost::Metadata` had been a declared rung with nothing standing on
it; this is the first criterion that does. No new ADR: the decision it rests on is
ADR-0033's, that a file is what is in it.

**Two things are refused rather than guessed at.** A date the parser cannot read adds no
criterion at all — narrowing by something the user did not manage to state would be worse
than ignoring it, and matching everything would be worse still. And a regular expression
that does not compile matches nothing, because a typo must not turn into a search of the
whole disk.

**Exclusions are counted in directories entered.** The test asserts the pruned walk listed
strictly fewer directories than the whole one, because a filter applied to what came back
would return the same list at the same cost — and the cost is the entire point on a disk
with `node_modules` on it.

**Every criterion round-trips through the tab's state**, because a query built out of nine
fields is not something to make somebody build again after a restart. And the form left
alone still behaves exactly as it did with three fields: a name, Return, and nothing else
to decide.

## A folder whose subfolder was indexed was treated as if nothing had been

**Asked for:** MOLE-149 — ADR-0005 ruled that partial coverage counts as none, and gave a
good reason: a list of which some rows are current and some are as old as the last scan,
with nothing on the row to say which. Partial coverage is also the ordinary case — people
index the big slow tree, not the disk it sits on.

**What it turned out to be:** the answer was in the objection. *Nothing on the row to say
which* — so it goes on the row. The index answers first and its rows appear at once, each
marked as a memory and carrying the date of the scan that recorded it; the walk then goes
over the whole folder and supersedes them one path at a time, so the list converges on the
truth while somebody is reading it. Once the walk has listed a directory it knows
everything in it, which is what lets a file deleted since the scan be taken back at the
moment that becomes knowable. [ADR-0038](docs/adr/0038-a-mixed-list-says-which-rows-are-remembered.md)
records it and supersedes that one rule of ADR-0005.

**The ticket contradicted itself, and the author settled it.** It asked for the walk to
skip the covered subtree *and* for the walk to supersede rows the index reported — which
cannot both hold, since a skipped subtree is one the walk never reaches. Three of the seven
acceptance criteria needed the overlap. Asked, the answer was to walk it: the index buys
time to the first answer rather than less work, and the case where the work really is
avoided is full coverage, where there is no walk at all.

**The marking is the feature.** Every assertion in the new `tst_MixedSearch` is about a
row's provenance rather than a count, because a build that mixed the two halves silently
would be the thing ADR-0005 refused. The suite covers all three shapes — fully covered,
partly, not at all — and the first two are unchanged from what they were.

**Two smaller things came out of it.** `DirectoryWalker` gained a callback for *this
directory has now been seen whole*, which is the only moment a walk can honestly say
something is missing. And the path-prefix test the scope rests on was a plain
`startsWith`, so a search of `/data` counted `/database` as inside it; it now requires the
boundary to be a separator.

## There were two searches, and which one you were in was your problem

**Asked for:** MOLE-148 — `Ctrl+F` walked a folder and `Ctrl+Shift+I` queried the index,
in two tabs with two forms and two result views. The difference between them was which
engine answered, which is an implementation detail wearing a keyboard shortcut.

**What it turned out to be:** a field. *Search in* offers this folder, a typed path, or
everywhere indexed; picking the last is what turns the search into the question the second
tab existed for, and the volume picker sits beside it as a filter rather than as a screen.
`Ctrl+F` is untouched — a box, a name, Return — and `Ctrl+Shift+I` opens the same search
with that scope preset, so the key in anybody's fingers still lands somewhere that answers.
[ADR-0037](docs/adr/0037-the-scope-is-a-field-not-a-second-tab.md) records it and supersedes
the one paragraph of ADR-0005 that kept the tab.

**The merge is a merge, not a deletion.** Everything the retired tab had is in the form:
the name box, the volume picker with its per-volume counts, the search button, the status
line, the busy indicator, *Scan a folder…* and its dialog, and the refresh that makes a
folder searchable the moment a scan of it finishes anywhere. It gained something too — the
results can become a file set, which that tab never could.

**A retired id is not a missing plugin.** Every session saved before this names
`mole.indexsearch`, and dropping those tabs would be the answer an uninstalled plugin
deserves. `IFeature` gained `absorbedIds()`, so the feature that did the absorbing is what
says so, and the restore lands on the merged search with the scope the old tab meant —
which it works out from the state that tab saved, since only it ever wrote a volume and no
root.

**What the engine choice still is.** ADR-0005's rule is untouched for a folder: the index
when it covers the subtree and the toggle is on, a walk otherwise, and the status line says
which answered. The toggle is hidden for the everywhere scope because there is nothing
there to turn off — searching everything ever scanned is the index by definition.

## A query was described twice, and nothing kept the two in step

**Asked for:** MOLE-147 — `IndexSearchQuery` and `LiveSearchTask::Criteria` held the same
list of fields, in two structs, with two evaluators behind them. ADR-0005 had already
written down what that costs; the epic above this ticket adds four more criteria, and each
would have been added to both, in two languages.

**What it turned out to be:** one `SearchQuery`, a list of predicates rather than a struct
of optional fields, and a planner that splits it per source. A predicate says what it
matches and what it costs; `matches(FileEntry)` is a pure function with its own test; and
`planSearch()` answers what a source will state in its own query and what is left over,
cheapest first. Nothing is dropped — a criterion the source cannot express is evaluated
afterwards, and the plan says so, which is what the status line and MOLE-153's form will
read. [ADR-0036](docs/adr/0036-one-query-with-a-cost-on-every-criterion.md) records why the
cost sits on the predicate rather than in a lookup keyed by field.

**The test that proves the merge lost nothing** asks one fixture tree the same nine
questions through both engines and compares the answers — every criterion that exists,
including the ones that only ever worked by accident on one side. It also insists each
question matches something, because a merge test where both engines return nothing passes
for the wrong reason.

**Two things were quietly wrong and are now written down.** The indexed path narrowed its
own answer by hand in the controller, because a volume can be a whole disk and the question
was about one folder in it; that is a `underPath` predicate now, so every future caller of
the index gets it rather than having to remember it. And the two engines' result caps were
5000 and 10000, picked independently and documented nowhere; they are one number now.

**One thing was nearly changed by accident.** The first draft exempted directories from a
size range — defensible, and not this ticket's decision to make. Both engines have always
compared a folder on the size the listing gave it, and they still do; the comment in the
evaluator says whose call it is to change that.

## Twenty results examined was twenty browser tabs, and the comment said the opposite

**Asked for:** MOLE-158 — going from a search result to the folder it lives in opened a
browser tab, every time. The comment above the code said the current tab was reused *so a
search does not leave tabs behind*, and the code did the reverse: it reused the current tab
only when that tab had an `activePane`, and a search tab has none. So from a search the
reuse branch was never the one taken.

**What it turned out to be:** the shape `previewFile()` has had all along, applied to the
folder side. A tab already records the tab it was opened from — that is how closing it
hands the user back — so *the browser I opened from this search* needed no new state, only
`TabsModel::rowOpenedFromCurrent()` to ask the question. `revealFile()` and `openLocation()`
now both go through one place that answers *the browser this tab opens for*, so a folder
result routed through `goTo()` lands in the same tab as a file result revealed beside it.
The comment is gone: one that contradicts its own code is worse than none, and this one is
why nobody noticed.

**The half that was already right is the half that had to survive.** The search tab is
never closed, replaced or navigated, so the results, the narrowing filter and the scroll
position are all still there. The test asserts the first two directly and the third through
what would destroy it — a model reset, which is what MOLE-32 was about.

**The tab now says where it came from.** Three folders further in, which tab held the
results is a guess, so a browser opened from another tab carries one line above its
contents naming that tab and returning to it. It is a model role rather than a lookup, so
renaming the search renames the way back and closing the search removes it — both of which
`tst_TabStrip` holds, a new suite that drives the real window because a visible control
deserves better than a unit test of the property behind it.

**One thing had to move to make room.** A tab's delegate is now a layout rather than the
loader itself, so `tabStack.itemAt()` no longer answers with something that has an `item`.
Four places in the shell reached for that, and all four now go through one
`currentTabItem()` — which is where they should have been anyway. Focus was the first thing
to break and `tst_KeyboardNavigation` said so immediately.

## A rescan emptied the index first, and the search said so confidently

**Asked for:** MOLE-146 — `ScanTask` called `clearVolume()` before the walk, so for as
long as a rescan ran the index held only the part already re-walked. A search over that
volume came back fast, sure and short: no error, no warning, no *still scanning*.

**What it turned out to be:** a generation on the row. Every row carries the generation of
the scan that wrote it, every volume names the one generation that is its contents, and a
search reads where the two agree — so it is answered by the whole of the previous scan or
the whole of the new one, never by the half that has been reached. `commitScan()` drops the
old generation and points the volume at the new one in a single transaction, which makes
the swap an instant rather than an interval. `clearVolume()` and `markVolumeScanned()` are
gone; the first cannot be called without bringing the fault back, and the second was only
ever the second half of a commit. [ADR-0035](docs/adr/0035-a-scan-is-swapped-in-not-cleared-and-refilled.md)
records why a column beat a shadow table.

**The same fault had three more endings, and they are all one fix.** A cancelled rescan, a
failed one and one killed with the process each used to leave the index emptied, because
each of them stopped after the delete and before the refill. Now none of them commits, so
none of them changes what a search can see — and `last_scan` moves only on a scan that
finished, so *when this was last indexed* stopped meaning *when one was last attempted*.

**Holding a scan still needed something the fixture did not have.** `FaultyFileSystem`
stalls at a byte offset, and a scan opens no files — it only lists. So it gained
`listStalls(path)`, in the same shape as the read stall: the walk stops before that
directory and stays stopped until `release()`, and the test waits on `isStalled()` rather
than on a clock. The tree it is held in has more entries than one insert batch holds, so
the scan has genuinely written rows by the time the assertions run — a smaller tree would
have passed against a version that had merely moved the `DELETE`.

**The test that was already there had its premise changed rather than its assertion
weakened.** `tst_KilledOutright` used to kill a first scan and check its rows were
searchable, which is now the wrong expectation: a first scan that never committed is
nothing, correctly. The victim now finishes one scan and is killed during the next, and
what it had before the kill is what it must still have after it — a stronger claim than
the one it replaced.

**The migration is the part that could have gone silently wrong.** Both new columns default
to nought, which is what every row and every volume in an existing index already reads as,
so nothing goes blank on upgrade. `tst_IndexDatabase` builds a version 1 database by hand
and opens it, because a migration tested against a database the migration built proves
nothing.

## The guide described the fact list as the last resort, and it had stopped being one

**Asked for:** MOLE-137 — after this epic the guide's *Anything else* section was wrong, the
README's roadmap promised two libraries that were not taken, and two more preview providers
had arrived since MOLE-115 counted the guide's coverage.

**What it turned out to be:** three sections and three pictures. *What a file is* explains
the content pass with a coloured `Dockerfile` in front of it; *Details* says what a
photograph, a document, a video and an audio file each add, that it is closed until asked
for, and that reading it puts nothing on the network — a picture's position is the numbers
in the file and is not looked up anywhere. The hex window's section was written when it
landed and says what it is for, that it pages, that a selection copies as hex or as text,
and that it is read-only, in the same words the page uses for PDF and SQLite.

**The roadmap now says what is true.** Image metadata and audio tags are done and neither
`exiv2` nor `taglib` was linked, for the reason ADR-0034 gives; video *playback* is still
owed and MOLE-37 is still its ticket. The extension-point table gained the fifth point and
lost the heading that called it three.

**All forty-six pictures are regenerated, and that is not only the new one.** Every preview
picture now carries the Details header, the fixture gained a `Dockerfile`, and the sidebar
prints the machine's free space in every window — which is the churn [TODO.md](TODO.md)
warns about. Regenerating the lot is right for an epic that changed the preview strip; it
would not be right for a one-picture change.

## An audio file's tags were inside it and never on the screen

**Asked for:** MOLE-136 — an audio file carries its title, artist and album inside it and
Mole showed a size and a date. Four tag formats, a bounded read, and no cover art.

**What it turned out to be:** `AudioMetadataReader` over ID3v2.3 and 2.4, ID3v1, Vorbis
comments in FLAC and Ogg, and an MP4 `ilst` — the last through MOLE-135's box walk, used
rather than written again, which is the reason that walk is in a header at all.

**An estimate is labelled.** An MP3 with no Xing or VBRI header has its length written down
nowhere, so it is arithmetic on the bitrate and the file size: right for a constant bitrate,
wrong for a variable one. The row says "(estimated)". A FLAC's `STREAMINFO` carries the
total sample count, so that duration is exact and says nothing.

**Three bugs, all in the fixtures, all worth the time.** `"\xa9ART"` is not four bytes: the
hex escape swallows the `A`, because `A` is a hex digit — so the tag name in the *reader*
was wrong in exactly the same way as the one in the test, and the two wrongs did not cancel.
`\xa9nam` and `\xa9gen` were fine and `\xa9alb` and `\xa9day` were not, which is the kind of
half-working nobody notices. Both are now written as `"\xa9" "ART"`. The other two were the
test writing an ID3v1 track number and an MP4 `trkn` a byte out of place: the reader was
right and the fixture was the corrupt file.

**No cover art, deliberately.** It is the one thing in a tag block that is megabytes rather
than bytes, and nobody asked for it.

## A video file was a size and a date

**Asked for:** MOLE-135 — how long a video runs, how big the picture is and what codec it
is in are all in the container's header, and nothing read it. Three container families, a
bounded read, and every length in the file treated as a claim.

**What it turned out to be:** `VideoMetadataReader` over three parsers — ISO base media
(`.mp4`, `.mov`), EBML (Matroska and WebM) and RIFF (AVI) — with the ISO box walk exposed
in the header rather than hidden in the reader, because MOLE-136 wants the same walk for
an `.m4a`'s tags.

**A tail read is a window into the middle of a file, and that is the part that had to be
learned twice.** An ISO file not written for streaming keeps its index at the end, so the
reader takes one bounded read of the last 256 kB — and the first attempt walked that buffer
from byte zero, which lands in the middle of the payload and finds nothing. The index is
*found* in a tail window, by looking for its name and checking that the length in front of
it is one the buffer could hold; a wrong guess fails the same bounds checks as everything
else.

**Two fixture bugs found by the same test.** `FaultyFileSystem` did not forward `seek()` to
the stream underneath, so every tail read through it silently returned the head — a wrapper
swallowing an offset looks exactly like a reader ignoring one, and it would have quietly
weakened any future test about reading the end of a file. And the reader now refuses to
read a tail from a drive that cannot seek, rather than being read through to reach it.

**The sanitizers found the third.** `QPdfDocument` keeps its loader's buffer when a load
fails, through `load(QString)` as much as `load(QIODevice*)` — so there is no overload to
choose instead and nothing of ours to free. Suppressed by module, like Qt's scene graph,
and the suppression list is no longer described as being only for QML tests.

## Nobody could see who wrote a document

**Asked for:** MOLE-134 — a PDF renders its pages and says nothing about its author, and a
`.docx` had no viewer at all although the author's name is a few hundred bytes of XML a
short way into the file. Two readers against MOLE-132's interface.

**What it turned out to be:** `PdfMetadataReader` beside the PDF viewer, and
`DocumentMetadataReader` for the zip-based families, plus `membersFromZipPrefix()` in the
archive backend — twenty lines that pull named members out of the *front* of a container.

**The prefix is the whole idea.** A zip's local headers appear in stream order, so the
members near the front can be read without the central directory at the end of the file:
256 kB answers for every writer we have seen, and a container that keeps its properties
further in costs its author's name rather than a hundred megabytes of transfer. The reader
says which of the two happened rather than reading like a document with no author. The
bound is asserted through a counting drive at the app level, on a four megabyte container.

**The PDF is the exception, and it is written down rather than hidden.** A document's
metadata is reached through its trailer at the *end* of the file, so there is no prefix that
answers. A local file is opened where it lies and Qt reads what it needs; anything else is
fetched — a second time, the viewer having fetched it once. That is a consequence of the
panel being closed by default, and a local copy shared between a viewer and a reader is a
caching decision of its own.

**Two hostile inputs, both tested.** A zip that is not a document contributes nothing —
which is not a corner case but the ordinary outcome of OOXML, ODF and a bag of holiday
photographs all being zips to a magic rule. And an XML entity naming a local file is not
resolved: the test writes a real secret to a real path, builds a container whose `core.xml`
points at it, and asserts that no row anywhere contains it. Same rule as ADR-0006's
`<img>`: previewing a file puts nothing on the network and reads nothing it was not asked
to.

**The hand-built zip moved to `tests/support/ZipFixtures.h`** the moment a second suite
wanted one, which is where a fixture belongs.

## A photograph showed its pixels and nothing about the photograph

**Asked for:** MOLE-133 — a photograph opens as a picture fitted to the pane and Mole says
nothing else about it. Write a reader against MOLE-132's interface: dimensions from the
header, EXIF for JPEG and TIFF, GPS as numbers, no new dependency.

**What it turned out to be:** `ImageMetadataReader`, two IFD walks and a formatter, about
four hundred lines with no dependency added. `README.md` promises exiv2 for this and the
promise is not kept, for a reason that is now in
[ADR-0034](docs/adr/0034-what-a-file-says-about-itself.md): a reader is handed a *prefix*
of a file that may be on a remote drive, and exiv2 wants a path or a whole buffer — so
using it would mean fetching a 60 MB raw file to read forty bytes of it, or writing the
bounded read underneath it anyway. The roadmap line is MOLE-137's to correct.

**Every offset in the file is treated as a claim.** An IFD entry says where its value lives
and how long it is, and a corrupt or hostile file says whatever it likes. Each range is
checked against the buffer before it is followed, and one that leads outside costs that tag
and nothing else. The test writes the hostile file itself — a tag pointing at 0x7fffff00,
and a sub-directory pointer bent the same way — and asserts that the tags around it are
untouched. Same lesson as [ADR-0010](docs/adr/0010-archive-entries-are-not-paths.md)'s
archive entries: an offset inside a file is not a promise.

**A header read, never a decode.** Dimensions come from `QImageReader` over a `QBuffer` on
the prefix, so a 60 MB raw costs the same kilobytes as a thumbnail. The test states it as an
equality rather than as a hope: the facts from the first 64 kB are the same list as the
facts from the whole file, and `wantsMore()` says when one page is not enough — which is the
only reason a second, bounded read ever happens.

**Two bugs the tests found.** A camera with a make and no model came out as `"Nikon "` with
a trailing space, because the join was written for the usual case. And the app-level test
first failed with the model missing entirely — its hand-written EXIF block declared a
ten-byte string where nine bytes followed, and the bounds check refused it. The parser was
right and the test was the corrupt file; both are fixed, and the second is a small
demonstration that the check does what it says.

## A preview showed what a file looked like and never what it said about itself

**Asked for:** MOLE-132 — everything Mole knew about a file was nine facts out of
`stat()`, and they were only shown when no viewer claimed it. Build a fifth extension
point, a registry, and a details panel every viewer gets for free.

**What it turned out to be:** `IMetadataReader` in the SDK, `MetadataRegistry` in the
host, a panel in `PreviewView.qml`, and one reader — the generic one. The reasoning is in
[ADR-0034](docs/adr/0034-what-a-file-says-about-itself.md); three things about the build
are worth keeping here.

**The registry is deliberately not the preview registry.** That one stops at the first
provider that claims a file, because a file can only be drawn one way. This one returns
every reader that claims it, because a container and its contents are two sets of true
statements about one file and making them compete would mean one of them silently losing.
It is the same twenty lines of sorted vector with the loop ending differently, and the
difference is the whole design.

**The information viewer lost its facts rather than keeping a copy.** It builds nothing
now — the nine facts are the generic reader's, shown in the panel, so a file *with* a
viewer gets them too, which is the fault the ticket is about. That viewer opens the panel
by default, which needed a way for a viewer to say so without the shell knowing which
viewer it is: `IPreviewProvider::detailsOpenByDefault()`, one virtual with a default.

**Two tests were wrong before they were right.** The cancellation test waited for the tab
to let go of the reader and then asserted that the reader had noticed — two different
threads, and only the second is the claim; it passed or failed on scheduling. And several
tests here are about what is *remembered*, so a preference left behind by one was deciding
the next one's answer: `init()` now removes `preferences.json` as well as the session.
Both are the same mistake in different clothes — a test that reads state somebody else
wrote.

**A picture in the guide was carrying the clock.** The file photographed for the fact list
is written by the test rather than by the fixture, so its timestamp was "now" and the
picture was rewritten every day. Fixed date, like everything else in the fixture. The
Polish month name next to it is a real defect and a bigger one — it is in
[TODO.md](TODO.md), because fixing it rewrites every picture that shows a size.

## There was nothing in Mole that would show you the bytes of a file

**Asked for:** MOLE-131 — a stripped `.so`, a firmware image, a `.dat` nobody documented:
all of them reached the fact list, which reports nine numbers from `stat()` and nothing
whatsoever from inside the file. Build a hex viewer: read-only, windowed, with a selection
that can be copied as hex or as text.

**What it turned out to be:** `HexPreviewController`, `HexPreviewProvider` and
`HexPreview.qml`, in the shape the extension point already existed for — no change to the
SDK, no change to the registry. It claims at priority -500, below every viewer that
understands a format and above the fact list, and only on the content pass: `canPreview()`
is true when the type MOLE-129 found is set and is not text. A viewer for the files nobody
can name is the last place that should be guessing from a suffix.

**The consequence is that the fact list stops being the last resort**, which is what the
epic said would happen, and it is a bigger change than one more provider. Every readable
non-empty file that is not text now reaches the hex viewer, so what is left for the fact
list is a file with nothing in it, or one that could not be read. Two tests had to be
rewritten rather than retargeted, and one of them — the fact list naming a sniffed type —
is now asked of the viewer directly, because nothing routes an identified file there any
more.

**Selection is arithmetic, not a hit test.** Every column is fixed width, so the byte under
the pointer is worked out from the position and the two measurements the delegate and the
hit test share are declared once at the top of the view. Dragging lives in one MouseArea
above the rows rather than in each delegate, so a drag that leaves the row it started in
keeps going — the mouse grab stays put and the row is arithmetic on the y position.

**The 100 GB claim is tested at 100 GB.** A sparse file that size opens on its first
window, `lastWindow()` jumps to the tail, and the drive counts what it handed over: one
page to identify the file and two windows, out of a hundred gigabytes. The drive that
cannot seek is tested too, because a backend that streams is a real thing and the answer
has to be the error rather than an empty grid — `FaultyFileSystem` grew `cannotSeek()` for
it, and per-stream byte counts so "one read of one page and then one window" can be told
apart from "one read of both".

**Regenerating the guide's pictures rewrites all forty-five of them**, because the sidebar
shows the machine's real free space and it is never the same twice. Only the two that
changed for a reason are committed: the new hex window, and the fact list now showing an
empty file instead of `dump.bin`. This is in [TODO.md](TODO.md) so the next person does not
commit forty-three pictures of their own disk.

## Three languages the highlighter has always known were unreachable

**Asked for:** MOLE-130 — `Dockerfile`, `.gitignore`, `.bashrc`, `Jenkinsfile` and
`LICENSE` are all text and none of them previewed as text; and one layer down,
`SourceHighlighter` has had full `dockerfile`, `make` and `cmake` rule sets since it was
written while `suffixTable()` maps no name to any of them, so `Makefile` and
`CMakeLists.txt` were claimed as text and shown grey.

**What it turned out to be:** the first half was already done — accepting a sniffed text
type is what MOLE-129's own checklist required, so this ticket is the colouring. A file's
language now comes from `SourceHighlighter::languageFor()`, which asks three things in
order: **the name, then the type, then the suffix**. That order is the whole point, and it
is not the obvious one — for exactly the files this is about, the suffix is where the
information *is not*. `Dockerfile` and `Makefile` have none, `CMakeLists.txt` has one that
only says "text", and `.bashrc` has one `VfsUri::suffix()` will not admit to, since it
returns nothing when the only dot is at position 0.

That last quirk is left alone. Widening `suffix()` so dotfiles have one would change what
a preference key, a rename plan and every suffix test mean, to fix a handful of names that
an exact-name table fixes better. The two entries in `textSuffixes()` that can only ever
match `something.gitignore` stay too — harmless, and now beside the point.

**The type row earns its place on the files that have no conventional name either.** A
shell script called `deploy` is `application/x-shellscript` to the content pass and is
coloured as shell, which neither its name nor its suffix could ever have said. Both
spellings of that type are in the table, because shared-mime-info moved it and a database
older or newer than the one here should still colour a script.

**The 100 GB promise is held by a test rather than by a paragraph.** A two gigabyte sparse
file with no suffix and no line break in its first window opens on one page to identify it
and one 512 kB window to show it — asserted through a drive that counts bytes, with a
lower bound as well as an upper one so that a drive nobody asked cannot pass by reading
nothing.

## The preview layer decided what a file was from its name and never looked inside it

**Asked for:** MOLE-129 — `IPreviewProvider::canPreview()` is by contract a name, a suffix
and a size, and no provider may do any I/O, so a `Dockerfile` shows nine facts out of
`stat()` although it is plainly text. Read the head of a file that nothing claimed, put
what it is in `FileEntry::mimeType` — a field that has carried the comment *"the preview
layer sniffs the type lazily"* since it was written and has never been filled — and ask
the registry again.

**What it turned out to be:** `FileType::identify()` in `core`, a second lookup pass in
`PreviewTabController::showEntry()`, and two providers that read the answer. The shape is
in [ADR-0033](docs/adr/0033-a-file-is-identified-by-its-contents.md).

**The ticket said to use `QMimeDatabase::mimeTypeForFileNameAndData()` and let it apply
the freedesktop precedence. It does not apply it.** In Qt 6.4 a *unique* glob match is
returned without the bytes being consulted at all, so a zip renamed `notes.txt` comes back
as `text/plain` — the one case the whole ticket exists for, and it would have shipped
green if the test had asserted what the ticket assumed. So `identify()` asks the database
twice, once for the magic rules and once for the globs, and chooses between its two
answers: magic beats the name unless the name's answer is a subclass of it (a `.docx` is a
zip and the name knows more), and when both say text the name wins, because magic for text
formats is thin enough to call every C++ file C. The magic table itself is still the
database's; only the choice between two of its own answers is ours.

**The second surprise was the other direction.** The ticket asks for a "more generous" text
test than Qt's, for the case Qt answers `application/octet-stream` — and its own list of
cases wants a Latin-1 log to be text, which the rule as written (*"unless the decoder
errors"*) makes binary, since Latin-1 is not UTF-8. Bytes that fail as UTF-8 are read as
Latin-1 instead, where 0x80..0x9F is control characters too and the same 2% threshold
decides. A Latin-1 log is text because of a rule in `FileType`, rather than because Qt's
private heuristic happens to catch it first.

**A file is no longer shown before it is known.** The tab holds no viewer for the length of
one 4 kB read rather than opening the name's viewer and swapping it, so one viewer is built
per file and nothing reads a file it turns out not to be showing. That made `viewer()`
asynchronous for the fallback tier, which half a dozen tests were asserting synchronously —
each now waits on the condition, and `identifying` is what tells the view the difference
between "not yet" and "nothing can show this". A zero-byte file is not read at all: there
is nothing to read, and its name's answer stands.

The bound is held to account rather than described: `FaultyFileSystem` now counts opens and
bytes, and a 4 MB file with no suffix is identified in one open and one page.

## Five features and four viewers had no picture, and two no prose either

**Asked for:** MOLE-115 — Duplicates, Sync, File sets, Alerts and Reports appear in no
picture anywhere in the guide, and Sync and Alerts are not mentioned in any prose either.
Four of the seven preview providers are undocumented too, and no picture anywhere shows a
transfer actually running.

**What it turned out to be:** ten pictures, each with a test that drives the thing into
the state and asserts it before shooting — which is the guarantee the guide's README
makes, so a picture that got there any other way would be a lie about the whole document.

The part that matters most is not the ten pictures. **Five features were registered,
tested and shipped without anybody noticing they were undocumented**, which is the third
instance this week of the same shape: a fact declared in one place with nothing checking
it. So a test now holds three claims about every registered feature and every preview
provider — that it names a picture, that the picture exists, and that some page in the
guide actually shows it. An id in neither the map nor the exemption list fails by name.
Verified by removing Sync's entry and watching it fail. The exemption list is empty
today, and an entry in it is a decision with a reason rather than a gap.

The third claim earned its keep immediately: `03-preview-text.png` was being generated
and committed and no page in the guide showed it. A picture in a folder that nothing
displays is not in the guide.

**Duplicates was in the ticket as blocked** — it says to wait for MOLE-69, MOLE-70 and
MOLE-71, since that view is being rebuilt — and it is done anyway, on the author's call:
the picture is produced by the test suite, so when those land a regeneration corrects it
without anybody remembering. What is committed now honestly shows what exists today,
including how much empty space that view has, which is the fault MOLE-69 is about.

**Two things had to be built.** `tests/support/TableFixtures` writes the SQLite, Parquet
and image fixtures, so the guide's picture is of the same fixture the readers themselves
are tested against rather than a second copy that can drift; the image is drawn with
`QPainter`, which costs no licence question and no weight in the repository, and is made
larger than any pane so the picture shows the viewer fitting it.

And `MemoryFileSystem` gained `setReadThrottle()`, which is the difference between a
delay and a pace. A transfer in flight cannot be photographed with `setReadDelayMs()`: it
makes the file slow to *arrive* and leaves the copy itself instant, so the strip goes from
nothing to "finished" with no moment in between. Handing the bytes over at 256 kB every
60 ms gives a second and a half of a genuine transfer — a bar between the ends and a real
speed — for six megabytes of memory rather than the several hundred a real copy would
need to take that long. Two false starts before that: the memory backend builds its entry
uris with no authority, so a mount at `mem://camera/` hands back uris the manager cannot
resolve, and the transfer refuses with "one of the panes has no drive mounted" several
steps away from the cause.

Sync and Alerts have prose now, and the reasoning is in it rather than only the
mechanics: why a sync plans before it touches anything, and why an alert that never
clears is one people learn to ignore.

## The guide's pictures were of four small files, half drawn, at 1280x800

**Asked for:** MOLE-114 — three faults in one regeneration, because done separately
every binary in `docs/guide/images/` is replaced three times and git keeps all of it.
Pictures taken 40 ms into a 220 ms dialog transition; a window one rung too small;
and a fixture of four small files with every timestamp inside the same minute.

**What it turned out to be:** two of the three as described, one not.

**The grab now decides by looking at the window.** It takes a frame, waits, takes
another, and shoots when two are identical — rather than waiting a number chosen to
outlast whatever the longest animation happens to be, which is exactly the kind of
number that produced this fault. The cap is documented where it is set, because one
picture reaches it on purpose: `02b-preview-csv-loading` is deliberately a load in
progress and its spinner never stops. With that in place the twenty `settle()` calls
that sat in front of a screenshot are noise, and they are gone.

**The window is 1440x900** — one rung up the same 16:10 ladder, a quarter more pixels,
still opening whole on an ordinary laptop screen. 1600x1000 needs a viewport wider
than 1600 to be looked at without scrolling. Not a device pixel ratio of 2: four times
the weight for sharpness GitHub scales away anyway.

**The fixture looks like somebody's disk.** Twenty rows instead of four; seven folders
worth opening; sizes from 9 B to 224 GB, because `QFile::resize()` reports a size
without occupying it, so a 240 GB machine image costs nothing and the analysis picture
finally shows what an analysis is *for* — 249 GB across 21 files where almost all of it
is one file. Timestamps spread over two years rather than one minute. And contents
worth previewing: a changelog, an nginx configuration, a page of handover prose, a
4,000-line log, in place of `plain notes` and a stub.

**And the fixture directory has a name.** `mole-guide` rather than
`mole-tests-wAYrZa`, for a screenshot run only — the random name is right for an
ordinary run, where several go in parallel, and wrong for a run whose whole output has
that name in the tab and the breadcrumbs of every picture.

**Two regenerations of the same commit now produce 31 of 34 pictures byte for byte.**
The three that differ are pictures of something genuinely different each time: two
spinners, and one measured duration in the automation view. That was worth checking
rather than assuming, and it is what caught the folders — their timestamps came from
the clock until `setModified()` learned to handle a directory, which needs `utime()`
because a directory cannot be opened as a `QFile`.

**The part the ticket got wrong**, which is worth recording because the picture looked
exactly like the diagnosis. `13-compress.png` showed the listing through the dialog and
its labels half transparent, and the ticket put that down to the fade. It is not: with
the frame settled and four seconds of waiting, every picture with a popup open still
comes out with the whole window behind it at about forty percent — pixel (300,300) is
(158,160,164) where it should be (21,25,34), alpha 255, unchanged by any amount of
settling, and not the modal scrim either (declaring our own made no difference). It is
something in how the offscreen path renders a window under a popup, it is the last
thing wrong with these pictures, and it is now MOLE-128 with those measurements in it
rather than a guess.

Fifty-three `rowCount()` assertions and about thirty fixture-name references moved with
the tree. The ten tests that broke were all the same mistake — pressing Return on
whatever row 0 happened to be, which was `documents` only for as long as the fixture
had two folders — so they now name the folder they mean, through one `enterFolder()`
helper. The three filter tests picked new letters and kept their reasoning: `j` is
still in nothing but `settings.json`, `doc` still in nothing but `documents`, and `log`
now demonstrates substring-not-prefix better than `m` did, since it matches
`changelog.md` in the middle of the name.

## FTP was the last backend that staged a whole upload

**Asked for:** MOLE-34 — FTP stages a whole upload in a temporary file before
sending it, so a file larger than the local scratch space cannot be written to an
FTP drive. SFTP, S3 and WebDAV no longer do.

**What it turned out to be:** the machinery was already written and already used by
two backends, so this is `net::StreamingUpload` in place of `net::BufferedUpload`,
plus a `sendSpan()` that sets `CURLOPT_APPEND` for anything past the first span —
`APPE` rather than `STOR`, which is in the protocol beside it rather than an
extension.

One decision worth the line: **the span is a ceiling, not a working figure.** SFTP
sends in 256 MiB spans to keep every connection clear of an SSH re-key fault; FTP has
no such fault, and paying for a login and a data channel per span would buy nothing.
So FTP's span is set past any file anybody will hand it, and the loop is kept rather
than removed so that a file which does exceed it continues with `APPE` instead of
failing.

**This reverses a deferral in ADR-0014 and ADR-0015**, both of which said FTP keeps
staging because "changing a backend nobody is blocked on is change for its own sake".
That was a decision to defer rather than a decision to stage, and being the last
backend that cannot write a file bigger than the disk is itself the reason to finish.
ADR-0014 carries the amendment.

**And a fault found on the way, which is now MOLE-127.** `openRead()` stages too, so
FTP cannot *read* a file bigger than the disk either. It is not folded into this
change on purpose: streaming a read needs ranged fetches, and whether libcurl honours
the end of `CURLOPT_RANGE` for FTP or only the `REST` offset decides whether a span
can overrun into the next one and hand the same bytes over twice. There is no FTP
server in the build environment to settle that against, and a read that silently
duplicates a span is worse than one that needs scratch space, so it is written down as
its own task with the measurement to make first.

The test needs no server, which is the point of it: a staged write is a different
class from a streamed one, so the test asks FTP for a write stream — against a port
nothing listens on, so the `stat()` on the way past is refused instantly — and holds
that what comes back is the streaming kind. What that class *does* is proven by the
conformance suite and the heavy tier's peak-scratch assertion, on the days there is a
server.

## A task with a speed and a size now says how long is left

**Asked for:** MOLE-30 — a task that measures bytes already knows its speed and
its total, so it should say how long is left.

**What it turned out to be:** the arithmetic, and three decisions about when to
keep quiet. `Task::setBytesDone()` already kept a smoothed rate and already knew
the total, and `TaskMetric::Kind::Duration` already existed so a view could format
a time without parsing text — so the estimate is published there, beside the rate,
and every task that measures bytes gets it at once rather than each one growing its
own.

The three decisions, in the order they matter:

- **Nothing until the rate has settled.** Three closed sampling windows, so the
  first figure is never one taken from the opening half second, where a copy is
  still spinning up and the estimate is wrong by multiples rather than by percent.
  A wrong estimate is worse than none: it is read once, believed, and remembered.
- **The smoothed rate, not the instantaneous one**, which was already there for the
  speed and for the same reason — a figure that changes every frame is not a figure.
- **A stall holds the last estimate.** Dividing by a rate on its way to zero gives
  something that runs off to hours and then to infinity. The last figure that meant
  something is more honest than that.

One thing that fell out of it: `Kind::Duration` now formats a negative value as an
empty string. "Not known" is a real state for an estimate — before the rate settles
and again once there is nothing left — and the model already drops a metric with no
text, so that is how the column comes and goes instead of showing "0s left" on a row
that has stopped.

Four tests: the estimate appears and is in the right ballpark, a task with no total
says nothing, a stalled one keeps its last figure and never prints infinity, and the
strip carries it — that last one through the real window, because the property name
in the binding is the part a C++ test cannot get wrong. `ScriptedTask` moved to
`tests/support/` on the way, since "a task that takes a measurable amount of time and
reports as it goes" is now wanted by two suites.

## Five of the thirteen "New … tab" entries opened onto nothing

**Asked for:** MOLE-73 — the File menu offered a *New … tab* per registered feature,
including a preview of no file, a duplicates view whose own first line reads *"Open
this from a folder to search it"*, and a sync with neither endpoint set. Give
`IFeature` a way to say which kind it is, order what remains, and keep everything
reachable.

**What it turned out to be:** mostly a deletion, and a third instance of the same
underlying shape. Two methods on `IFeature` already existed for questions in this
area and were read by nobody who needed them: `needsContext()` was documented as
*"the shell uses it to avoid offering a tab that would open onto nothing"* and only
the empty-window buttons read it, and `sortOrder()` was documented as *"ordering hint
for the new-tab menu"* while the menu used registration order. A declared method with
no reader is a promise the code is not keeping, which is the same thing MOLE-79 and
MOLE-100 were about.

There turned out to be **three** kinds of feature, not two, and that is what decided
the design:

- **Opens from nothing** — a browser, a search. You open several, from cold.
- **Needs a subject** — a preview, a bulk rename, a duplicate scan, a sync, an
  analysis. `needsContext()` is the existing name for this.
- **A standing tool that exists once** — the alerts list, the saved reports, the
  schedule, the sets. It opens perfectly well empty, so `needsContext()` is correctly
  false for it, but *New Alerts tab* is still the wrong entry: there is only ever one
  alerts list, and *Saved reports* says what it is. ADR-0003 had already put those in
  Workflows.

So `opensFromNothing()` is a new predicate rather than a reuse of the old one, and the
File menu now holds four entries — the two browsers and the two searches, in
`sortOrder()`, read for the first time.

**The default is `false`, and that was the decision worth making rather than falling
into.** Defaulting to `true` keeps a third-party plugin's entry working without its
author knowing the method exists, at the cost that every wrong answer is a menu entry
that opens onto nothing — the fault being fixed — and nothing would notice. Defaulting
to `false` makes a wrong answer a *missing* entry for something still reachable three
other ways. The failure mode that is visible and recoverable won.

**And the reachability is checked rather than asserted in prose.**
`MenuAction::opensFeature` names the feature an entry opens, and a test fails, naming
the feature, when a registered one is reachable by no action at all — which means
nothing in the window and nothing in the palette can get to it, since the palette is
built from the action registry. Verified by removing one link and watching it fail.
Without that, tidying the menu is one edit away from orphaning a whole feature.

## The passphrase was asked for at the one moment nobody has a reason to answer

**Asked for:** MOLE-111 — stop asking at startup. A drive that needs the credential
store should read as inactive until something asks for it; opening it should be what
connects it; and the passphrase should be asked for at that moment, in a dialog
rather than in a strip of the sidebar.

**What it turned out to be:** three changes with one shape, and a fourth that fell
out of the first.

The band above the drive list appeared whenever anything was waiting, and every such
drive wore an amber dot — all before anybody had asked for any of them. Nothing had
gone wrong: the store is shut at every startup and may stay shut all session. That
band was itself a fix, for something worse (a drive used to wait at startup in
complete silence), and its trade was recorded in a commit message rather than in an
ADR — which is part of why it went unexamined. It is gone. What stays is the news:
the row still says `Locked`, its tooltip still says so, and it still offers a key
rather than a play triangle. What changes is the colour: `Locked` joins `Disconnected`
in `idle`, because amber is what a drive on its way to failing wears and a drive
nobody has opened is not a problem.

`AppController::goTo()` — where the sidebar row, the palette and the bookmarks all
arrive — now connects the configured drive behind a uri when nothing is mounted
there. It used to hand the uri straight to the pane, `VfsManager::resolve()` found
nothing, and an unconnected drive was shown as a folder with nothing in it. A connect
that fails now says so.

And when that drive needs the store and the store is shut, the passphrase is asked
for **then**, in a modal centred on the window, and the navigation finishes on its
own once it is open. This reverses half of MOLE-44's "never a modal" and keeps the
reason behind it: a modal at startup for a drive nobody asked for is still the wrong
trade, and this one only ever appears because somebody just asked. ADR-0031 records
it, including what it used to do.

`UnlockBand.qml` is gone and `UnlockDialog.qml` is the only place the copy lives —
the sentence about the passphrase not being tied to this computer is the thing people
need to hear exactly once. The drives dialog gets a line and an *Unlock…* button that
opens the same dialog, rather than a second copy of the paragraph.

**Two things that had to be built to make it work**, both worth the paragraph:

- **A button that acts without closing.** A passphrase can be refused, and the dialog
  is the only place that can say so — but `DialogButtonBox`'s accept role closes the
  dialog before anything can be reported. The shared footer gained
  `actWithoutClosing`, which reports Apply instead, so the dialog decides whether to
  close. The alternative was closing and immediately reopening, which is a flicker
  and a lie about what happened.
- **`goTo()` now says whether it navigated.** The sidebar row hands the keyboard to
  wherever it just went, which is right until the click puts a modal up: handing the
  keyboard to the listing behind a modal takes it *out* of that modal for good — the
  popup is left holding no focus, so nothing inside it can take the keyboard
  afterwards either. That cost an hour and looked exactly like `forceActiveFocus()`
  being ignored.

**A fault in the test fixture, found by the same test.** The locked-drive fixture
gave the SFTP backend a host and a password and no user name, so `create()` refused
before any I/O and the drive never connected. Both suites had been asserting only
that the drive "left `Locked`" — which it did, for `Disconnected`. The fixture is
complete now and the drive really does connect, which is what let the last assertion
in the walkthrough be the one that matters: the pane ends up at the drive that was
asked for.

Five tests rewritten rather than deleted — they were the tests for the old flow, not
tests that happened to touch it — and three new ones: that startup asks nothing, that
the key and the palette reach one dialog, and that a drive with no secret still comes
up with nothing typed.

## ADR-0010 reached six dialogs out of thirteen

**Asked for:** MOLE-100 — the copy and move dialog has two identical buttons and
the keyboard on neither, which is the arrangement ADR-0010 replaced. Close the
whole gap rather than the one place it was noticed.

**What it turned out to be:** the gap was bigger than the report, and the reason
it existed is the part worth keeping. ADR-0010 landed the shared footer, converted
the six dialogs that existed then, and predicted in writing that a dialog setting
`standardButtons` would go back to two identical labels. Three did. The worst of
them was not the one reported: the reports view offered to delete every saved run
for a folder — history that cannot be recovered — on a button labelled *Ok*, in the
same grey as the one next to it. Four more dialogs opened with the keyboard nowhere
at all, so Return did nothing and there was no focus ring to see.

The footer was opt-in, nothing failed when a dialog did not reach for it, and
nobody was watching. So the fix that matters is not the seven dialogs; it is that
**`standardButtons` now fails the build**, naming the file, in the same shape of
configure-time check as the one beside it that catches a QML file missing from the
application. A rule kept by remembering lasts until the next contributor.

The seven: `transferDialog` gets *Copy* or *Move*, `scanDialog` *Index*,
`forgetDialog` *Forget* in red with the keyboard on the way out; `transferHint`, the
drives dialog, `aboutDialog` and `shortcutDialog` get the same footer with
`dismissOnly`, which is one outlined *Close* button that holds the keyboard.
`keyboardOn` now states where the keyboard starts — the acting button, the way out,
or a field of the dialog's own — instead of leaving it to be inferred.

**Two things about Qt that cost the most time**, and are in the ADR for the next
person:

- **`focus: true` on the right button does nothing.** `DialogButtonBox` lays its
  buttons out with a `ListView`, and a `ListView` hands the keyboard to its own
  current item — whichever button ends up first. Every dialog was quietly opening
  on the way out, including the ones where that is the wrong button, and the
  property said otherwise. The footer places the keyboard imperatively as well.
- **A focused `Button` answers Space and not Return.** Return is a dialog-level key
  and Qt Quick's `Dialog` only turns it into an answer when a standard button is in
  charge — the very thing the footer replaced. So the footer handles Return itself.
  Without that, the focus ring sits on a button Return does not press, which is a
  more convincing kind of broken than no focus ring at all.

**And one fault found on the way, which is the reason for the second changelog
line.** The transfer dialog's *Name* field — the one that lets a single file arrive
under a different name — has never appeared. The row was `visible: nameField.visible`
and the field was `visible: <there is a single name>`; `visible` on an item is its
effective visibility, parents included, so the row waited on the field and the field
waited on the row, and both settled on false for ever. Found by a test asserting the
keyboard was in that field, which is what testing through the real window buys.

## A file with no line breaks in it stopped the window

**Asked for:** MOLE-112 — `F3` on a 13 MB JSON export left the window not
answering, and it did not come back in any time anybody would wait for.

**What it turned out to be:** not the size. The preview reads a 512 kB window and
never holds the file, so this reproduces on a 600 kB file and would not reproduce
at all on a 13 MB file with lines in it. What mattered is that the window had *no
line break in it*: `ReadRangeTask`'s line snapping is a no-op when
`lastIndexOf('\n')` returns -1, so 524,288 bytes arrived as one line of 514,694
characters, and everything downstream of the window assumes lines. The layout has
to itemise and shape a single block that long before the `ScrollView` can be told
how wide its content is, and one line has no partial layout to fall back on; the
highlighter colours per block, and a window of minified JSON is around twenty
thousand `setFormat()` calls with a format kept per character while it runs.

The fix folds in `updateDisplayText()`, which already existed to derive what the
view shows from what was read: any run longer than 4,096 characters is broken up,
so the window arrives as about 126 blocks instead of one. Colouring goes off for a
folded window for two reasons rather than one — the twenty thousand `setFormat()`
calls go with it, and a fold cuts strings in half, so the highlighter, which
carries nothing across a block boundary but block-comment state, would colour the
second half of every cut token as something it is not. The language is unchanged,
so paging on to a window that does have lines in it gets its colour back.

Three things were decided rather than fallen into:

- **Measured against every character Qt's text engine starts a block on**, not
  just `\n`. A file with old Mac line endings is already in blocks as far as the
  engine is concerned, and folding it would have been work for nothing.
- **The fold never lands between the halves of a surrogate pair**, which would
  have put an unpaired code unit either side of the break and shown two
  replacement characters where the file has one emoji.
- **Only where a line break is what makes a block** — the plain text and source
  case. Markdown and a rendered page parse their own blocks out of the markup and
  fold a newline inside a paragraph back into a space, so a fold there would
  change what is shown and fix nothing. Recorded in TODO.md rather than left to be
  discovered.

The view says so, in the colour of a caveat: *long lines folded, colouring off*.
The reader has to know the breaks in front of them are Mole's and not the file's.
One cost that goes with it and was accepted rather than missed: text selected out
of a folded window carries the folds. This is a preview, not an editor, and a
window that can be read beats a window that does not answer.

Four tests on the controller and one through the real window. The one through the
window is the one that matters, because it is the only one that lays out a frame:
with the fold turned off it does not finish in five minutes, and with it on it
takes 1.8 seconds. Pretty-printing the JSON would be a different feature and is
not this: the rule has to hold for a minified stylesheet, a one-line log and a
base64 blob, none of which have a pretty form to be put into.

## Code now sits level with prose, not a shade under it

**Asked for:** MOLE-93 — plain text and source previews drawn one step larger,
level with the primary text everywhere else.

**What it turned out to be:** one number. `monospaceSize` was 13 against
`textSize` at 14, and that entry in the scale is read in exactly one place in the
whole application — the body size in `TextPreview.qml` — so raising it to 14
raised the plain text and source preview and nothing else. Rendered Markdown and
rendered pages already took `textSize` and did not move.

This reverses half of a decision made when the scale was introduced (see *The type
was too small, and there was no scale to raise* below): code was to read a shade
smaller than prose. A log or a source file is the preview people leave open the
longest, and being the smallest thing on the screen apart from captions was the
wrong end of that trade. The assertion `monospaceSize() <= textSize()` stays, since
what it stops — code drifting *above* prose — was never the part that was wrong.

Not an offset inside the view, and not a preview-only size: the scale is where
sizes are decided, and a second knob would let a preview and a listing disagree.
No new test is owed — the scale is asserted as a shape rather than as five
numbers, and the shape has not changed.

## Phase 5, the catalogue: fourteen groups, and the eleven faults they found

**Asked for:** MOLE-1 through MOLE-18, the tests themselves — around 205
scenarios weighted towards what can destroy data, taken one group at a time.

**What it turned out to be:** fourteen tickets, about 250 new cases, and eleven
faults in the product. The suite went from 63 binaries to 72 and still runs in
33 seconds. Every group's card carries the detail; what follows is what the
exercise was actually for.

**Four of the faults could lose a file, and none of them had a test.**

- *A mirror deleted the far end when it could not read the near one.* One helper
  answered a failed listing and an empty directory identically, so a refused
  source listing read as "the source has nothing" and a mirror emptied the
  destination to match. [ADR-0030](docs/adr/0030-an-unreadable-source-is-not-an-empty-one.md).
- *A move deleted a file it had skipped.* `Conflict::Skip` reports a success, and
  the move then deleted a source that had been copied nowhere.
  [ADR-0029](docs/adr/0029-a-move-deletes-only-what-it-finished-moving.md).
- *A directory could be moved inside itself*, after which the delete pass removed
  the only copy of everything in it. Same ADR.
- *Deleting a shortcut to a folder emptied the folder it pointed at.* A link says
  yes when asked whether it is a directory, and `QDir::removeRecursively` walked
  through it.

**Two were security-shaped.** An archive entry called `../../etc/passwd` became a
path outside its own mount — and `/..` resolved to the root, so the root's
listing contained the root and any walk went round for ever. Entry names are
resolved and clamped now. The other is the same class one layer up: a backslash
in a file name was turned into a folder separator on every platform, so a file
with one in its name could not be addressed at all.

**Two were the framework lying.** A progress bar could read 150% and could slide
backwards. An exception escaping a task body ended the process — the test for it
did not fail, it called `std::terminate`.

**Three were arithmetic and ordering.** A task measured its own duration and
throughput by subtracting two wall-clock readings, so a stepped clock produced a
negative rate. A bulk rename that swapped or shifted names refused every row,
because the plan allowed a batch the task could not carry out in the order it was
listed. A duplicate scan hashed a file, and anything that wrote to it afterwards
left the hash proposing a deletion.

**The tools grew with the tests.** The fault injector gained a delete refusal; a
gated drive was added so a cancel can be aimed at a stage rather than a moment; a
paced server holds a transfer still one block at a time; `SftpFileSystem` gained
a seam so a server's answer can be supplied without a server. `make tsan` exists
beside `make asan`, and what it found is MOLE-126.

**What was struck through, and why.** Around forty scenarios need something a
fake cannot honestly provide: a case-insensitive filesystem, hard links, a
quota, a second real backend, or gigabytes. Each is struck through on its card
with the reason, and the two clusters worth scheduling are MOLE-113 (the sizes
where a backend changes strategy) and the live conformance run. Nothing was
dropped in silence.

## Five faults from one day, and the three that could still have come back

**Asked for:** MOLE-18. Each fault found in one day of working against a real
server becomes a named tier 1 test, because finding one costs a day and keeping
it costs a second. Five of them: a transfer that stops short of an announced
length, a read that fails being taken for the end of a file, a large SFTP read
crossing the point where the session re-keys, a listing of a file that has to
mean "not a directory" whichever way the server answers, and a forward seek
inside a stream's buffer that must not depend on how fast the network was.

**What it turned out to be:** two of the five were already held. `net::errorFor`
is covered by `tst_CurlTransport` — a short transfer, an unknown length, a HEAD
that asked for no body, and a 404 whose body arrived whole — and the read that
fails by `tst_TransferTask::aReadThatStopsHalfWayIsNotAnEndOfFile` next to the
truncation case, which is the other half of the same question. The other three
had nothing that would have caught them, and one of those three had a test that
looked as though it did.

**The re-key point had only a live test.** An SFTP transfer that runs past the
point where the session re-keys stops dead — full speed, then nothing, with the
connection open and the server there — and the answer was to fetch in spans, each
over a connection of its own. Nothing checked that on a machine with no server:
`tests/plugins/tst_StreamingDownload.cpp` now has a fake whose connection carries
only so much and then reports that nothing more arrived, and the file still
arrives byte for byte because no span is longer than the limit. It comes with its
control — the same server asked for the whole file in one go dies part way —
because a test that passes against a fake which does not actually misbehave is
worse than no test.

**Two kinds of SFTP server, neither reachable offline.** Asked to list a regular
file, one server answers with a "." row describing the file and another refuses
and says the path does not exist — true of the directory that was asked for, and
false of the file that is sitting there. Both have to come out as
`NotADirectory`, and nothing above the backend may have to know which kind of
server it is talking to. Neither answer can be arranged on a server that is
behaving, so `SftpFileSystem::fetchListing()` is now a seam: the transport on one
side, and on the other everything that decides what an answer means. Five tests
sit behind it, including the one that keeps the rule honest — a path that really
is missing stays `NotFound`, rather than every failed listing becoming "that is a
file".

**A seek that looked tested and was not.** `StreamingDownload::seek()` answers a
short hop forwards out of the transfer already running, and the rule is a
*distance* — up to one bufferful — rather than what happens to have arrived. The
test that existed read a kilobyte, seeked fifty forward, and passed whether or
not the bytes were there. Two now replace it: one at the far edge of the buffer
and one byte past it, which pins the distance; and one where the server has
handed over a single block and is holding the rest, which is the state a stream
that consulted its buffer would give up in. `kStreamBufferBytes` moved into the
header for the first of those, because the size of the buffer is part of what
seek promises rather than an implementation detail.

**Each new test was checked against the fault it names** by putting the fault
back — the span loop removed, the seek made to consult the buffer, the "." row
dropped like any other dot entry, and "no such file" passed on as it stands. Each
one turned exactly the expected tests red and left the rest green.

**One clock survives, in the seek test, and it decides nothing.** The state under
test is a transfer that is holding bytes back, and only the test can let them
through; releasing them immediately would answer the easy question instead. So it
waits a moment first, and what it asserts is the count of transfers started —
which, once a second one begins, is wrong for ever, whether it began inside that
window or after it.

## The server attacked while a transfer runs, and what it did not survive

**Asked for:** MOLE-28. The same transfers with the machine being interfered with
while they run: the connection killed at a byte offset in each direction, the
service stopped and restarted, 200 ms of latency and 1% and 5% packet loss, an
outage shorter than the stall guard and one longer, the destination disk filled,
the host key changed between two operations, and the process killed outright.
Each one to produce a *named outcome* rather than a hang.

**What it turned out to be:** `tests/scale/tst_Interference.cpp`, eleven cases
over the control channel from phase 1. Nine of them hold, and each one interferes
**at a byte offset** — waiting on `Task::bytesDone()` rather than on a clock, so
the attack lands in the middle of the transfer on a fast machine and on a slow
one alike.

What holds: a connection cut at 25% either finishes or fails by name, with
nothing left under the name it was aiming at. A server restarted mid-transfer
comes back and the drive works again afterwards. Latency and loss are survived
with every byte verified through the same bad link. **A changed host key is
refused** — the one SSH warning nobody may wave through. **A process killed
outright leaves nothing under the final name and the job runs again over the
wreckage**, which is what the working name from ADR-0020 exists for. And a
destination with less room than the payload fails with `no room left on the
server`, naming the file.

**One finding, and it is the serious kind.** A download cut off by a total outage
of 140 seconds — against a stall guard set to give up after 120 — neither failed
nor recovered. It sat there having moved 34 MB and was still sitting there **641
seconds later**, when the test gave up on it. The same outage at 119 seconds is
survived correctly and every byte arrives, so the recovery path works; what is
missing is the giving-up path. A job that neither finishes nor fails is the one
outcome a file manager may not produce, and it is MOLE-108.

**Three of the failures in this suite were the suite's own**, which is worth
recording because each was the same shape: a green test that checked nothing.
A transfer that finished before anything could be done to it passed every
assertion for the wrong reason, so interference now has to *prove* it landed
while the transfer was running. A `netem` clearer written as an anonymous
`sleep N; tc qdisc del` deleted whatever qdisc it found on waking rather than the
one it was scheduled for, so two cases in a row cut each other's outages short.
And filling a disk takes half a minute of `dd`, which lost its race with an
upload every time — so the room is taken away *before* the copy starts now, and
the payload is sized from what the machine says is left, which needs no timing at
all.

**Two cases are behind a variable, and that is a gap rather than a decision.**
The outage pair needs a total blackout, and the instrument for one cuts the path
that undoes it: `netem loss 100%` stops the machine answering ARP, so it is
unreachable to the timer that should clear the rule and to the command that would
check on it. It happened twice while this was being written, and both times the
way back in was the hypervisor's guest agent — which is now
`scripts/testbed/rescue.sh`, because a lesson learned twice belongs in a script
rather than in somebody's memory. A per-port blackhole is in the control channel
now and is closer, but until it is proven the two cases run on purpose rather
than unattended: `MOLE_TEST_INTERFERENCE_OUTAGE=1`. That is MOLE-109, and it
carries the port conflict it exposed — the host-key case rotates the second
server's identity, so a control channel arriving there refuses to speak to the
machine for the rest of the run.

**And the heavy tier came out of `make test`.** Two suites that move gigabytes and
need a server were in the ordinary run by label alone; `ctest --label-exclude
heavy` keeps `make test` something anybody can run.

## Gigabytes each way, and the wall that was still there

**Asked for:** MOLE-27. Transfers at the sizes that break things, in both
directions, byte-verified, with **peak local scratch space measured and
asserted** — the check that would have caught staging before it became a wall —
plus memory and descriptors sampled and asserted flat, a transfer crossing the
re-key point on both server configurations, and throughput recorded per run so a
regression in speed is visible as well as one in correctness.

**A hundred gigabytes was asked for and cannot run anywhere.** This machine has
80 GB free and the test machine 27 GB, so the tier takes its size from
`MOLE_TEST_HEAVY_BYTES` and defaults to 10 GiB. That is not a compromise on what
is being checked: the assertion is the *ratio* between the payload and the
temporary space it needs, and ten gigabytes proves that as well as a hundred
would. Where the payload does not fit, the case is a **skip with the reason**
rather than a pass — the WebDAV and FTP roots are on a small disk on purpose, and
filling it would take every other suite down with it.

**The tier asks the machine how much room it has**, through a new
`mole-control room <service>` on the control channel, rather than having numbers
about somebody's disk typed into a script in a public repository. A machine is
the only thing that knows its own layout.

**Verified byte for byte without holding ten gigabytes anywhere.** The payload is
a function of where it is: one megabyte of fixed pseudo-random bytes, repeated,
each block stamped with its own index. Every byte is compared against what
belongs at that offset, and the stamp catches what a plain repetition would hide
— a copy that duplicated, dropped or reordered a block.

**What ten gibibytes each way measured.** Local disk to itself at 329 MiB/s, to
SFTP at 25 and back at 16, to S3 at 47 and back at 62 — and **peak temporary
space of zero** on every SFTP transfer and 125 KiB at worst on S3, against a
payload of ten gigabytes. That is ADR-0014's claim about streaming, measured
rather than believed, and it is the number that would have caught staging.
Resident memory grew by 11 MB at its worst and descriptors came back. WebDAV and
FTP were skipped, out loud, because their roots have 2.73 GiB and the run needed
twice ten.

**And the tier found what it was built to find.** A read from the second
SSH server configuration — `chacha20-poly1305`, `RekeyLimit 256M`, which exists
on the test machine precisely to provoke this — stops dead at 267,780,096 bytes,
640 KiB short of exactly 256 MiB, and the stall guard fails it two minutes later.
[ADR-0013](docs/adr/0013-a-large-sftp-read-arrives-in-spans.md) fetches large
reads in 256 MiB spans on the reasoning that this is "far enough below the fault
that a server re-keying earlier is still covered". It is not: on that server the
re-key lands *inside* the first span. Writing to the same server is unaffected.
It stops at the same byte at 1 GiB and at 10 GiB, which is as reproducible as a
fault gets. It is MOLE-99, with the failing scenario left in place — named and
red — because the tier's job was to produce exactly that.

**Two things about running it.** QTest kills a test function after five minutes,
which is less than one large transfer takes on any link worth testing, so the
watchdog is raised rather than removed. And the tier is not part of `make test`:
it moves real data and takes real minutes.

## Every task, from a console, with no window

**Asked for:** MOLE-26. A binary that starts any task against any configured
drive and says what it did, so that a fault found by hand against a real server
is one command rather than a window, two panes and a drag; so that the scale tier
can run on a machine with no display; and so that a shell script can put a
transfer under `tc netem`. Deliberately not a scenario language — scenarios are
C++ tests. See
[ADR-0028](docs/adr/0028-a-console-runner-for-the-tasks.md).

**What it turned out to be:** `src/tools/`, a second binary in the same build,
linking `mole_core` and `mole_host` and nothing above them. Ten commands — copy,
move, delete, sync, compress, rename, scan, duplicates, verify, and `drives`,
which says what is mounted and how to address it. It reads the drives file, the
credential store and the index the application uses, because a runner that
connected its own way would reproduce its own faults.

**The exit codes are half of what it is for.** A script in a loop has to tell
"the copy failed" from "I typed the command wrong" from "the drive was never
there", so those are 1, 2 and 3, with 130 for an interrupt. Ctrl-C asks the task
to cancel rather than killing the process, which is what makes the half-written
file get cleaned up.

**Secrets never appear in an argument.** A configured drive is named with
`--drive` and its passphrase comes from `MOLE_PASSPHRASE`; a drive described on
the command line takes `password=@SOME_VARIABLE`, read from the environment. An
argument list is readable by every process on the machine, and a shell history
outlives the run.

**`sync` and `rename` say what they would do and stop**, as the interface does.
`--apply` carries it out. Nothing that can delete files does it on the strength
of a typo.

The commands are driven by the test suite in-process, so a failure reads as an
assertion rather than as a blob of captured output — with one test that starts
the binary for real with `DISPLAY` and `WAYLAND_DISPLAY` removed, because "runs
with no window" is a claim about a process and cannot be checked from inside one.

## A copy, going wrong thirteen ways, and two faults it found

**Asked for:** MOLE-25. A first vertical slice of hostile scenarios through
`TransferTask` — source renamed, deleted or truncated part way through, the
destination removed or filled up, cancellation before the first byte, mid-file
and between files, ten concurrent copies, a drive unmounted with a transfer in
flight — each asserting that no partial file is presented as complete, that the
source is intact, and that the failure says which file and why.

**What it turned out to be:** `tests/core/tst_TransferTaskUnderFault.cpp`,
thirteen scenarios, running in 44 ms with no sleep anywhere in it. Faults fire at
a byte offset through `FaultyFileSystem`, and a transfer is held still by
stalling its read rather than by hoping. The destination is the local disk rather
than the memory drive on purpose: the guard being tested lives in a backend, and
testing it against a drive that does not implement it would prove nothing.

Nine of the thirteen were red on the first run, and two of those were faults in
the product rather than in the test.

**A cancelled copy was renamed into place as if it had finished** — MOLE-97. A
local write goes under a working name and is renamed when it is closed, so an
abandoned one can be discarded; `~PartialFile` says exactly that in a comment and
then does the opposite. It calls `QFile::remove()`, which closes the file first,
and `close()` is virtual — so the call arrives back in `PartialFile`, finds a
write that has not committed, and commits it. The remove that was meant to
discard the file then fails with "No such file or directory", against the name
that no longer exists. Every failed and cancelled copy in the slice left a
complete-looking file behind. `MemoryFileSystem` reached the same outcome by a
different route: it committed from its write device's destructor on purpose.

The rule now lives in the backend conformance suite rather than in one test, so
every backend is held to it — local, memory, and the remote ones when there is a
server to run them against. It fails on both backends when either fix is backed
out, which is how it was checked.

**A read that ended early was reported as a copy** — the second fault, and the
reason for [ADR-0027](docs/adr/0027-a-read-that-ends-early-is-not-a-file-that-shrank.md).
`QIODevice` answers "the file ended" and "the connection went away and I am
pretending it ended" with the same zero, and a plan built from a listing cannot
tell which happened. So the source is asked again, once, and only when fewer
bytes arrived than expected: a file that really shrank is copied as it now is, a
source that still claims the larger size is a failure. The check happens before
the destination is closed, because closing is what puts it in place.

**And a short write now says why.** "short write" was true and useless — a disk
that filled up and a server that hung up read exactly alike, and neither is
something anybody can act on.

**One fault was left as a task.** `SyncTask::copyOne()` still reads with the
`QByteArray` overload and stops on an empty result, which is the older and worse
version of the same fault: a dropped connection is taken for the end of the file
and the file is counted as copied. It is MOLE-98 rather than a quiet fix here,
because sync's own hostile slice is what should land it.

## One drive that misbehaves on purpose, instead of three that each did once

**Asked for:** MOLE-24. Three fakes had been written in a single day —
`CommitFailingFileSystem`, `HalfReadingFileSystem`, `ForgetfulFileSystem`, all in
`tst_TransferTask.cpp` — each invented to reproduce one fault, each about forty
lines of delegation around the one line that misbehaved. A fourth would have been
written the same way. Write the general one, and make every fault fire on a byte
offset rather than on a clock. See
[ADR-0026](docs/adr/0026-faults-are-injected-at-a-byte-offset.md).

**What it turned out to be:** `tests/support/FaultyFileSystem`, a decorator over
any `IFileSystem` in the shape `LoggingFileSystem` already uses for every real
mount. Ten faults, each one something that had actually been seen: a read that
fails after N bytes, a read that goes short and recovers, a read that stalls
until it is released, a write that accepts everything and stores every fourth
byte, a write that only fails when it is closed, a file that changes size or
vanishes or is renamed under the reader, access withdrawn part way through, a
destination that fills up, and a listing that overstates a size.

**The chunk size nearly made the offsets a lie.** A fault declared at byte 1200
would first have fired at whatever multiple of 256 KiB the caller's read landed
on, so the wrapper clamps each read and write to stop exactly on the next offset.
`theOffsetHoldsWhateverChunkSizeIsUsed` reads the same file 7 bytes at a time and
4096 bytes at a time and demands the same answer.

**Each stream takes its own copy of the faults.** The first version kept the
fired flags on the policy, which would have meant ten concurrent copies of one
file producing one failure and nine successes — the exact scenario phase 2 was
written to check, quietly passing.

**A stall is the one fault that cannot be an offset**, since it is the absence of
an event. It is still not a clock: the stream stops at its offset and stays there
until `release()`, and a test waits for `isStalled()`.

The three fakes are deleted and the four tests that used them go through the
wrapper unchanged in meaning, down to the assertions on "stopped after 4 bytes"
and "4000 bytes were sent but 1000 arrived". The wrapper has its own suite,
`tst_FaultyFileSystem`, because a test tool that miscounts bytes would make
everything built on it green for the wrong reason.

## The board moves to Vikunja

**Asked for:** GitHub's GraphQL rate limits were making the Projects board
unreliable to work with, so move the tickets to a self-hosted Vikunja instead.
See [ADR-0022](docs/adr/0022-work-is-tracked-in-vikunja.md).

**What it turned out to be:** 67 issues, 11 comments, 20 labels, 9 milestones and
66 board cards, moved by a script that reads GitHub, writes Vikunja and then reads
the result back to compare the two. The comparison is the part that earned its
keep: the first run reported 41 faults, all of them real, and none of them
visible by looking at the board.

**Every ticket kept its number, and that took a trick.** `MOLE-19` is what `#19`
was, which is what keeps every `Closes #…` already in the git history — and this
file — pointing at the right work. Vikunja assigns the per-project task index
itself and will not accept one, but its counter is not rolled back by a deletion,
so the 29 numbers that had belonged to pull requests were reserved by creating a
task and deleting it again. That the counter behaves this way was tested rather
than assumed, including deleting the highest index, because the alternative
silently slides the numbering by one at every gap.

**Two positions of 100 are not two positions.** Numbering each column's cards from
1 again gave five cards the same position, and Vikunja recalculates every position
in a view when two of them crowd each other. The order came back *nearly* right,
which is the worst outcome available — it looked correct and was not. A position
is stored per view rather than per column, so one counter now runs across the whole
board.

**Labels belong to the account, not the project.** Deleting a project leaves its
labels behind, so the second attempt created a second `blocked`, a second
`area:ui`, and no way to tell the pairs apart. The script reuses a label with a
matching title now.

**The descriptions convert themselves.** Vikunja's API takes GFM Markdown on write
and converts it server-side, which was worth finding before hand-rolling a
converter: the 314 `- [ ]` lines across these tickets became real checkboxes
rather than the characters `[ ]`, and every table, fence and cross-reference
survived. Cross-references were rewritten to point at the board, with the code
blocks left alone — a link inside a code sample is damage, not a convenience.

**A milestone became three things**, because Vikunja has no milestone and no one
feature does the job: a label, a column in a second `By epic` view, and a task in
an `Epics` sub-project whose subtasks are the tickets. The tickets stay on the
workflow board, where an epic card would otherwise sit in `Ready` between the work
it is made of.

Then the GitHub issues were deleted and the Projects board with them, at the
author's instruction — one place, not two. The Issues tab stays open as a way in
from outside. Everything GitHub held was snapshotted first, and the verifier can
still be run against that snapshot, which is the only reason it can still be run
at all.

**Then the dispatch rule changed, and that is the part that changed the board's
shape.** The author asked for the work to be taken epic by epic rather than off a
flat `Ready` queue — see
[ADR-0023](docs/adr/0023-work-is-dispatched-by-epic.md). Reading `Ready` down the
column showed why: it jumped between subjects six times in the first fifteen cards.
The order the epics are now in was not invented, it was read off the last GitHub
board — each epic sits where its first `Ready` task sat — so the next task to do is
the same one it was before the rule changed.

The rule had a hole in it that the board did not show. **Twenty-one tasks belonged
to no epic**, four of them dispatchable and one of them the very next thing to do,
and a rule that reaches tasks through epics never reaches those at all. They are in
an epic now — `Loose ends` — and the `no epic` column of the `By epic` view is kept,
empty, so that a task falling out of every epic is visible instead of silent.

**And the two roles now write as themselves**
([ADR-0024](docs/adr/0024-planning-and-engineering-write-as-themselves.md)), which
turned up the one sharp edge in Vikunja's label model: a label can only be attached
by an account that can already see it in use, so an unused label is invisible to the
account most likely to want it, and the API says only `403`. Nothing that no task
carries is created any more. `blocked` was one of those, and it is not coming back —
blocking is a `blocked`/`blocking` relation between two tasks, which Vikunja has and
a label only approximated.

## Backends, beyond the current conformance (#15)

**Asked for:** eight things the conformance suite does not check, run against the
real servers.

**What it turned out to be:** the suite had a hole in the middle of it. **It never
called `openWrite`.** Every fixture was seeded out of band — correctly, because a
listing bug that mirrored a writing bug would cancel out — and the consequence
was that the write path of every backend was covered by nothing at all. That is
how a WebDAV backend that could not write a file survived for months, and why
#35's fix turned out to be half a fix: it covered the sizes at which curl asks
for `Expect: 100-continue` and left the small ones failing exactly as before.

The suite now writes through the backend at two sizes and overwrites what it
wrote, and it found three faults on the first run.

**WebDAV could not write a small file either.** The real cause was never about
size: an authenticating server answers the first request with a 401 and curl
retries, which means sending the body a second time, and curl had no way to
rewind a `QIODevice`. `Expect: 100-continue` avoids the retry only when curl
decides to ask, and it asks by size. The proper fix is one layer down —
`CurlPool::sendFrom` now gives curl a **seek function**. A staged upload is a
temporary file and can always oblige; a stream being written as it is sent says
`CANTSEEK`, which is true and is what makes curl ask permission before sending a
body it cannot repeat.

**And that immediately broke something else**, which is the best argument for the
scripted-server suite from #4. With the body rewindable, curl started *following
a redirect on a PUT* and re-sending the file to whatever address the server
named — the exact scenario #4 lists, caught within minutes by the test written
for it. `FOLLOWLOCATION` is right for a listing and wrong for a request carrying
a file, and it is now off for those. It had been harmless only for as long as the
body could not be replayed at all: an accident, not a policy.

**WebDAV handed back a directory as a file.** A `GET` on a collection is not an
error — Apache redirects to the same path with a slash and answers with an HTML
index, 200 and a body, with nothing saying it is not what was asked for. So
copying a folder produced an HTML page named after it. The redirect is the tell,
and asking where the transfer landed costs nothing where a `PROPFIND` before
every read would cost a round trip.

**The fourth failure was the server, not us.** FTP uploads failed with `426`,
intermittently and at no consistent size. Reproduced with plain `curl` and no
Mole involved: vsftpd's shutdown handling predates TLS 1.3 and misreads a data
connection closing cleanly, logging `FAIL UPLOAD` for transfers that arrived
whole. The environment is what had to change — `scripts/testbed/services.sh` now
pins the data channel to TLS 1.2, and eighteen uploads in a row succeed where
most had been failing. An intermittent failure in a test environment is worse
than a constant one, because it teaches everyone to re-run rather than to look.

## Killed outright, and what is left (#6)

**Asked for:** `SIGKILL` at a known offset, then an independent look at what
survived, then the same job run again — across six places a kill can land.

**What it turned out to be:** two faults, both of them losses of data the user
already had.

**A local copy destroyed the file it was replacing.** `LocalFileSystem::openWrite()`
opened the destination with `Truncate`, so the old contents were gone before the
first byte of the new ones arrived; a kill in between left neither. A remote
upload interrupted half way at least leaves the previous version alone, because
nothing on the server is touched until the write begins — the local one had
already thrown it away. And this is not the exotic case: a copy over a file of
the same name is exactly what a re-run of a failed copy is. The working name from
[ADR-0020](docs/adr/0020-an-upload-in-progress-wears-a-different-name.md) moved
out of the network plugin into `core/vfs/PartialWrite.h` and now covers the local
disk too — [ADR-0021](docs/adr/0021-the-working-name-is-not-only-for-servers.md).

That change broke sync's overwrite mode immediately, and rightly: ADR-0020's
"the destination must be free" had been quietly relying on the callers it
happened to have. The question is now asked at the moment that can answer it —
whether the destination existed *when the write began*. Existing then is an
overwrite and proceeds; appearing since is somebody else's file and is refused.

**Preferences were written with `QFile` and `Truncate`** while every other store
in the project already used `QSaveFile`. Preferences are written wholesale, so a
kill part way through the write is a kill part way through the only copy — and
what the test found on disk afterwards was an empty file where every setting the
user had ever chosen used to be.

**`tests/support/Victim`** is what made these testable. `SIGKILL` cannot be
simulated from inside a process that intends to keep running: no destructor runs,
no error path runs, nothing tidies up. So a real process does the work and a real
signal stops it — the test binary starts another copy of itself with an
environment variable naming what to work on, waits on a *condition* rather than a
clock, and kills it.

**The same trap twice.** Both times the first version of a test waited for the
working name to appear, and against the unfixed code that name never appears — so
the test failed with "the write never started", which is untrue and useless.
Waiting for *either* name makes it fail with `the file being overwritten
disappeared`, which is the fault. It is worth writing down because the mistake is
invisible when the test is green: a test that can only fail one way will
eventually fail that way for the wrong reason.

## The network misbehaves (#4)

**Asked for:** fifteen scenarios in which the network or the server does
something other than what it should, each one named for the loss it prevents.

**What it turned out to be:** two different jobs wearing one label. Most of the
list is not about the *network* misbehaving at all — it is about a **server
answering wrongly**, and a well-behaved server cannot be asked to do that. A 411
to a chunked PUT, a `Content-Length` that does not match the body, a listing cut
off half way through an XML document, a redirect aimed at a request carrying a
file: none of them can be obtained from the testbed, and all of them are things
some appliance out there does.

So they are scripted. `tests/support/ScriptedHttpServer` is a server that
answers wrongly on purpose — it runs on a thread of its own with no event loop,
because `curl_easy_perform` blocks the thread that calls it and a server sharing
that thread would never accept the connection. Six scenarios run offline in
under a second, on every change.

The one that matters most is the truncated listing. A mirror sync asks what is
on the far side and deletes anything that is not there; a listing that arrives
half-finished and is reported as a short directory is not a display problem, it
is an instruction to delete everything the answer was cut off before mentioning.

**The host-key test taught something about tests.** ADR-0011 says a host whose
key has changed is refused, always. The first version of the test recorded the
key, flipped one character of the base64 and reconnected — and the connection was
*accepted*, which looked like a serious security finding. It was not. A flipped
character stops the blob decoding, the parser discards the line, and a discarded
line reads as a host nobody has ever met — so it was accepted on trust, exactly
as it should be. Decoding the blob and changing a byte of the key material
inside it, leaving every length prefix intact, is what an impostor's key actually
looks like; against that the policy holds and the connection is refused. A test
that passes for the wrong reason and a test that fails for the wrong reason are
the same mistake seen from two sides.

`SftpSettings` gained `knownHostsPath` so that test could work on a file of its
own. Doing it to the account's real `known_hosts` would be interfering with the
machine the suite runs on.

**What is not here.** The scenarios that need a real network being mistreated —
latency, packet loss, the stall guard's two sides, a connection killed at a byte
offset, a server stopped and restarted mid-transfer — belong to the control
channel and are done under #28 rather than duplicated here. The issue records
which box went where.

## WebDAV met a real server, and could not write to it (#35)

**Asked for:** the WebDAV backend has never been run against a live server, and
has since grown a streaming write that has never been exercised — a large write
goes out with a chunked transfer encoding, and a server answering 411 would
refuse it. Run the conformance suite and a large streaming write against a real
server, green.

**What it turned out to be:** far worse than the issue expected, and nothing to
do with chunked encoding. The first large write returned

    Writing /large.bin.mole-partial: necessary data rewind wasn't possible

`CURLAUTH_ANY` — chosen so a user never has to care whether their server wants
Basic or Digest — makes curl send the first request with **no credentials**, take
the 401 and try again. The retry has to send the body a second time, and the body
comes from a `QIODevice` curl has no way to rewind. Meanwhile `Expect:` was being
appended to every request, switching off the one mechanism that would have made
the 401 arrive *before* the body did.

**It was not a large-write problem.** Reproduced with plain
`curl --anyauth -T -` against the same server: exit 65, `CURLE_SEND_FAIL_REWIND`,
at every size tried from one kilobyte upwards. Every WebDAV write of any real
size had been failing against any server that asks for a password. The only
reason the suite was green is that the conformance fixtures are a few bytes each
— small enough for curl to hold a copy and send it again.

The fix is one condition: keep `Expect: 100-continue` for a request that carries
a file, and suppress it only for the ones that do not. curl asks only when it
needs to, so a directory of small files pays nothing.

Two tests, both live, and both fail on the old code for the reason above: a
96 MiB write that asserts it took the streaming route, and an 8 MiB write that
asserts it took the staged one. Each checks the route with a `dynamic_cast`
rather than trusting a size threshold — a threshold quietly raised past the test's
file would otherwise turn one of them into a duplicate of the other, passing while
covering nothing.

**The lesson is about the fixture, not the backend.** A conformance suite that
only ever writes a few bytes tests a different code path from the one users
take, and it had been green for months over a backend that could not write a
file. Sizes that cross a real boundary — an authentication retry, a staging
threshold, a buffer — belong in the suite deliberately.

## An upload killed mid-flight left a partial file that looked finished (#33)

**Asked for:** an upload interrupted by the process being killed leaves part of a
file on the server, and what it leaves looks like a finished file. Write to a
temporary name and rename on success.

**What it turned out to be:** exactly what the issue said, and the interesting
part was where the fix could not go. [ADR-0014](docs/adr/0014-remote-files-stream-rather-than-stage.md)
already handles an upload that *fails* — the backend deletes what it wrote — and
that answer covers every failure the process is alive to see and none of the ones
it is not. `SIGKILL` runs no destructor and no error path. So no amount of
cleanup code was ever going to help; the protection had to be the name the bytes
were travelling under all along, which needs nothing to survive because it is
already on the server. That is [ADR-0020](docs/adr/0020-an-upload-in-progress-wears-a-different-name.md).

`StreamingUpload` and `BufferedUpload` gained a commit hook — one function, run
after everything has arrived and nothing has gone wrong, whose failure becomes
the stream's failure. It does not run for a stream that was cancelled or
abandoned, which is the case that matters: part of a file has gone up, and giving
it the real name would be the fault itself. SFTP, FTP and WebDAV write to
`<name>.mole-partial` and rename at the end. **S3 was deliberately left alone** —
a single `PUT` is atomic and a multipart upload does not exist as an object until
it is completed, so there is nothing there to protect.

**The test was the harder half.** A transfer that fails can be faked; a process
that is killed cannot be, because the whole point is that no code of ours runs
afterwards. So the live test spawns *this same binary* with an environment
variable telling it to be the victim, waits for the file to appear on the server
and sends it `SIGKILL`. It waits on the file appearing rather than on a clock,
and — this took a second attempt — it waits for the file under **either** name.
The first version watched only for `.mole-partial`, so against the unfixed
backend it sat there for ninety seconds while the victim uploaded two gigabytes,
and then reported "the upload never reached the server, so nothing was killed
mid-flight". Which was untrue, and was the least useful thing it could have said:
the whole question is which name the bytes are under, so both are worth waiting
for and the answer is what gets asserted. Watching for either, it fails in three
seconds with `a killed upload left mole-killed-555030.bin, which looks like a
finished file` — the fault, named.

Two things were found while writing it rather than being designed in. A `stat` of
the destination that fails for a reason other than "not there" was originally
treated as "the name is free"; it is now treated as "could not find out", because
guessing there is guessing about whether somebody else's file is about to be
replaced. And a rename that fails now removes the working file, so a failed
upload still leaves nothing behind — bytes under a name nothing will ever open
are litter, not a result.

## Only part of a large file arrived from an SFTP drive

**Asked for:** work out why files copied from an SFTP server to local disk came back
as fragments, and why large files threw timeouts. A log to diagnose it with was
asked for alongside, and then broadened: every task and every drive should report
itself the same way, whatever the question being chased.

**What it turned out to be:** an SFTP transfer stops dead a little short of a
gibibyte. The bytes arrive at full speed and then simply cease, with the connection
open and the server still there; two minutes later the stall guard gives up. Whether
that presented as "a timeout" or as "half a file" depended only on which layer the
caller was watching.

It is not ours. Plain `curl` does it with no Mole involved, and every attempt to
place the blame closer to home was disproved by measurement:

| what was done | result |
|---|---|
| `curl`, one transfer, fresh process | 1,211,049,311 bytes, complete, 53 MB/s |
| `curl`, a listing and then the file | stops at 1,072,635,904 |
| `curl`, a small file and then the file | stops at 1,072,635,904, the same byte |
| Mole, pooled connection | stops at 1,071,513,600 |
| Mole, `CURLOPT_FRESH_CONNECT` | stops at 1,071,529,984 |
| Mole, a brand-new easy handle | stops at 1,071,529,984 |
| `scp`, any size | fine, always |

Two wrong answers were held on the way, and both were killed by the table above. The
first was connection reuse -- the pool hands out warm connections, and the failures
all happened to follow a listing. A fresh connection stops in the same place. The
second was handle reuse, since curl's own failures shared an easy handle across two
URLs. A brand-new handle stops in the same place too.

What survives is the byte offset. Every failure lands just short of 2^30, which is
where an OpenSSH server re-keys the session when the negotiated cipher has a block
size under sixteen bytes -- `chacha20-poly1305` here. `libssh`, which this
distribution's curl is built against, says nothing at all when it happens: the trace
ends mid-transfer and resumes with the stall guard firing. OpenSSH's own client
re-keys without trouble, which is why `scp` is untroubled, and why this belongs to
the pairing of the two rather than to either end.

**The answer:** a read larger than 256 MiB arrives a span at a time, by byte range,
each span over a connection of its own, appended to the same local copy. No
connection carries enough for the re-key to arrive. A file that fits in one span is
fetched exactly as before, over a pooled connection -- an SSH handshake costs 0.58 s
here, measured, which is ruinous across ten thousand small files. Telling those two
cases apart is what `expectedSize` on `openRead()` is for: `TransferTask` and
`SyncTask` pass what their plan already measured, everything else says nothing and
gets the careful path. See
[ADR-0013](docs/adr/0013-a-large-sftp-read-arrives-in-spans.md). The 1.2 GB file that
could not be read at all now arrives in five spans in 65 seconds.

**Two ways the same fault was being hidden, both fixed:**

- **A short transfer was handed over as a whole file.** `net::errorFor` now compares
  what the server announced against what arrived, for every protocol, and fails the
  read with both numbers in the message. It fires only when the server itself stated
  a length and the request asked for a body, so a `HEAD` is unaffected.
- **A source that stopped responding looked like the end of the file.** The copy loop
  read until a read returned nothing, which `QIODevice` also does when a read fails,
  and then reported success. It now checks whether the device is actually at its end.

**The log**, since none of this could be seen from outside: four categories by
subject -- `mole.task`, `mole.drive`, `mole.net`, `mole.curl` -- turned on with
`MOLE_LOG=net,curl` or `MOLE_LOG=all`, writing into the session log that already
exists. Two of them are written in one place each and cover everything by
construction: `Task::execute` for every job, and a `LoggingFileSystem` wrapper that
every mount goes behind for every drive. Silent at debug and audible at warning, so a
short download or a failed job leaves a line whether anyone asked for logging or not.
Credentials in header lines are redacted, because a log gets sent to other people.
See [ADR-0012](docs/adr/0012-a-log-you-can-turn-up.md).

**Then the staging, which was the other half of it.** Every network backend read a
file by downloading all of it into a temporary file first, and wrote one by
collecting all of it before sending any. For the files this was about -- 94 GB, 20
GB, several at 4 GB -- that is not slow, it is impossible: a copy to a drive with
room to spare needed 94 GB of local scratch space on a machine with 84 GB free, and
twice that between two remote drives. So SFTP now streams in both directions, in the
same spans, with a bounded buffer between the transfer and the caller: about 8 MiB
per direction whatever the file weighs. Measured on a 1.2 GB read: not one byte of
temporary space. A 1.5 GB upload through the new write stream came back byte for
byte. Progress also moves from the first second now, because bytes go through the
copy rather than into a temporary file first, and a preview of a huge remote file
reads its first page instead of fetching the lot. Files up to 64 MiB are still
fetched whole, where random access is free and the preview layer wants it. See
[ADR-0014](docs/adr/0014-remote-files-stream-rather-than-stage.md).

**Then S3 and WebDAV, which had been left staging on the grounds that their
protocols require it.** Both reasons are real and neither survives a file larger than
the local disk. S3 now sends an object it cannot measure as a multipart upload --
begin, 64 MiB parts, complete -- staging one part at a time so each can be signed,
and dropping back to a single PUT when the payload turns out to fit in the first
part. That also lifts the 5 GB ceiling a single PUT imposed: the new one is 640 GB.
A failed upload is abandoned rather than left to be charged for, and completion is
read out of the response body, because S3 answers that request with 200 and puts the
failure inside the document. WebDAV streams a large write with a chunked transfer
encoding and keeps the staged PUT for everything else, so the servers that refuse
chunked are only met in the case that has no alternative. Verified against Backblaze
B2: a 150 MB object goes up in parts and comes back byte for byte, with the
conformance suite still green for everything smaller. See
[ADR-0015](docs/adr/0015-s3-uploads-in-parts-webdav-in-chunks.md).

**And then the question of who says a copy worked.** Everything up to here was our
own account of what we sent: bytes handed to a backend, no complaint, stream closed.
So now every copied file is weighed where it landed -- one listing per destination
directory rather than a stat per file, because on SFTP a stat *is* a listing of the
parent -- and a file whose size there is not the number of bytes that went into it
fails the copy. It runs before a move deletes anything, so a move to a destination
that lost bytes no longer destroys the only good copy. A check that cannot be run is
logged rather than treated as a failure. See
[ADR-0016](docs/adr/0016-a-copy-is-weighed-at-the-destination.md).

**Two more faults found by running the suite against the real server**, neither
related to the above:

- **Listing a file reported "not found" on this server rather than "not a
  directory".** Asked to list a path with a trailing slash, some servers answer with
  a "." row describing the file and others say it does not exist -- which is true of
  the directory asked for and false of the file that is there. `list()` treated "not
  found" as unambiguous and returned it; it now falls through to the same check that
  handles the other answer. The live conformance suite passes against this server for
  the first time.
- **A forward seek on a stream raced the transfer filling it.** Whether a short hop
  forwards was answered from the buffer or started a new connection depended on how
  much had arrived, which made the behaviour a matter of network timing. A hop within
  the buffer's length now waits for those bytes and skips them; further than that
  still starts again.

**What is not fixed** and is now in TODO.md: an upload interrupted by the process
being killed leaves a partial file that looks finished -- writing to a temporary name
and renaming on success would close that; the WebDAV write path has still never met a
real server; and FTP still stages, which nobody is blocked on.

---

## Copying a location to the clipboard

**Asked for:** copy the path of the open folder, of the selected file, or of the
drive root.

**What it turned out to be:** three separate menu entries rather than one that
guesses. They copy three different things, and a single action that sometimes took
the file and sometimes the folder would be a coin toss whose result is only visible
after it has been pasted somewhere. The file entry is disabled when a folder is
under the cursor instead of quietly copying that folder.

What gets copied is the **native path** for anything on local disk and the **uri**
for anything else. A remote drive has no native path, and handing out the path part
alone would produce something that looks local and is not: `/reports/2026` pasted
into a terminal means a directory that does not exist rather than a folder in a
bucket.

`Ctrl+Shift+F` for the file, not the `Ctrl+Alt+C` some file managers use: Ctrl+Alt
is AltGr on Polish and many other layouts, where AltGr+C is a letter people type.

The methods return the text they copied, so the tests assert what would be copied
without needing a clipboard, and the ones that do have a QGuiApplication check the
real clipboard as well.

**Two faults found on the way, both of which made the feature look broken:**

- **The menu can advertise a key that does nothing.** A `MenuAction`'s `shortcut`
  is only what the menu prints beside the title; the binding is a separate `Shortcut`
  in Main.qml. Naming the key without declaring it would have shipped a menu entry
  that lied. The walkthrough test presses the keys rather than trusting the field.
- **A notification stopped every shortcut in the application.** The toast was a
  `Popup`, whose default close policy includes Escape -- so it wanted key events, and
  while it had them no window shortcut fired for the five seconds it was up. It
  looked exactly like the second copy key being unbound, and it was not: the first
  press always worked and every press after it did nothing, whichever key it was.
  The toast now closes on a click outside and on its timer, and asks for no keys.
  Pressing a shortcut twice in a row is what catches this, so the test does.

---

## Sidebar rows stopped being cramped and stopped twitching

**Asked for:** drive and bookmark rows are low for something that behaves like a
button, and the label shifts when the pointer crosses the row -- a jumping label
that looks bad.

**What it turned out to be:** two separate faults with one appearance.

The name label fills whatever space its siblings leave, and both siblings were
bound to hover: the "free" caption was hidden on hover and the × was shown on
hover. So crossing a row changed the label's width twice, and it re-elided in
place. The × now keeps its place at all times and only fades in and out, and the
caption stays put -- so nothing beside the name moves and the name has nothing to
move for. The test hovers a real row with a real mouse event and asserts the
label's x and width are unchanged.

The heights were worse than "low". A plain row was 30 pixels around a button that
is 28 on its own, and a drive row with a capacity bar was fixed at 46 while the
content inside it wanted 54 -- it was being squeezed, which is its own share of why
the rows looked wrong. Heights now come from the content with a floor of
`minimumTarget + 8`: 36 for a bookmark, 57 for a drive with a capacity bar. The
row that holds the name is pinned to the target size so it measures the same with
or without a ×, because the drives list is mixed -- a local disk cannot be ejected
and an archive can -- and two row heights in one list read as a bug.

Found while writing the test: `hovered` never became true under it, because
`Control.hoverEnabled` follows a platform style hint. The highlight, the tooltip
and the × all quietly depended on how the platform felt about hover effects, so it
is now stated outright.

---

## A drive is checked where it is configured

**Asked for:** verify the configuration when it is saved. The complaint was
precise — with a wrong parameter the road to finding out was far too long — and it
came from hitting it: an S3 drive against Backblaze B2 failed with *"SSL: no
alternative certificate subject name matches target host name"*, several steps
away from the form that caused it.

**What it turned out to be:** nothing in the application ever asked the far end
anything. `connectDrive()` only built the backend, and building one performs no
I/O, so a drive that could not work looked exactly like one that could until
something tried to read from it. Saving now runs a `DriveCheckTask` that lists the
drive's root — the cheapest request that proves name resolution, TLS, credentials
and the path all at once — and reports either what it found or why it could not.
The answer arrives as a notification and as a band across the drives dialog, and
every configured drive has a button to ask again.

A Task rather than a call, because `IFileSystem` is synchronous and worker-thread
only: a blocking check against a host that is not answering would freeze the
window for the whole timeout. The drive is saved either way, so a failed check
never costs what was typed.

Two details worth keeping:

- **The verdict is a band across the dialog, not a line in the form.** Saving
  clears the form, so a line inside it vanished at the exact moment it became
  worth reading.
- **It reports a count, not "connected".** An empty answer from the wrong place
  looks identical to an empty answer from the right one, and the number is what
  lets someone tell them apart.

Found while writing the walkthrough test for it: the band appeared, carried the
right text, and was zero pixels wide, because it was measured on the frame it
became visible rather than after the layout had run.

**Also fixed, from the same report:** a bucket whose name contains a dot cannot be
addressed through the host name at all — a wildcard certificate covers exactly one
name part, so `my.backups.s3.…` fails TLS however it is configured. Such a bucket
now always goes in the path, and an endpoint that already carries the bucket is not
given it a second time. Both were verified against B2's real certificate rather
than reasoned about.

---

## rclone out, four network backends in

**Asked for:** drop rclone — too much ballast for what it gave, and its
configuration had become far too complicated — and put SSHFS, SFTP, FTP, S3 and
WebDAV in its place as a plugin, with room for more backend plugins later. The S3
one had to work against AWS, Backblaze B2 and anything else S3-compatible.

**What it turned out to be:** four backends, not five. SSHFS was dropped on the
author's decision once it was pointed out that it is SFTP over FUSE: the files
should be reachable from inside Mole and not mounted into the operating system,
and FUSE would not port to Windows anyway. So a drive stays virtual and
in-application, and SFTP covers talking to an SSH server.
[ADR-0011](docs/adr/0011-network-drives-without-rclone.md) records the whole
decision, including what is genuinely lost: Google Drive, Dropbox, OneDrive and
Mega speak proprietary APIs that none of these protocols reach.

One dependency serves all four: **libcurl**, which speaks sftp, ftp, ftps and
https. The deciding argument was not its feature list but the threading contract —
`IFileSystem` is synchronous and worker-thread-only, and libcurl's easy interface
is blocking, where `QNetworkAccessManager` would have needed an event loop pumped
on every worker. aws-sdk-cpp was rejected for repeating rclone's mistake at a
different scale; what was wanted from it was SigV4, and that is a page of
HMAC-SHA256 over the OpenSSL already required for the credential store.

Verified against real servers rather than only in principle: SFTP, FTPS and S3 all
pass the full conformance suite live — SFTP and FTPS against an Egnyte account, S3
against Backblaze B2 — with the fixtures seeded through plain libcurl rather than
through the backend under test, so a bug could not cancel itself out. The live
tests are driven by `MOLE_TEST_*` variables and skip when there are none, so the
suite is green on a machine with no account and no credential is committed. WebDAV
has no live server to hand and is covered by parser tests against Nextcloud- and
Apache-shaped answers, with the conformance run waiting on
`MOLE_TEST_WEBDAV_URL`.

Five things went wrong on the way, and each one is now a test:

- **A file listed as a directory read as an empty folder.** Asked to list a regular
  file, an SFTP server does not refuse — it answers with a `.` entry describing the
  file. The parser was dropping dot entries, so browsing into a file showed an
  empty directory instead of an error.
- **The signer did not sign the headers it produced.** `x-amz-date` and
  `x-amz-content-sha256` were sent but left out of the signed set, which worked in
  a test harness that passed them by hand and was refused by B2 with "header must
  be included in signature". The signer adds them itself now.
- **Signing took a pre-encoded, pre-sorted query.** The first thing to get that
  wrong was this project's own cross-check harness, which is a fair verdict on the
  interface. The signer now owns the encoding, and the url that is sent is built
  from the same encoder.
- **FTP's success code read as a failure.** A finished listing ends with 226
  "Transfer complete"; interpreted as an HTTP status that is an error. Protocols
  that report through the transfer result now say so explicitly.
- **Every WebDAV timestamp came back empty.** `getlastmodified` is an HTTP date
  ending in `GMT`, which Qt's RFC 2822 reader rejects, and it starts with a weekday,
  which Qt validates — so a server whose weekday arithmetic is a day out would have
  its timestamps thrown away. The weekday is now dropped before parsing.

Two problems found in existing code while wiring the backends up, both invisible
until a backend buffered its writes:

- **A failed upload looked like a successful copy.** `TransferTask` and `SyncTask`
  called `close()` on the write stream and ignored the outcome, and every remote
  backend commits in `close()`. `closeAndReport()` now collects it through a small
  `ICommitsOnClose` interface, and a failed send is reported as a failed transfer.
- **`CompressTask` never closed its stream at all**, resetting the device instead,
  so packing an archive onto a remote drive would have written nothing while
  reporting success.

Also hardened on the way past: `saveDrive` used to write any value it was handed
into the readable settings file, including keys no backend had declared. An
undeclared key is now dropped, so nothing can smuggle a secret into a file that is
meant to be read — no field would have marked it secret.

---

## User documentation

A guide in [docs/guide/](docs/guide/README.md): an index, then browsing, looking inside
files, finding things, the operations, and the command palette. Written for someone
using Mole rather than someone building it, which is why it is separate from
ARCHITECTURE.md and from the ADRs.

The part worth having is the rule about its pictures. Every screenshot comes from
`make screenshots`, which drives the real application headlessly and photographs each
state the walkthrough test has *just asserted* — so a picture cannot outlive the feature
it documents. Twenty-eight of them exist because twenty-eight states are checked;
`make guide-images` regenerates them and copies them in, so refreshing the guide after a
change is one command rather than an afternoon with a screenshot tool.

A script checks that every image and every internal link in the guide resolves, because
a guide with a broken picture is worse than one with no pictures: it looks maintained.

README.md gained a *Using it* section pointing at the guide, and its list of things not
yet done lost PDF, SQLite and Parquet — all three are built, and leaving them on a wish
list would have been the same kind of lie the screenshot rule exists to prevent.

## The palette had never actually offered the drives

Reported: searching commands should find the available drives too, because they sit in
the list on the left and reaching one means clicking rather than typing.

They were supposed to be there already — the palette is built as a view over the action
registry, the bookmarks *and* the mount list, precisely so it cannot drift out of step
with them. The test said otherwise at once: five drives in the sidebar, none in the
palette.

The cause was ten lines of ordering. The palette was constructed with a pointer to the
mount list before that list existed, so it held null for the lifetime of the process and
offered no drives at all. Everything else about the palette worked, which is why this
survived: the failure is silent, and an empty category looks exactly like a category
nobody has anything in.

The test is the useful part, and it does not check that *a* drive is offered — it checks
that every drive in the sidebar is, because the palette holds no list of its own and
anything missing is something the mouse can reach and the keyboard cannot. It then types
a drive name and follows the choice through to the navigation it asks for. Putting the
old order back makes it fail.

## Telling the two buttons in a dialog apart

Reported: on the popups it is not clear which button is the active one, they look the
same, and it is easy to make a mistake.

Measured, it was worse than that. The two buttons were flat labels of identical size,
weight and colour, **and neither of them held the keyboard** — so there was no active
one to see. Over the delete dialog they read *Yes* and *No*, which says nothing about
which one deletes.

Now: the button that acts is filled and the one that backs out is outlined; the fill is
red when the thing cannot be undone; both are labelled with the verb — *Delete*,
*Compress*, *Rename*, *Create* — and a destructive dialog opens with the keyboard on
the safe way out, outlined so it can be seen. A stray Return closes the question rather
than answering it. Dialogs that are typed into keep the keyboard in the field.

The instructive part was the Material style. Asking it for a highlighted button gives
an item that reports itself visible, correctly sized, and filled with the colour asked
for, and paints nothing. Every property said red. What settled it was counting red
pixels in the screenshot: zero. The backgrounds are drawn by hand now, and the test
reads the colours off the items rather than the properties that had lied.

Two process notes worth keeping. My direct runs of the test binary do not write
screenshots — only the `screenshots` target does — so for a while I was looking at a
stale image and drawing conclusions from it. And the focus took three attempts because
a `DialogButtonBox` is three focus scopes deep: the popup, the box, and the ListView
the box lays its buttons out with. Miss the middle one and the focus is set, reported
as set, and never active.

See [ADR-0010](docs/adr/0010-telling-the-two-buttons-apart.md).

## Compressing can take the originals with it

Asked for as a checkbox: pack this, and leave me the archive without the files.

One operation to a person, two to a file manager -- and the middle step, finding the
same selection again to delete it, is where the wrong thing gets deleted. So the
feature is easy and the rules around it are the work, because this is the only thing
here that deletes data as a side effect of something else.

The archive is the only copy once the originals go, so nothing goes until the archive
is provably a complete copy of them:

- **After the archive is written and closed**, never as part of writing it. There is
  no moment with the files gone and no archive.
- **Nothing is deleted if anything could not be read.** One unreadable file inside a
  packed folder means the archive is missing it, and the original is the only place it
  exists. Both are kept and the status says which case it was.
- **Nothing is deleted if the job was cancelled**, even when the archive survived.
- **A source containing the archive is never deleted.** Packing the folder you are
  standing in writes the archive inside it, so deleting that source would take the
  archive with it -- turning "keep the archive, drop the files" into keeping nothing.
- **A deletion that fails is reported, not fatal.** The archive is written and correct,
  which is the part that cannot be repeated.

The box is off every time the dialog opens. Archiving something once and dropping the
originals is not a standing instruction.

Three tests cover the three refusals, and the guards were checked by removing them:
without the first, the unreadable-file case deletes a folder the archive does not fully
contain; without the second, the archive deletes itself along with its folder. Both
failures are real data loss in the temporary tree, which is what makes them worth
having tests for.

Moving to a trash instead was considered and left alone: it belongs to all deletion in
the application rather than to compression, and a trash that some drives have and
others do not is worse than none.

See [ADR-0009](docs/adr/0009-packing-can-delete-the-originals.md).

## Dialogs that destroy something say what they are aimed at

Reported plainly: the delete popup does not show which files it is about to delete,
"and that could be a problem in a moment".

It could. Four dialogs stood in front of something irreversible and all four asked
with a number — *"Permanently delete 2 items?"*, *"7 files, 2.4 GB"*, *"12 files at
the destination will be removed"*. A number only confirms what somebody already
believes. It cannot catch the case the dialog exists for: that the operation is aimed
at something other than what they think.

That case is not hypothetical here. Ticks survive navigating away and coming back, the
cursor is a target when nothing is ticked, and a selection made in one folder is easy
to still be carrying in another. Each of those is a route to a correct-looking count
over the wrong files.

So every one of them now lists the entries by name, through one shared component
rather than four hand-rolled copies. Compressing had grown its own list first; a
second copy would have been the point where the two started saying the same thing
differently. Deleting duplicates lists full locations instead of names, because inside
a duplicate group every name is identical and the location is the only thing that
tells them apart. Syncing lists only the deletions, since a plan can be thousands of
steps and what is being agreed to here is the part that destroys something.

Two details that are the whole point. The list comes from the same call that performs
the operation — `FileListModel::targetEntries()`, which `targets()` is now built from
— because two functions answering "what is selected" is exactly how a list and an
action drift apart. And it is taken when the dialog opens rather than bound live: a
refresh landing or a watcher firing behind an open dialog must not change what
pressing Yes means. That second one is tested by mutation — binding it live instead
makes the test fail.

One correction on the way through: I read a scaled-down screenshot as a clipped
warning line and "fixed" the dialog's height for it. Measuring said the content was
114 pixels in a 209-pixel space and had never been clipped; the change was reverted
rather than left in as a fix for nothing.

See [ADR-0008](docs/adr/0008-naming-what-an-operation-touches.md).

## Compressing: 7z, bare xz, and a name that was being thrown away

Two formats added and one bug fixed, and the bug was mine.

**The name was being discarded.** Reported as: a name typed into the box, and the
archive written under the original one. The cause was the *Kind* picker — changing the
format rebuilt the whole name from the selection, silently replacing whatever had been
typed, while the comment above it claimed it "renames the suffix". It does that now:
the base is kept, including any dots in it, and only the suffix is swapped. The test
covers the multi-part suffixes that make this easy to get wrong (`holiday.tar.gz` →
`holiday.7z`, not `holiday.tar.7z`), case, dots in the middle, and an empty box giving
nothing rather than a file called `.zip`.

**7z is now offered**, which reverses the first version of ADR-0007. That exclusion
rested on libarchive writing less of the format than it reads; measured on 3.7.2, a
multi-entry 7z writes and reads back correctly, so the caveat did not apply to what is
actually being done here.

It cannot be encrypted, though, and that came with a trap worth recording. This
libarchive rejects `7zip:encryption` as an undefined option — yet it accepts a
passphrase anyway, returns success, and the written file contains no plain text,
because LZMA2 compressed it rather than anything encrypting it. A test looking only for
the plain bytes would have called that encryption. So a password is refused for 7z, and
whether a format can carry one is a stated fact rather than an inference from the
output.

**A bare `.xz`** is one compressed stream with no container: one file, no folders.
Asked for with more, it refuses before writing anything, rather than failing on the
second entry with *"Raw format only supports one entry per archive"*.

Its test took three attempts, each corrected by measurement rather than by guessing.
Asserting an entry list found none — correctly, since a bare xz keeps no names.
Asserting the plain text was absent failed, because eleven bytes are not compressible
and LZMA2 stores an input that small uncompressed inside a perfectly valid stream.
Asserting a tenfold reduction failed too: libarchive pads its output to a
ten-kilobyte block, so 56 kB of repetitive text comes out at 10 kB of which most is
padding. Checked with `xz -t` and `xz -dc` on the way through — the stream is valid and
returns every original byte — and the bound in the test is deliberately loose so nobody
tightens it into a flake.

## Compressing: what it packs, and a password

Two changes, and the first was not the one it looked like.

The report was that a selection should be compressed rather than the folder. Measuring
first: a *ticked* file already was — `currentTargets()` returned it and the suggested
name followed it. What was empty was the case where a file is only **under the cursor**,
unticked, which fell back to the whole folder. Compressing was the one operation here
that ignored the cursor: copying with `F5`, deleting with `F8` and analysing all use the
commander rule of "ticked entries, or the row under the cursor". It now does too, and
the folder in view applies only when there is no row at all.

Since a rule about what gets packed should not have to be inferred, the dialog now says
what it is aimed at — *notes.txt*, *2 selected items*, *the folder documents* — and
**lists the entries by name**, with folders marked as folders. A count is a summary; the
point of a dialog before an operation is being able to see it is pointed at the right
things. The list scrolls and stops growing at a sensible height, so ticking forty files
does not produce a dialog taller than the window.

A password protects a zip with AES-256, not zip's original scheme, which is broken and
known to be. It is offered only where the format can carry one: a tar has no notion of a
password and gzip and xz encrypt nothing, so the box is disabled with a line saying why.
A passphrase handed to the task for such a format is **refused**, not ignored — someone
who typed a password and received an archive anybody can open has been quietly lied to,
which is worse than an error.

The test for it checks the thing that matters rather than the thing that is easy: that
the plain text is not sitting in the written file, and that the archive reader cannot
hand the contents back without the password. An implementation that accepted a password
and wrote everything in the clear would pass any assertion short of that.

## Compressing files and folders

An operation on the selection — or on the folder in view when nothing is ticked —
that asks the two things worth asking, a name and a kind, and packs in the background
like every other job that takes time. The archive lands beside what was packed, which
is where anyone would look for it.

Three formats: zip, tar.gz, tar.xz, with zip the default because it is the one anyone
can open anywhere without being told how. 7z is deliberately absent — libarchive
writes less of that format than it reads, so offering it would mean sometimes
producing an archive something else refuses. Reading 7z is unchanged.
[ADR-0007](docs/adr/0007-writing-archives.md) has that and the rest.

Both ends go through `IFileSystem`, so packing a selection on a remote drive is the
same code as packing one on local disk, and archive mounts stay read-only: this writes
a new archive and nothing else. Adding to an existing one sounds like a small extension
and is not — it means rewriting the container and having something sensible to do when
the process dies half way — so it is not offered.

A cancelled or failed compression deletes the partial file. An archive that exists is
one that finished; a half-written file waiting to be mistaken for a good one is the
worst outcome available here. One unreadable file is recorded and skipped rather than
fatal, the way the walker treats a directory it cannot enter — the rest of the archive
is still worth having, and what was missed is said out loud.

Where the code lives took a correction. The writer went into
`mole_archive_backend`, which already links libarchive, and the define was put on
`mole_builtin` — but `AppController` lives in `mole_ui`, *below* the built-ins, so
`canCompress()` compiled to `false` and the walkthrough test skipped itself while
looking like it had run. The link belongs where the operation is; `mole_ui` sits on
`mole_core` exactly as the backend does, so nothing is inverted.

The tests pack a tree and read it back through the archive *reading* backend, in all
three formats, so the writer and the reader hold each other to account rather than the
writer being checked against its own idea of what it wrote. The cancellation test
needed a second version: waiting to observe `Running` and then cancelling is a race the
task can win, and under a loaded machine it did once — it now packs from a drive that
is slow to open files and cancels on the first sign of progress, which lands mid-write
every time.

And then a test failed about one run in three, which turned out to be the most useful
thing that happened here. It was not flakiness: the entry header was written *before*
the file was opened, so a file that then could not be read left a header promising N
bytes with nothing behind it — corrupting everything after it in the stream. It only
showed when the walker happened to reach the unreadable file before a good one, which
is exactly why it looked intermittent. The file is opened first now, and the test names
the unreadable file ahead of the readable one instead of leaving the order to the
walker, so the case that found the bug is checked every time rather than a third of the
time.

## Search results were a list you could only look at

Three things they could not do, and one of them was in the wrong place.

Building a set from them existed, beside the criteria — which is not where the rows
are. It has moved to a strip above the results, along with the two other things worth
doing to a match: showing it in its folder, and looking at it without leaving. The
index search shows the same strip without the set button, since it has nowhere to put
one yet.

Walking them was impossible: the list had no focus handling at all, so results could be
read and double-clicked and nothing else. Arrows move now, Enter goes there, F3 previews,
and Down out of the query box walks into the answers — once a search has answered, the
answers are where the keyboard should be. Arriving at a fresh list puts the cursor on the
first row rather than nowhere, which took a second attempt: results arrive after the view
exists, and a model that had no rows leaves `currentIndex` at -1.

"Show me where this is" is the natural end of most searches and had no way to be asked
for. `revealFile()` opens the folder holding a file *with the cursor on the file* —
arriving in the right folder with the cursor somewhere else is only half an answer. The
listing lands asynchronously, so the pane remembers what it was asked to reveal and
consumes it when the entries arrive; a file already in the current folder needs no
navigation and is selected straight away. Both paths are tested, because the second is
the one that quietly does nothing if forgotten.

## Previews had no options, and nowhere to remember one

An `.html` file previewed as coloured source. Sometimes that is what someone wants and
sometimes they want to read the page, which makes it a setting — and there was nowhere
to put a setting: `SessionStore` remembered which tabs were open and the window
geometry, and nothing else about anything.

Three answers, all in [ADR-0006](docs/adr/0006-preview-options-and-preferences.md).
`Preferences` is one small file of dotted keys that knows nothing about what they mean.
A provider *declares* its options — key, title, choices, default — and the strip above
the preview renders them without knowing what any of them are, the same way the menu
renders entries from plugins it has never heard of. And a choice is keyed by provider
*and* suffix, because "the next `.html`" is what was asked for and one text viewer
serves `.html`, `.xml` and `.svg` with different sensible answers; the provider id keeps
two viewers claiming a suffix from overwriting each other.

The choice applies immediately and is remembered, and it is applied *before* the file
is read, so opening the next `.html` shows the page straight away rather than showing
source and then correcting itself.

The rule that mattered most is the one about the network. Qt's rich text engine
resolves what a document names, so a page could quietly tell whoever wrote it that a
file had been looked at — in a file manager that is a nasty surprise, not a feature.
Anything a document could reach out with is removed before it is rendered: images,
scripts, stylesheets, frames, embedded objects, event handlers. Blunt rather than
clever, because telling a local reference from a remote one means parsing and resolving
and getting that subtly wrong is exactly the failure being prevented. The test feeds it
a deliberately hostile page and asserts that not one `http` survives while the words do.

Two mistakes worth recording. The first is mine and it leaked: adding a store without
teaching `PrivateProfile` about it meant the tests wrote into
`~/.local/share/Mole/mole-tests/preferences.json` — real user data, outside the sandbox
— which is why one test passed alone and failed in the suite, reading back what a
previous *run* had left. `MOLE_PREFERENCES_PATH` is in the profile's list now, the
leaked file is gone, and the suite was run twice to prove it. Adding a store means
teaching the test profile about it, or the tests are not isolated at all.

The second was the same delegate-recreation trap as bulk rename: republishing the option
list rebuilds the Repeater's delegate, so the test's pointer to the picker was dangling
and reading it hung the run. The test looks the item up again instead.

## A long search froze the interface

Reported as: a search that runs for a while eats so much CPU that the window stops
responding — and, tellingly, a folder analysis running alongside it took *longer*
and did nothing of the kind.

That comparison was the clue that mattered, because it ruled out the walk. The
analysis walks the same trees through the same `DirectoryWalker` and reports its
status just as often. What it never does is put anything into a model the interface
is watching.

`FileListModel::appendEntries()` reset the model and called `rebuildVisible()`,
which copies every entry found so far, filters it, and `stable_sort`s the lot. On
every batch. A search returning forty thousand results in batches of two hundred
therefore sorted a growing list two hundred times, on the thread that draws the
window. Measured before touching anything: **9,670 ms** of pure CPU for that case.

The batch is now filtered and sorted on its own and `std::inplace_merge`d into what
is already in order — both halves share the comparator, so the result is sorted
without looking at the earlier entries again. The same case now takes **246 ms**,
which is thirty-nine times less work in front of the person waiting.

Worth saying plainly: the time-based flushing added an hour earlier made this worse
in exactly the case reported. Before it, batches only went out every two hundred
matches; after it, also every hundred and twenty milliseconds — so a long search
produced more batches, and each batch cost a full re-sort. The latency fix was right
and the quadratic append underneath it was the bug; together they were the freeze.

The test states the case rather than the mechanism — forty thousand results in two
hundred batches, and a ceiling generous enough for a debug build on any machine —
and it fails with the number in the message, which is how the 9,670 ms above was
measured in the first place.

One thing deliberately left: a reset still discards the view's scroll position and
selection, so scrolling through results while they arrive is unsatisfying. Fixing that
means proper insert semantics rather than a reset, which is a bigger change than the
freeze warranted; it is recorded in TODO.md.

## Search results arrived late, and led nowhere

Two halves, and the first turned out to be one number.

`LiveSearchTask` batched matches at two hundred before emitting them — and only at
two hundred. A search over a large tree that matched a dozen files therefore showed
nothing at all until the whole walk had finished, which is exactly the case anyone
searching a disk meets. Batches now go out on whichever comes first, enough matches or
a hundred and twenty milliseconds, so the first answers arrive almost immediately and a
flood still costs one signal per two hundred rather than one per file.

The test for that was the interesting part. The suite already had
`streamsResultsWhileRunning`, which only checked the totals once everything had
finished — it proved nothing about arriving early, and it passed before and after. The
first replacement was no better: asserting a batch arrived while `isFinished()` was
false passes even with count-only batching, because the last flush happens inside
`run()` before the task is marked finished. Only a clock can answer *when*, so the test
now times the first batch against the whole walk and fails with a sentence that says
what went wrong: *first matches arrived at 1511 ms of a 1511 ms walk*.

The second half is what results are for. They can be narrowed where they are —
straight onto the model that already holds them, so no walk and no query, just less of
what is there — with a count that reads "3 of 41" when a filter is on. And they can
become a file set, which is where the work carries on: a snapshot of what is on screen,
narrowing included, because the rows in front of someone are what "these results"
means. A set that re-ran the query later would be a different promise from the one the
button makes. Nothing to build from produces no set rather than an empty one, and an
unnamed set is named after the query rather than after nothing.

## Ctrl+F was not usable as a search box

Three things, and only the last is a feature.

The keyboard was not in the field, so the first thing anyone did after pressing
`Ctrl+F` was reach for the mouse to click into it — which is exactly what the key is
supposed to save. It is focused now, on creation and whenever the shell asks the tab
for its pane.

Enter already started a search; nothing said one was *running*. A tree walk over a
large disk takes long enough that silence reads as nothing having happened, so the
results area now says it is searching while there is nothing to show, and steps aside
the moment rows arrive — the same threshold-free rule the table preview uses, because
here the walk streams matches from the start.

Then the criteria. Size, typed the way people write it: `10M`, `1.5 GiB`, `500k`,
`1,5M` with a comma, because that is a decimal point in most of Europe and this
application already shows sizes that way. Nothing and nonsense both mean "no limit"
rather than zero — a limit of zero bytes would quietly match nothing. It lives in a
*More* section that is folded away, so the common case stays one field and one key.

The interesting part was the index, and it needed a decision rather than code:
[ADR-0005](docs/adr/0005-which-engine-answers-a-search.md). The form now asks the
index when an indexed volume's root is a prefix of the folder being searched, and
walks otherwise. Partial coverage counts as none — the temptation was to ask the index
for the part it covers and walk the rest, which would produce one list where some rows
are current and some are as old as the last scan, with nothing to say which. The
toggle is on by default because the index is enormously faster and usually right, and
what makes that default safe is that the status line always names the engine that
answered and how old the index is. Turning it off is the case that matters: the truth
on disk right now, whatever the index remembers.

Both engines already had `minSize`/`maxSize`, which is why size was the criterion
added first — anything the index cannot express would have to fall back to walking and
say so.

Tested at both levels: the size parser on its own including the cases that must mean
"no limit", the engine choice as behaviour (unindexed walks, indexed answers from the
index, the toggle forces a walk on a file written after the scan), and the box itself
in the real window — the field holds the keyboard on opening, five typed characters
reach the controller, and a 500M floor empties a fixture that has nothing that big.

## A dist/ from before the rename failed the licence check

`make licence-check` had been failing on *bundled Qt cannot be replaced by the user*
since before any of the recent work, and it had nothing to do with licensing. A
`dist/` sat in the tree from a `make bundle` run made when the binary was still called
`superfilemanager`; the check looked for `dist/mole`, found nothing, and reported the
bundle as non-replaceable. Everything that actually mattered passed throughout — Qt
dynamically linked, no Qt symbols in the binary, no GPL-only module.

Seventy-six megabytes of git-ignored build output holding the old binary and copied
system libraries, with nothing hand-made in it. `make bundle` already begins with
`rm -rf dist`, so it was a stale artefact rather than a bug in the target, and deleting
it was the fix. The tree is left without a bundle, which is how a fresh checkout looks:
one is built on demand, and a bundle left lying about is precisely what caused this.

The change worth keeping is the message. Ten minutes went into working out what
"bundled Qt cannot be replaced" meant, so the check now names the launcher it wants —
*there is no launcher at dist/mole (stale bundle? run: make bundle)* — and separates
the three ways replaceability can fail instead of reporting one verdict for all of
them. A fresh `make bundle` was run end to end to confirm the check still passes when
there is something real to check, including the replaceability test that had never
actually run.

## The header says the palette is there

A shortcut nobody has been told about is a feature nobody has. So the title bar now
carries something that looks like the box it opens — a search glyph, the words *Search
commands*, and `Ctrl+R` drawn as a key — in the middle of the window, on the same line
as the hamburger and the name.

It opens the palette rather than trying to be one. Two boxes that both filter would
mean two places owning the same state, and the reason the palette works is that one
place owns the list.

Centred in the *window*, which took a second attempt: laid out in the toolbar's row
between the menu on one side and the task indicator on the other, it sat noticeably
right of centre, because equal spacers centre a thing between its neighbours and not
in the window. It is anchored to the toolbar instead. The test holds all three claims
that make it work as a teaching aid — visible, within two pixels of the window's
middle, on the same line as the menu button — and that clicking it opens the palette.

## The palette moved to Ctrl+R, and stopped remembering the last query

Three small things, and one of them was only found by trying to break the test.

The box kept whatever was typed into it last: `onAboutToShow` reset the model's
filter but not the field, so the next opening showed a list narrowed by a query the
user could no longer see a reason for. It clears the field now.

`Ctrl+R` belonged to Refresh, which was the wrong use of a key that good — refreshing
is one row in the palette like everything else, and it keeps its View menu entry. Its
`shortcut` label went with the binding, because a menu that advertises a key that no
longer works is worse than one that advertises nothing.

And the palette lost its animations. That started as a test problem — pressing the key
again straight after Escape did nothing, because `opened` goes false when the exit
transition *starts* and `open()` during that transition is silently ignored — but it is
a real one: a human closing and reopening quickly would hit exactly the same wall. A
box you summon to type one word into should be there the instant you ask.

The test was worth more than the fixes. Removing `field.clear()` and running it again
showed it still passing, which meant the assertions I thought I had written were not in
the file at all — the edit had not matched. Written properly, and checked the same way,
it now fails with `"termi"` still sitting in the box. It also has to check the clearing
*before* running a command, because the command it runs is the terminal, and a terminal
that holds the keyboard stops `Ctrl+R` reaching the window — which is ADR-0002 working
exactly as intended, in a place I had not expected to meet it.

## One input that can reach everything

`Ctrl+Shift+P` opens a box with a list underneath of everything that can be done
right now — the whole `F4` menu tree, every bookmark, every drive. Typing `termi`
leaves one row, *Operations → Terminal here*, and Enter runs it. Arrows move, Escape
leaves, and nothing about it needs the mouse, which matters because the reason it
exists is that not every control has a shortcut of its own.

The design decision that makes it worth trusting is that it holds no list. The menu
entries come from `ActionRegistry::buildModel()`, the places from `BookmarkModel` and
`MountListModel`; the palette is a view over those three. A second list maintained by
hand would drift out of step with the menu the first time somebody added an action,
and then the one thing the palette promises — that it has everything — would quietly
stop being true. The test that says so is the first one in the file: the palette's
paths must equal what the menu would show, entry for entry.

"Only what is available" came for free rather than needing a mechanism: the menu
already evaluates each entry's `enabled` callback at the moment it is asked, so a
greyed-out action is simply absent. It is rebuilt on every open, because what can be
done depends on the tab in front of the user.

Ranking is less optional than it looks. A title match beats a match on the group, or
typing `set` buries *Add to set* under everything in a section whose name contains
those letters; and several words match anywhere in the path in any order, so both
`op term` and `term op` find the terminal. Each of those is a test, because each is
a way for the box to feel broken while technically working.

The model asks and the shell acts — `actionRequested` and `locationRequested` rather
than a call into tabs or navigation — which is what lets it stay a plain view. The
walkthrough proves the whole path in the real window: the key opens it, the input has
the keyboard immediately, five characters narrow everything to one row, and Enter
opens the terminal.

## PDFs had no preview

A PDF fell through to the information viewer — the last resort for a file we cannot
show — so previewing one gave its size and its type and nothing of its contents,
while the listing already drew it an icon and `IPreviewProvider.h` named PDF in its
own description of what previewing is for.

It opens as a column of pages now, rendered by `QPdfDocument`, read-only, with the
same `Ctrl+PgUp`/`PgDn` paging the text viewer uses. Pages are rendered when a
delegate asks for one, so opening a six-hundred-page scan costs the first page rather
than six hundred, and the delegate reserves its height from the page's own aspect
first so the list does not jump about as images arrive. The rendered width is
quantised in steps because it goes into the cached file's name — bound to the raw
width, dragging a window would have re-rendered every visible page per pixel.

Two decisions were made before any code, and both are in
[ADR-0004](docs/adr/0004-pdf-previews.md). Qt PDF rather than poppler, because
poppler is GPL and does not sit with shipping Mole under Apache-2.0, while the Qt
module as packaged declares `LGPL-3 or GPL-2` — which is what the licence audit turns
on. And pages reach the screen as image files in a scratch directory rather than
through a `QQuickImageProvider`, because an image provider is registered on the
`QQmlEngine`, which a preview provider deliberately cannot reach; threading the engine
through the plugin boundary to save a temporary file would have traded a real
architectural rule for a smaller one.

`QtQuick.Pdf` would have supplied most of this view for free and was not used: its
QML module is not installed here, so depending on it would mean a second optional
dependency for one feature and a view that silently does not exist without it.
Rendering through `QPdfDocument` costs a page-image path and buys control over when
pages are rendered, which is the part that matters.

The dependency is optional. Without `Qt6::Pdf` the provider still compiles and still
refuses every file, so a PDF behaves exactly as it did before — and the test states
that both ways round, so a build without the module is a green build rather than a
skipped one.

Licence work done rather than promised: `THIRD-PARTY-NOTICES.md` records Qt Pdf and
what it embeds — PDFium and PDFium's own third-party components, all inside
`libQt6Pdf.so` rather than in Mole's binary — and the audit table in
`docs/LICENSING.md` lists the module. `make licence-check` confirms Qt is still
dynamically linked, now across twelve libraries including `libQt6Pdf.so`, and that no
GPL-only module is referenced. It also fails one check, on a stale `dist/` from an old
bundle whose launcher is still named `superfilemanager` — that failure predates this
work and is noted in TODO.md rather than quietly worked around.

The tests write their own PDF with `QPdfWriter`, because a binary fixture in the tree
is one nobody can review. They check the page count, that an A4 page comes out
upright, that asking twice at one width reuses the file while a different width
renders again, that a page past the end is nothing rather than a crash — and that the
rendered page has ink on it, since a renderer quietly producing white paper would pass
every other assertion. The walkthrough then proves the whole path in the real window:
a delegate asks, an image loads, and the strip says "Page 1 of 2".

## Bulk rename hid the thing it calls its own feature

`BulkRenameView.qml` opens by stating its own priority — *"The preview is the
feature"* — and then laid itself out as though the form were. The rules column asked
for 40% of the window and there was no minimum width anywhere in the view, so the
grids of full-width text boxes inside it stretched a two-character prefix across a
third of the screen and the before-and-after list took what was left. The form is
now capped, and the preview keeps a floor of its own, so no arrangement of rules can
crowd it out.

The other half was that nothing happened while you typed: the fields were wired to
`onEditingFinished`, so a prefix showed no effect until Enter was pressed or the
focus moved elsewhere — while the dropdowns and spin boxes in the same form updated
at once. Two behaviours in one panel, and the fields carrying the interesting part
were the ones that felt dead.

The first attempt at this was wrong and worth recording. Measuring `RenamePlan::build`
first — 2 ms for a thousand files, 15 ms for five thousand, 63 ms for twenty thousand
— it looked like live updates needed coalescing, so a debounce went in. That was
solving a problem nobody had: the complaint was about not seeing the changes, and a
debounce delays exactly the feedback being asked for. It also made the preview
asynchronous, which broke a test that reasonably expected the plan to be current. It
came out again.

The real cause was elsewhere and would have survived any amount of debouncing. Every
keystroke made `setRuleField` emit `rulesChanged()`, the form's `Repeater` rebuilt its
delegates, and the field being typed into was destroyed and replaced. Typing "2024_"
left "2": the first character round-tripped through the model, the field was
recreated, and the rest went nowhere. `setRuleField` no longer announces that the
rules changed — the form is the only thing that reads them and it is where the change
came from; what has to follow the keystroke is the preview, and `previewChanged()`
says so.

The test types into the field rather than calling the controller, and keeps the
keyboard there while it asserts, because the whole bug lived in the difference
between those two things. It also holds the layout: the preview list must keep at
least 320 pixels.

## The small controls were too small to hit

Adding a bookmark and closing a tab — the two things anyone does most — were
`ToolButton`s of 22 by 22 with a text glyph inside, and the drive's remove button was
20 by 20. Fiddly to hit, and they read as afterthoughts.

It was never two files. Fifty-two explicit `implicitWidth` or `implicitHeight` values
sat below 24 across 18 QML files, so the fix was a decision rather than a nudge:
`App.minimumTarget` is now the floor for anything that is only an icon, at 28 —
twenty-four is the figure usually quoted as a minimum for a pointer, and on a desktop
something nearer thirty stops feeling like a pinprick. Twenty-four controls were
raised to it, and nineteen glyphs now take their size from the type scale, because a
bigger button with the style's default mark in the middle looks emptier rather than
clearer.

What was left alone, deliberately. The remaining small sizes all belong to
`BusyIndicator`s, which are not click targets — a spinner does not need to be
reachable. And several of these controls appear only on hover, the drive's × among
them; that is a discoverability question rather than a size one, a drive can also be
removed from the Drives dialog, and changing when a control appears is a different
decision from how big it is when it does.

The floor is testable and the look is not, so the test holds the floor: the two
controls the request named are at least `minimumTarget` in both directions and their
glyph reports exactly `textSize`. It is two rather than all of them because a
tree-wide assertion would need every icon-only control to carry an `objectName`
first — recorded in TODO.md rather than left implied.

## The type was too small, and there was no scale to raise

A file name was 13 pixels, most of a listing 12, supporting text 11, and in places
it dropped to 10 and 9. A file manager is a thing people stare at all day.

Raising the numbers where they stood was the wrong shape of fix: there were about
270 `font.pixelSize` literals across 27 QML files, so "a bit bigger" would have been
270 edits and the next view added would have guessed its own size again. The sizes
now come from `AppController`, for the same reason the monospace family already did
— picked once so that a listing, a preview and a form line up instead of each
choosing. Five steps, each with a job: `headingSize`, `textSize` for primary content,
`secondaryTextSize` for sizes and dates and labels, `smallTextSize` as captions *and*
the floor, and `monospaceSize` for code, which reads a shade smaller than prose.
`listRowHeight` is derived from `textSize` rather than stated, so raising the text
cannot crop a row.

Applied where the reading happens: the file listing, every preview, and the sidebar
— which was not in the original plan but ended up looking small next to a listing
that had grown, and it is the next place the eye goes. Around 200 literals remain in
the other views and adopt the scale as those views are touched; that is recorded in
TODO.md rather than left as a surprise.

Constant for now, and deliberately so: when these become a preference the views do
not change, which is the whole point of them living in one place.

Two tests, because "looks better" is not assertable but two things around it are.
The scale's shape is held at the application level — the steps in order, nothing
below the floor of eleven, code no larger than prose — and the binding is held in the
real window: a listing row's name label must report exactly `textSize`, because a
literal left behind in a delegate is invisible until someone compares two views side
by side. Whether the result is *pleasant* is still what `make screenshots` and a
human are for.

## How big is this folder, answered in the listing

`Ctrl+Shift+S` measures the ticked folders — or every folder in the listing when
nothing is ticked, because "which of these is the big one" is the question — and
writes each total into its row as the walk finishes it. A background `Task` like
everything else that takes time: progress, cancellable, visible in the task strip,
and the window stays usable throughout.

No second tree-walker: `FolderSizesTask` uses the same `DirectoryWalker` the
analysis and the indexer use, so cancellation, unreadable directories and symlink
loops stay solved in one place. What it does not reuse is `AnalyseDirectoryTask`
itself, which was the first plan — it produces a whole `AnalysisReport` per folder,
and forty folders would mean forty reports built and thrown away to read one number
off each.

A measured total lives beside the entry rather than in it. `FileEntry::size` for a
directory is the inode's own size, and writing a recursive total over it would make
one field that is sometimes one thing and sometimes another — a field nobody can
trust afterwards. Keyed by uri, so re-sorting or filtering cannot move a number onto
the wrong row, and dropped whenever the listing is replaced: a measurement describes
the tree as it was when it was taken, and a stale number is worse than an empty cell.
Sorting by size uses the measured total for folders that have one, which is the only
number anyone means when they sort a listing by size.

Two things the tests had to be dragged into being honest about. Cancellation cannot
be tested on a local disk: `folderSized` is queued to the test's thread, and by the
time the cancel is sent the worker has already finished the next folder, so both
answers arrive and the test proves nothing — it needed a drive that takes its time
listing, and then it asserts exactly one whole answer arrived. And "cancelled" must
never mean "reported half a folder as a total", because a wrong number in a listing
is worse than none, so the task checks for cancellation between finishing a folder
and announcing it.

Also covered: an empty folder answers zero rather than staying silent for ever, a
folder the walk cannot fully read reports what it managed, files are counted but the
directories in between are not, the ticked folders win over the whole listing when
there are any, and a refresh clears what was measured.

## F3 did nothing on a folder

`F3` previews the file under the cursor, `currentFile()` returns nothing for a
directory, so the action was disabled and the key did nothing at all — which is
indistinguishable from a key that is broken. On a folder it now opens it, the same
thing `Return` does, through the same `openRow()` the pane already uses.

Handled in the pane rather than in the action, deliberately. The menu entry says
"Preview this file" and stays disabled on a folder, because that is what it says it
does; it is the *key* that carries the second meaning, which is how function keys
have always worked in a commander.

The test asserts which of the two paths ran, since both are one keypress and easy to
confuse: on a folder the listing navigates and no tab appears, on a file a tab
appears and the listing stays put. It holds the pane pointer from the start, because
`pane()` asks the current tab for its pane and the current tab is a preview by the
end — the first version of the test dereferenced that null and took the whole binary
down with it.

## The menu had one heap called Tools

Eleven entries under one heading, of two entirely different kinds. *Preview this
file*, *Terminal here*, *Add to set* and *Index this folder* do something to the
files in front of you and hand you back to the listing. *Analyse folder*, *Find
duplicates*, *Bulk rename*, *Sync folders*, *Saved reports*, *Alerts* and
*Scheduled jobs* open a tab that is a tool you then work in. Read as one list they
are indistinguishable, so finding anything meant reading all of it.

`Tools` is now `Operations` and `Workflows`, and the deciding question is written
down: does the entry do something to the files in front of you, or hand you a tool
to work with? The tie-break for the ones that sound like both — if it needs a tab of
its own to be useful at all, it is a workflow. *Bulk rename* is a workflow even
though it acts on a selection, because what it opens is a tool with rules and a
preview. *Add to set* is an operation even though the sets view is a workflow,
because adding the selection to a set is one act, finished when it is done.

This is an extension point, not decoration: `Section` is what a plugin picks, so
leaving it as one bucket guaranteed plugins would keep filling the same bucket. The
names, the rule and the alternatives that lost are in
[ADR-0003](docs/adr/0003-menu-sections.md) — including why not `Tasks` (the
application already shows running tasks in a strip and the word would mean two
things), why not `Selection` (*Terminal here* acts on the folder, not a selection),
and why not `Actions` (every entry in a menu is an action).

`Section::Tools` is gone rather than deprecated, which breaks any out-of-tree plugin
that named it. That is deliberate: an alias would let a plugin keep dodging the
question this change exists to force. `docs/WRITING_PLUGINS.md` documents both
sections and the default is now `Workflows`, since a contributed feature tab is the
common case.

Two tests, at both levels. The registry one proves the sections come out in a fixed
order and that entries land where they asked to; the application one proves it for
the real eleven entries rather than for a registry fed by a test. What no test can
settle is whether a given entry was *filed* correctly — that is what the rule and
its worked examples are for.

## The F4 menu stopped answering the keyboard halfway along

Opening the menu with F4 worked, and so did stepping along the headings and opening
one with Right or Enter. Coming back out was where it ended: Left closed the
submenu and left the menu it came from without the keyboard, so the arrows did
nothing from then on and the only way forward was the mouse — which is the whole
thing F4 exists to avoid.

Measured rather than assumed, and it took three attempts to get right, each one
corrected by what the previous measurement said. Restoring the focus inside the
`closed` handler does nothing, because Qt moves the focus as part of closing the
popup and takes back anything claimed there; deferring with `Qt.callLater` does
work, but `closed` only fires once the exit transition has finished, which is
around a fifth of a second in which the menu is still deaf — long enough for the
next keystroke to fall into the gap, and long enough that an early version of the
fix reset the highlight *after* the user had already moved off it. The hand-back
now happens on `aboutToHide` as well, and it only restores the highlight when Qt
has actually cleared it, so it can never undo a heading the user has just moved to.

The five submenus were five near-identical blocks, and they had already drifted:
only File declared `focus: true`. They are one `SectionMenu` component now, which
is why the behaviour cannot differ between them again — the drift was part of the
bug, not tidiness. Each submenu's `objectName` follows its section name, so a test
can address one without a second thing to keep in step.

`f4MenuWalksIntoSubmenusWithTheKeyboard` walks the whole path — open, along,
in with Right, within, out with Left, in again with Enter, out with Escape — and
was checked against the bug by putting it back. The rule this belongs to is in
[ADR-0002](docs/adr/0002-window-shortcuts-versus-focused-views.md), alongside the
terminal: focus declared is not focus held, and that applies coming back as much as
going in.

## The terminal did not get the keyboard, and Ctrl+D bookmarked instead of closing

Opening the panel left the keyboard on the file list, so the shell was on screen
while what you typed went somewhere else, and it took a click before it answered.
`focus: true` declares an intention, not a fact — something has to call
`forceActiveFocus()` when the panel is revealed, which is what the menu already
does when F4 opens it. The panel now does it too, on both paths: when it is
revealed after it exists, and when it exists only once it is already being
revealed.

`Ctrl+D` was the more interesting one. In a shell it means end of input, and the
encoding for it was already correct — `Ctrl+A`..`Ctrl+_` become control characters,
which is how `Ctrl+C` reaches the shell as an interrupt. The key simply never
arrived: it is a window `Shortcut` bound to `mole.bookmarks.add`, and Qt matches
shortcuts before offering the key to whatever has the keyboard, so the panel's own
handler — the one whose comment insists that every key goes to the shell — was
never consulted. The panel now accepts `ShortcutOverride` for everything, which is
Qt's own way for a focused item to say the key is its business.

A shell that ends should take its panel with it, which is what closing a terminal
means everywhere else, so a clean exit hides the panel. A shell that died of
something keeps it open along with the exit code, because otherwise the reason
disappears with the window.

This was the third collision of the same kind, after `F5` being swallowed by
`StandardKey.Refresh` and `Ctrl+W` being claimed by a read-only editor, so the rule
for all three is written down in
[ADR-0002](docs/adr/0002-window-shortcuts-versus-focused-views.md) rather than
being rediscovered a fourth time. Note that the terminal needed the opposite of
`ViewerKeys`: not a view handing a shortcut back to the window, but a view taking
one away from it.

Three assertions, and each was checked against its own bug by putting the bug back.
Typing after the panel opens reaches the shell — typed on the keyboard, not sent
through the controller, because every other assertion about the shell would pass
with the keyboard on the list behind it. `Ctrl+D` ends the shell, closes the panel
and adds no bookmark. And `Ctrl+D` on the listing still bookmarks the folder, which
is where that behaviour belongs.

## A slow table preview looked like a hang

Opening a large CSV showed an empty grid until the whole file had been imported,
however long that took, while the task strip insisted something was running. The
view said nothing, and a view that says nothing reads as a frozen application.

The comment in `TablePreviewController::reimport()` claimed the opposite — *"Rows
appear as they arrive rather than after the whole file"* — and it was not true. The
progress handler called `TableModel::refresh()`, but the model's source was only
attached in the `finished` handler, and `refresh()` without a source reports no
headers and no rows. So every batch of five thousand rows refreshed a model that
had nothing to look at. The store is a database that answers for whatever has been
committed to it, so the source is now attached before the import is submitted, and
the finish refreshes rather than re-sourcing — re-sourcing would clear a filter
typed while the file was still being read.

Fixing that broke a test, which was the interesting part: `parsesCsvWithADetected
Separator` had been using "there are rows" as its signal for "the import has
finished", and the detected separator was only published at the end. With rows now
arriving early, the picker above a half-filled grid was captioned with the default
guess instead of the separator actually in use. The task announces the separator
the moment the shape is settled, before the first row is stored, so the caption
tells the truth from the first row on.

What is left is the gap before the first batch, which on a slow drive is the whole
problem. The view now says it is reading, after one second, in the middle of the
grid — the threshold and the wording follow the file pane, which had solved this
already for slow folders — and gets out of the way as soon as rows land, because
rows are a better answer to "is this stuck" than any spinner.

Both halves are covered. `tableFillsWhileTheImportIsStillRunning` samples the row
count from the progress signal rather than polling, because a poll that arrives one
turn late would be looking at the finished state and pass without ever seeing the
middle. The view half needed a drive that is genuinely slow to open a file, so
`MemoryFileSystem` grew `setReadDelayMs` beside the `setListDelayMs` that the slow
folder test already used.

## Markdown previews were cramped

Qt's Markdown importer gives a heading no space above or below it, sets every
paragraph solid, and hands a fenced code block to the view as nine-point
monospace with no margins and nothing behind it. Rendered, it read as a wall of
text, which is the opposite of what a Markdown file is for.

The document is now restyled after the import: headings get room and a size that
shows the hierarchy, prose gets line spacing, code gets the application's
monospace family at a size that matches the prose and a slab behind it, quotes
keep their nesting and go quieter, tables get cell padding. The view stopped
running the text edge to edge — it keeps margins, and on a wide window the
gutters take the surplus so the line length stays readable.

Two things had to be found out by measuring rather than by reading the
documentation, and both are now written down in
[ADR-0001](docs/adr/0001-markdown-preview-typography.md): a style sheet cannot do
any of this, because `setMarkdown()` never consults one; and wrapping a code
block in a padded frame — the only thing in Qt's rich text with real padding —
injects blank lines into the document and mangles what the file says, so the rule
is formats only, never structure.

Two bugs the tests caught before they could ship. A paragraph that merely opens
with an inline `code span` is given a monospace block font by the importer, so
detecting code blocks that way handed such a paragraph a slab of its own; only
unbreakable lines are a safe signal. And the styling read a quote's nesting depth
out of the very margin it had just overwritten, which flattened every nested
quote to one level — the depth is now recorded before it goes. Applying the
styling twice is a no-op, and a test asserts it, because it runs again on every
change the document makes, including its own.

The one thing left alone is the importer itself: a blockquote and a fenced code
block each end with a stray empty block, and a table placed straight after either
one takes that block into its first cell, which loses its bold. It happens before
any of this code runs, and correcting it would mean editing the document's
structure.

## Terminal panel

A shell for the folder you are looking at, split along the bottom of the window.
Opening it starts there; navigating afterwards does not drag it along, because a
shell has its own idea of where it is and fighting that is worse than leaving it
alone. `Ctrl+\`` opens and closes it, and every other key goes to the shell — a
terminal that let the window keep `Ctrl+C` would be useless.

libvterm does the emulation when it is available, which makes full-screen
programs work properly rather than approximately; the alternate screen is enabled
so leaving an editor restores what was underneath it. Without libvterm there is a
built-in parser covering printable text, the control characters a shell relies
on, cursor movement, erasing and colour — and it says "basic mode" in the header
rather than drawing something subtly wrong.

Two things the emulator has to get right that are easy to miss, and both are
tested: an escape sequence split across two reads, and a multi-byte character
split across two reads. A read boundary falls wherever the kernel puts it.

Not available on a virtual drive, and the panel says so — there is no directory
for a process to start in inside a zip or a bucket.

## Sync

A desktop rsync in its own tab, between any two drives, because everything goes
through the VFS.

Three modes, because everybody's idea of "sync" is different: **Update** copies
what is missing or changed and never deletes, **Mirror** makes the destination
match exactly including removals, **Fill gaps** only adds what is absent. Files
are judged changed by size and time, by size alone for drives whose timestamps
cannot be trusted, or by contents when certainty is worth the reading.

The dry run is the default and Preview is the prominent button. It is not a
simulation of the real path — it *is* the real path with the last step withheld,
which is the only way a preview is worth believing. A mirror that would delete
anything asks again before it does.

Details that only show up when someone relies on them, each with a test:
timestamps get a second of slack, or every sync between two filesystems copies
everything every time; a narrow include beats a broad exclude, because
"everything except .tmp, but definitely notes" is how people express it; a
filtered-out name is never deleted by a mirror, since acting on a rule the user
did not give is worse than leaving a stray file; directories are created before
the files that go in them and deletions come last, so a cancelled mirror cannot
lose something it was about to be handed back.

## Duplicate detection

Four strategies behind one interface, expressed as ordered *stages* rather than
one comparison — because that is the shape the problem has. Every worthwhile
strategy starts with something cheap that rules most files out and only then pays
for something expensive on what is left.

"Identical contents" is size, then a hash of the first 16 kB, then a hash of the
whole file. A test with a counting strategy proves the point directly: of ten
files, ten reach the size stage and two reach the reading stage. Hashing the tree
is the obvious approach and is the difference between minutes and hours on a NAS.

The other three exist because they answer questions content comparison cannot.
"Same name" finds copies that were edited apart — where else did this file end
up — which no hash will ever pair.

Choosing what to keep is the hard half, and the tab never picks for you. It
offers the choices people actually make — keep the newest, the oldest, the copy
nearest the top of the tree — and says what each would free before anything is
deleted. Empty files are ignored: every one is identical to every other, and
listing thousands buries the results that matter.

## Bulk rename

A list of independent operations applied in order, with a live preview of every
file's before-and-after. The preview is the feature: renaming two hundred files
on faith is how people lose an evening.

Eight operations to begin with — replace (plain or pattern), case, insert,
remove, strip a character class, number, affix, extension — and the order is
meaning, not decoration: stripping digits before numbering is a different result
from numbering first, and both are legitimate. A form with eight fields could not
express either.

It refuses a batch that would collide rather than discovering it halfway: two
files taking one name, a name already taken by something outside the batch, a
name reduced to nothing, a name containing a path separator, or a file left with
only an extension. The filesystem would only notice the second collision, by
which time the first file has already moved.

Rules touch the stem by default — upper-casing a name should not turn `.txt` into
something no tool recognises — and `.gitignore` is treated as a name with no
extension rather than as an extension with no name.

## File sets

A named list of files built by hand, from anywhere, across any number of drives,
then treated as a thing in its own right.

The whole design rests on one decision: a set answers `targetUris()`, the same
question a pane's selection answers, under the same name. So bulk rename,
analysis and the rest take a set with no code of their own — a test asserts
exactly that, because it is the property that would quietly rot first. The shell
asks the current tab what it is aimed at and never asks whether that tab is a set.

A set outlives the files in it, so it can be checked: "not looked at yet" and
"not there" are distinct states, because reporting a healthy set as broken before
anything had looked would be worse than saying nothing.

## SQLite and Parquet previews

Two more viewers, both landing on a grid, and neither of them importing anything.

A SQLite file is already a queryable table, so paging and filtering are queries
against the file itself — a database of any size opens at once. It is opened
read-only through SQLite's URI form, the only way it will refuse writes outright:
previewing a file is not a licence to modify it, and a database another process
has open is exactly where that goes wrong. `immutable` is deliberately not set,
because that would promise the file cannot change while another process may well
be writing to it.

Parquet is columnar and stores rows in groups, so a window only decodes the
groups it touches. Filtering it does mean scanning — there is no query engine
behind the format — so the scan is bounded and the view says so rather than
letting an incomplete count look authoritative.

The work was mostly not the readers. The grid was written against the CSV
importer, so it grew an `ITableSource` interface and moved into `DataGrid.qml`;
selection, copying, column sizing and the filter are now shared by all three
viewers rather than existing in three drifting copies.

Arrow is optional. When it is absent `ParquetTable::isSupported()` is false, the
provider declines the file and it falls through to the information viewer — a
missing optional library must never stop the application being built. Arrow's
headers have to be included before any Qt header, because Arrow declares a
parameter named `signals` and Qt's macro of that name expands to `public:`.

## Permissions of the current folder

Beside the report and index tags, what the current user may do here.

Modelled as questions — may I read, write, add files here, delete this — rather
than as mode bits, because POSIX mode bits do not describe a Windows ACL and
neither describes a bucket policy. `Unknown` is a first-class answer: a drive
with no idea says so and the interface shows nothing, rather than a guess
presented as fact. The native form is offered alongside where the platform has
one, which on Linux is the nine characters everybody reads.

Optional on `IFileSystem` like `space()`, and the conformance suite now checks
the contract from both sides: a backend advertising the capability must answer,
and one that does not must refuse rather than return something empty.

Removing an entry is governed by the parent directory, not the entry — a
read-only file in a writable folder can still be deleted, and reporting otherwise
would be wrong in the direction that matters.

## Tasks report whatever their work is about

Progress was counted in files, which is useless for the case a progress bar
exists for: one 4 GB file sat at 0% and then jumped to 100%. It is counted in
bytes now, with throughput measured over a short window rather than over the
whole run, so a stall shows up instead of being averaged away by a fast start.

More importantly the mechanism is general. A task publishes named metrics — a
key, a label, a value and a kind (count, bytes, rate, duration, text) — and the
strip lays out whatever it finds. Sync, duplicate detection and bulk rename will
each have something different worth watching, and none of them should require the
interface to learn new vocabulary. Bytes and speed are simply the first two
users, published through a convenience that also drives the percentage.

Every task also carries when it started and how long it has been going, frozen
once it ends. An elapsed time that keeps counting after the task finished is not
a measurement of anything.

A cancelled task no longer animates. Progress of -1 means "unknown", which is
right while running and wrong afterwards — a cancelled scan was left with a bar
sweeping for ever, as though the work were still going.

## Copy and move ask the right questions first

The confirmation now shows how much is going where, offers a different name for a
single item, and names the files that already exist at the destination *before*
anything happens. Collisions come from the listing the other pane has already
loaded, so the warning is on screen the instant the dialog opens — the only
moment it is useful.

The conflict choice is explicit: stop and report, skip that file, or overwrite.
Stop is the default, because a prompt whose safe answer is not the default is a
prompt that will one day overwrite something by reflex. Choosing overwrite says
plainly that there is no undo.

## Opening a report is not a rescan

`setTargets` always walked the tree, so looking at yesterday's numbers cost a
full scan — minutes on a large folder. It loads what is saved now and walks only
a folder that has nothing saved, because an empty tab would be useless. "Analyse
folder" is a separate method that always walks, which is what asking for it
means.

## Smaller things

- **Ctrl+G showed a clipped path field.** The crumbs and the editable path share
  one slot, and the slot had a hard-coded height of 30 while a Material text
  field wants 40 — so the field appeared with its text and underline cut off,
  which reads as being covered rather than as being too small. The slot is
  measured from the field now, and from the field rather than per mode, or the
  bar would change height as the keyboard moved into it.
- **The waiting view is centred.** Same fault as the empty window: a
  `ColumnLayout` is only as wide as its widest child, so the message sat against
  the left edge of the pane. Now tested against a drive that is genuinely slow,
  which also covers the one-second threshold for the first time.
- **The mouse no longer highlights rows.** Two highlights competed for one
  meaning — the cursor is where Enter will act, and a second one trailing the
  pointer made it ambiguous which row that was.

## F5 did not copy anything

Two causes, both of which the test suite was blind to.

The first was mine, from converting `canTransfer` to a property: `BrowserView`
still called it as a method in one place. Calling a bool as a function throws,
and the throw took the rest of the handler with it — so F5 opened no dialog,
reported no error and did nothing at all.

The second was older. `StandardKey.Refresh` is `F5` as well as `Ctrl+R` on this
platform, so the window shortcut consumed F5 before the pane ever saw it. Refresh
is `Ctrl+R` only now; F5 is the commander copy key and the window has no business
taking it.

Both survived a green suite because every existing test called
`copyToOtherPane()` on the controller. The walkthrough now presses F5, accepts
the real dialog and waits for the file to appear on the other side — the only
shape of test that could have caught either fault. Finding the dialog needed a
harness addition: a `Dialog` is a `Popup`, absent from the visual tree, and
`QObject::findChild` on the window finds nothing because QML does not parent
items into the window's QObject tree. `object()` searches both hierarchies.

## Background work you cannot miss

The strip reported a count, which read as decoration. While anything is running
it is now tinted, ruled in the accent colour, and carries the running task's
name, a real progress bar and its status line — collapsed, without expanding
anything.

Finished rows retire themselves after an hour. `Task` is stamped when it reaches
a terminal state and the manager sweeps every minute. A list nobody prunes grows
for the whole session, and by the end the one failure worth seeing is buried in
it. Work still running is never swept, however stale the list.

`Hidden` moved from the status line to the toolbar, beside copy and move: it
changes what the pane shows, which is what that strip is for. It applies to both
panes, so a dual view cannot hold two different ideas of what is in one tree.

## The browser toolbar says what is already known

`Copy` and `Move` never enabled in dual pane. They were bound to
`controller.canTransfer()` — an invokable, so there was no change signal and QML
evaluated the binding once. Switching to Dual satisfied the condition with
nothing to notice it. It is a `Q_PROPERTY` with a notify signal now, fed by every
input the answer depends on: the mode, the selection and whether the far side is
writable.

The `Index folder` button is gone; indexing is a once-in-a-while action and lives
in the menu. In its place the strip carries what the application already knows
about the folder: whether it has a report (clickable — it opens the saved one
rather than rescanning), whether an alert is watching it and whether that alert
has tripped, and whether it has been indexed and when.

The index is asked about the volume the folder sits *under*, not the folder
itself: scanning `/data` indexes `/data/projects` too, and claiming otherwise
would send the user to re-scan what is already there.

## Report and alert tags on the listing

The same facts per row, beside the date. Affordability was the whole design
question: a store lookup per row would make a listing of five thousand entries
pay thousands of file opens for two small tags. Instead the report store hands
over its stored folder names in one directory read, and each row is a hash and a
set lookup with no I/O at all.

## Reports library

A tab listing every saved report — folders on the left, that folder's runs on the
right, with what each run changed by against the one before it. Sorted by most
recent activity rather than by name, because a library sorted alphabetically
makes you hunt for the one thing that moved.

The store moved out of the analysis feature and into the host, alongside the
schedule and the alerts. Three things now need it — the library, the browser
strip and alerts reading the latest report — and a store owned by one tab is a
store the others cannot see.

## Clickable breadcrumbs

`/mnt/nas/projekty` is now `/ › mnt › nas › projekty`, each piece a target.
Pressing Backspace once per level was work the interface could do. Typing is
still there on Ctrl+G or a click past the last crumb, because a pasted path has
to go somewhere. A long path scrolls to keep the end in view rather than the
start, which you already know.

## Going back restores the cursor

Navigation left the cursor at the top of every listing, so walking a tree meant
restarting at each level. The pane now remembers where the cursor stood in each
folder, and stepping up lands on the folder just left. Bounded to a few hundred
folders: the convenience is not worth an entry per folder ever visited.

An entry that has since been deleted falls back to the first row — landing on a
stale index would be worse than landing on the top.

## Ctrl+W in a preview

Clicking into a preview stopped `Ctrl+W` from closing the tab. Measured rather
than guessed: a `Keys` handler on the text area showed the key arriving there,
which meant the read-only `TextArea` had accepted the shortcut-override event and
Qt had skipped the matching `Shortcut`. `Ctrl+W` is `DeleteStartOfWord` in the
standard editing bindings, and the control then discarded it because the document
is read-only.

Qt offers no declarative way to un-claim those keys, so `ViewerKeys.qml` hands
them back to the window — one relay rather than a private copy of the shortcut
table per view, and narrow enough to leave the keys a viewer genuinely uses.

Two further defects surfaced while doing it. `attachHighlighter` was called on
every text change, and attaching rehighlights, which changes the text: infinite
recursion, reported only as a stack-overflow warning nobody had read. And the
final window of a large file was unreachable — snapping the window's start back
to a line boundary shortened it, so "is there more after this?" was always yes.

## Alerts

A tab that lists what is being watched, what tripped, and a form for watching
one more. Eleven metrics — total size, free space in bytes and per cent, file
and folder counts, largest file, hours since anything changed, permissions, last
modified, existence, unreadable folders — compared with above / below / changed
/ equals.

Two design points worth keeping:

- A metric that could not be read is `Failed`, never `Ok`. An unreachable drive
  reported as a green tick is the worst outcome available, because it looks
  exactly like everything being fine.
- `Changed` treats its first reading as a baseline rather than firing. Otherwise
  every alert would trip the moment it was created, which teaches the user to
  ignore the first one they ever set.

An alert can read from the latest saved report instead of measuring live —
instant, and only as fresh as that report, which is what scheduling the report
is for. It deliberately does not fall back to a live walk when no report exists;
that would quietly turn it into a different alert with different timing.

## Delimited files with no row limit

The CSV/TSV viewer stopped at 5000 rows and filtered only what it had loaded.
Now the file is streamed into a scratch SQLite database and every question is a
query, so paging and filtering cover the whole file however large it is.

- `DelimitedStreamParser` parses a chunk at a time and carries the state a row
  straddling a chunk boundary needs, including a quoted field with newlines in
  it.
- Columns are measured from the contents during the import, so the grid fits
  what is in it instead of a default that wasted half the window.
- Cells select by click, shift-click and drag, and copy as tab-separated text —
  what every spreadsheet expects on paste.
- The table widens to the widest row in the sample rather than to the header
  alone: a header that does not mention every column is common, and sizing to it
  would silently drop the extra fields.

## Text preview of very large files

The viewer held the whole file. Now it holds only the window being shown, read
through a seek, so a 100 GB log opens as fast as a 100 byte one. Windows snap to
line boundaries at both ends, or paging would show a severed line at every step.

Alongside it: source highlighting for twenty-odd languages from a table rather
than hand-written code per language, Markdown rendered instead of coloured, and
one monospace family chosen once by the application so every code and data view
lines up.

## Drive capacity in the sidebar

Each drive shows how full it is — amber past three quarters, red past nine
tenths — with free and total beside it. Drives that have no meaningful capacity
show only a name: a bucket has no size in any useful sense, and a chart is read
as a fact.

`IFileSystem::space()` is optional and defaults to `NotSupported`; only backends
advertising `ReportsSpace` are asked. The query goes through a task like
everything else that touches storage, because `QStorageInfo` blocks on an
unreachable NFS mount and the UI thread must never wait on a disk.

## Automation

Reports can be put on a clock. `Scheduler` polls rather than arming a timer per
rule, because a laptop asleep for two days has to notice on waking. A rule that
has never run is due immediately, so a job whose turn came while the application
was closed runs at the next start instead of waiting out another interval.

The tracking tab sorts broken rules first, counts consecutive failures, and
records three things the design deliberately makes visible: a rule whose plugin
is gone is `Skipped` with the reason, a run interrupted by a quit comes back as
`Failed` rather than stuck at `Running`, and every attempt is logged whether or
not it fired.

## A test harness that does not lie

The Xvfb-and-`xdotool` setup was worse than no harness: with no window manager
there is no X input focus, so roughly two of every six synthetic keystrokes
arrived and tests went green because nothing happened. It cost four rounds on
one Enter-key bug — a real defect that looked like a harness fault, next to a
harness fault that looked like a real defect.

`QmlAppHarness` builds the whole application offscreen and posts keys straight
to the `QQuickWindow`: the same delivery path production uses, minus the
display. Screenshots come from `QQuickWindow::grabWindow()`, so `make
screenshots` cannot produce a picture of a state the assertions did not just
verify.

## Smaller things

- **Closing a tab returns to the one it was opened from.** Each tab remembers
  its opener; position alone would send you to whichever tab happened to sit
  next to it, somewhere you were never working.
- **A working tab says so.** `FeatureController::busy` now reaches the tab strip
  as a spinner, so a report still running on a large tree does not look like one
  that finished.
- **The filter keeps the keyboard while narrowing.** Enter opened nothing and an
  arrow moved focus instead of the cursor; both cost a keystroke and swallowed
  the one just spent. Fixing it surfaced a second defect: navigating with a
  filter active left the new folder silently filtered by the old term, with
  nothing on screen to explain the missing files.
- **The empty window is centred.** A `ColumnLayout` is only as wide as its
  widest child, so centring inside it put the block off to one side of a wide
  window; and the tab stack kept claiming half the height until it was hidden
  rather than merely emptied.
- **The empty window offers only what it can open.** `IFeature::needsContext()`
  marks a tab that is meaningless without a selection — a preview needs a file,
  a report needs a folder — and those are left out. A button that opens an empty
  tab reads as broken rather than as inapplicable.
- **Combo boxes size to their widest entry.** The repeat picker had a fixed
  width and truncated its own labels.

## Adding a drive did nothing

Reported: pressing "+" under "Your drives" had no visible effect. Three separate
faults sat on top of each other, and each on its own was enough to produce
exactly that symptom.

**The form had no size.** The field area was a `ScrollView` whose content was
sized to the view. A `ScrollView` takes its own implicit size from its content,
so that closes a loop: width decides whether a scrollbar is needed, the
scrollbar decides the available width, the available width decides the width.
Qt detects the loop, prints `Polish loop detected. Aborting after two
iterations.` and abandons the layout — which leaves every child present,
visible in the tree, and zero pixels tall. It is now a `Flickable`, which takes
its size from the layout that placed it and is told its content size, so
nothing feeds back.

**The list could not update.** `model: App.configuredDrives()` and
`model: App.driveKinds()` bound to method calls. A method call in a QML binding
has no change signal, so it is evaluated once and never again — a saved drive
would never appear in the list beside the form however well the save worked.
Both are now `Q_PROPERTY` with `NOTIFY drivesChanged`, emitted from save,
remove, connect, disconnect and unlock. This is the third time this session
this exact defect has appeared, after `canTransfer()` and `rowSpans()`.

**Pressing add changed nothing.** The dialog already opened in the blank
"new drive" state, so a button that reset it left the screen exactly as it was.
`beginAdding()` now opens the kind picker as well, and the empty right-hand
panel says what to do instead of sitting blank. The reset stayed a separate
function because saving also resets, and a save should not throw a
sixty-backend dropdown open in the user's face.

Also capped that dropdown's height: at full size sixty entries covered the
dialog, the window behind it and the drive list being chosen for.

The lesson is the one already recorded above under the F5 bug, restated: every
existing drives test called `saveDrive()` on the controller, and the controller
was fine. Only a test that presses the button can see a button that does
nothing. `theDrivesDialogOffersBackendsAndAForm` now clicks "+", picks a kind,
asserts the form is on screen *with a size*, saves, and waits for the drive to
appear in the list.

## A log for the runs that end badly

`make run` now keeps a session log, because a crash takes the terminal's
scrollback with it and the useful lines are the ones printed just before the
fall.

Every line is flushed to the file and then to the operating system as it is
written. A buffered log loses exactly the part worth reading. The previous run
is kept beside the current one as `session.log.1`, because the way anybody
reacts to a crash is to start the program again, and that restart is what would
otherwise destroy the evidence.

When the program does fall over, a signal handler writes the backtrace into the
same file and then lets the process die exactly as it would have -- default
handler restored, signal re-raised -- so the shell still reports a crash and any
core pattern still gets its core. The handler allocates nothing, locks nothing
and calls no Qt: all three deadlock or fault a second time in a process that is
already broken. It writes with `write()` and `backtrace_symbols_fd()` to a
descriptor captured when logging started.

The tests crash on purpose, in a forked child, and the parent reads what the
child managed to write on its way down. That covers the part that is easy to
get wrong and impossible to notice: `tst_SessionLog` caught the signal number
being written as "06" instead of "6".

## Two type errors in the drives form

Reported alongside the above: the form appeared but printed
`Unable to assign QJSValue to QString` once per field.

Field defaults come from each backend's own metadata, so they arrive as
numbers, booleans, nulls and lists as well as text -- and a text field handed a
list refuses the whole binding, leaving the field blank. `fieldValue()` now
returns a string whatever it is given. A second warning with the same root,
`Could not convert array value at position 0 from QString to QChar`, went with
it: that is what assigning a JS array to a string property looks like.

The test that covers it builds a form for every one of the 59 backends and
fails on any warning at all, rather than leaving them to be noticed in a
console. 707 fields, silent.

## The segmentation fault: a layout re-entering itself

Reproduced, with a stack, and fixed.

The log's own backtrace stopped after two frames -- rclone brings a Go runtime
into the process, Go forwards signals to handlers installed before it started,
and no in-process unwinder can walk past that trampoline. So the application was
driven under gdb on a virtual X server with xdotool, through the reported steps:
open Drives, set a passphrase, then switch backends in the dropdown. It fell over
on the fifth switch, and gdb had the whole stack:

    qmlAttachedPropertiesObject
    QGridLayoutItem::stretchFactor
    QGridLayoutEngine::setGeometries
    QQuickGridLayoutBase::rearrange
    QQuickLayout::geometryChange
    QQuickItem::setWidth
    QQuickFlickable::geometryChange      <- the scroller
    QQuickGridLayoutBase::rearrange      <- already rearranging
    QQuickLayout::updatePolish

A layout inside a scroller, with its width bound back to that scroller,
re-enters itself: the outer layout sizes the scroller, the column's width
binding fires, and the column rearranges while the outer rearrange is still on
the stack. Switching backends destroys every field while that is happening, and
the layout engine reads the attached `Layout` properties of an item already on
its way out. That read is the crash.

It is the same circularity that produced `Polish loop detected` earlier in this
file. Fixing the warning did not fix the loop -- it moved it somewhere Qt could
not detect it, where instead of giving up it recursed until it touched a dead
object. The fields are now a `ListView`: a view owns its delegates, expects its
model to change under it, and never drives the layout that placed it.

Two things the harness could not see, and one it now can. It could not see the
crash: offscreen and even on a real X server, driving the same properties from
C++ never produced the polish cycle that a rendered window does. It could not
see that the dropdown had stopped opening at all -- capping `popup.height`
against `popup.implicitHeight` collapsed it to nothing, because that height is
zero while the popup is shut. That one is now a test: open the dropdown and
assert it is between 100 and 340 pixels tall, since "opens" and "is visible" are
not the same claim. The form test also now asserts the fields have a width,
which is what a layout the engine has abandoned does not give them.
