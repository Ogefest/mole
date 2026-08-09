# Project: testing tasks away from the interface

- **Started:** 2026-08-09
- **Status:** planned, not begun

## Why

A day of chasing one copy that would not work turned up five faults, and the
suite had been green throughout every one of them:

- an SFTP transfer that stops dead just short of a gibibyte, which no test could
  see because every test file was measured in kilobytes;
- a download handed over as a whole file when it had stopped early;
- a copy loop that read a failed read as the end of a file and reported success;
- a listing of a file answering "not found" on one server and "not a directory"
  on another, which only a second real server revealed;
- a seek on a stream whose behaviour depended on how much of the network had
  arrived — a race that was written, reviewed and merged the same day.

Two things follow. **Size and hostility are where the faults are**: a task that
works on a 4 KB file in a quiet room proves almost nothing about the same task on
a 94 GB file over a connection that re-keys. And **the interface was never the
problem** — every one of those faults lives under `TransferTask` and the VFS, and
every one could have been caught without a window on screen.

That second point is the opening this project works from. A task takes its
parameters, runs on a pool thread, touches storage through `IFileSystem`, and
reports through metrics; nothing in it knows what a window is. The existing
suite already proves this is true rather than merely intended — `tst_TransferTask`
drives real tasks through a `TaskManager` with no GUI at all. What is missing is
not the possibility of testing tasks in anger. It is the equipment.

## What already exists to build on

- **`tests/support/FileSystemConformance`** — the contract every backend is held
  to. It seeds through an out-of-band channel so a backend cannot mark its own
  homework. Today it runs against local disk and memory always, and against a
  real server only when `MOLE_TEST_*` credentials are in the environment, which
  in practice means never.
- **The wrapper seam.** `LoggingFileSystem` wraps every mount inside
  `VfsManager::addMount`, so a decorator around a drive is a shape the codebase
  already has and already trusts. A fault-injecting wrapper is the same shape
  with a different job.
- **Three ad-hoc fakes, written in one day.** `CommitFailingFileSystem`,
  `HalfReadingFileSystem` and `ForgetfulFileSystem` all live in
  `tst_TransferTask.cpp`, each invented to reproduce one fault. That they were
  each written separately, and that a fourth would be written the same way
  tomorrow, is the argument for a general one.
- **`TaskManager` and `waitForTask`** — headless task execution with an event
  loop, already used by nineteen test binaries.
- **`MOLE_LOG`** — when one of these scenarios fails at three in the morning, the
  session log is what says which of eleven concurrent transfers stalled.

## The three tiers

The tiers differ in what they cost and in how much they can prove. Every
scenario belongs to exactly one, and the choice is always the cheapest tier that
can express it.

### Tier 1 — in-process, deterministic, no network

A fault-injecting `IFileSystem` wrapper and a set of scenarios written as
ordinary QtTest cases. Runs inside `make test` on every change, on any machine,
in seconds.

**The one rule that makes this tier worth having: no sleeps.** A fault fires at a
byte offset, not after a wall-clock delay — "when the copy has read 30% of the
file, rename the source underneath it". A test that waits 200 ms for a race to
happen is a test that passes on a fast machine and fails in CI, and a race that
only reproduces sometimes is worse than one that never reproduces at all.

### Tier 2 — real servers on Proxmox

The same tasks against genuine OpenSSH, WebDAV and S3 implementations, on a
machine that can be interfered with: services stopped mid-transfer, a disk filled
up, latency and packet loss applied to an interface, the whole VM rolled back to
a snapshot between runs.

This is the tier that would have caught the re-key stall and both listing
behaviours. Run on demand and before anything is called finished, not on every
commit.

### Tier 3 — scale and the real world

10 GB to 100 GB transfers, hours-long runs, and the genuine remote endpoints:
Backblaze B2, a real SFTP server over a real link. Invoked by hand or on a
schedule when the thing being changed is a transfer path.

## Phases

### Phase 1 — the server environment

**Deliverable.** A Proxmox VM, provisioned by a script in the repository, running
everything the backends talk to, plus the documented way to point the existing
suites at it.

- One VM, Debian stable, on the local Proxmox. Services: **OpenSSH** for SFTP,
  **Apache `mod_dav`** for WebDAV — the backend that has never once met a real
  server — **MinIO** for S3, and **vsftpd** for FTP.
- A **second sshd on another port**, configured differently: an explicit
  `RekeyLimit`, and a different cipher list. The re-key stall is a property of a
  server configuration, and the fix for it should be held against a server that
  provokes it and one that does not.
- A **small dedicated disk** for the WebDAV and FTP roots, a few gigabytes, so
  "the destination fills up" is a real condition rather than a mocked one.
- A **control channel**: an ssh account the tests can use to stop a service, fill
  a disk, or apply `tc netem`. This is what turns a server into equipment rather
  than scenery. Reached through a new `MOLE_TEST_CONTROL` endpoint, and absent by
  default, so nothing that runs on a developer's machine can reach for it.
- **Snapshot and roll back** around a run, so destructive scenarios are safe and
  every run starts identical. Named snapshot, documented, scripted.
- Credentials in the environment as they are now, never in the repository. A
  throwaway account; the whole VM is disposable by construction.

**Acceptance.** `make test-live` runs every existing conformance suite against
this VM and passes, including WebDAV's, which has never been run at all. Standing
the machine up again from nothing is one documented command plus a wait.

**Why this phase first.** It converts tests that already exist and silently skip
into tests that actually run — the cheapest coverage available anywhere in this
plan — and phases 2 and 4 both want the equipment.

### Phase 2 — the fault-injecting filesystem, and the fast scenarios

**Deliverable.** `tests/support/FaultyFileSystem`, replacing the three hand-rolled
fakes, plus a first vertical slice of hostile scenarios through `TransferTask` --
enough to prove the wrapper is the right shape. The full catalogue is phase 5,
and is deliberately not attempted until the equipment has been used in anger
once.

The wrapper takes a policy: what to do, and at which byte. Faults it must be able
to produce, because each corresponds to something already seen or plainly
possible:

| Fault | What it reproduces |
|---|---|
| read fails after N bytes | a connection dropped mid-transfer |
| read returns short, then recovers | ordinary streaming, which callers get wrong |
| read stalls (blocks until released) | the stall that a guard is supposed to catch |
| write accepts and stores less | a server that acknowledges and loses |
| write fails on close | every remote backend's commit path |
| the file changes size under the reader | the source rewritten mid-copy |
| the file vanishes under the reader | the source deleted mid-copy |
| access revoked part way | permissions changed during a long job |
| the destination fills up | the obvious one, still untested |
| listing lies about a size | what the arrival check is for |

**The scenario catalogue.** Written against `TransferTask` first, then `SyncTask`,
`CompressTask`, `FindDuplicatesTask` and `VerifySetTask`, which are the tasks that
move or compare bytes:

- source renamed, deleted, truncated or grown at 30% of a copy;
- destination removed, or replaced by a directory, mid-copy;
- cancellation at every stage — before the first byte, mid-file, between files,
  during the arrival check, during a move's delete pass;
- a move whose verification fails must keep the original (already covered; it
  belongs in the catalogue rather than in one test file);
- **concurrency**: ten copies over one drive at once, sharing one connection pool
  of eight handles; a hundred over ten drives; half of them cancelled while the
  rest continue; two tasks writing the same target;
- **a drive unmounted while tasks are using it** — `VfsManager::removeMount` with
  transfers in flight. A `shared_ptr` should make this safe; nothing proves it.

**Acceptance.** The catalogue runs in `make test` in under a minute, with no
sleeps anywhere in it, and every scenario fails for the right reason when the
fault it names is reintroduced.

### Phase 3 — the headless task runner

**Deliverable.** `mole-tasks`, a console binary linking `mole_core` and the
plugins, that starts any task against any configured drive and reports what it
did: `mole-tasks copy --from sftp://nas/big.bin --to file:///tmp --log net,curl`.

It is deliberately not a scenario language. Scenarios are C++ tests, decided
in this project's opening question. What the runner is for is the other half:
reproducing something by hand against a real server, driving tier 3 without a
window, and giving tier 2 a way to run a task under `tc netem` from a shell
script.

**Acceptance.** Every fault found today can be reproduced with one command
against the phase 1 VM, and the binary needs no display.

### Phase 4 — scale, and interference

**Deliverable.** `make test-heavy`: the scenarios that need real size and a
server that can be attacked.

- 10 GB and 100 GB transfers in both directions, byte-verified, with peak local
  scratch space measured and asserted — the check that would have caught staging
  before it became a wall;
- sshd stopped and restarted mid-transfer; the network interface dropped and
  restored; `tc netem` at 200 ms latency and 1% loss;
- the destination disk filled while a copy is running;
- a transfer that crosses the re-key point on both server configurations;
- a copy left running for hours, watched for handle and memory growth.

**Acceptance.** Each one either passes or produces a named, reproducible failure
with a session log — and every failure found here becomes a tier 1 scenario, in
the wrapper, where it can be checked in a second for ever after.

### Phase 5 — the catalogue

**Deliverable.** The tests themselves, once phases 1 to 4 have made them cheap to
write and possible to run. Phase 2 delivers a first vertical slice through
`TransferTask` to prove the equipment; this phase is everything else.

**The priority is not evenly spread, and the reason is worth stating.** A
shortcut that does nothing is noticed in a second and costs nothing. A copy that
was 99% of a backup is noticed a year later, by which time the original is gone.
So the order is: the tasks that mutate data first — `TransferTask`, `SyncTask`,
`DeleteTask`, `CompressTask`, `RenameTask` — and within them, the failure mode
that matters most is not the crash but the **silent partial success**. Anything
that can end with "done" and leave less than it promised outranks everything
else in this catalogue.

**Four rules, for every entry below.**

1. **Verify out of band.** What the destination holds is established by a channel
   that is not the backend under test — the conformance suite already works this
   way, and it is the only reason a listing bug cannot cancel out a writing bug.
2. **Compare content, not size.** A size check catches truncation; only content
   catches bytes delivered in the wrong order, a duplicated span, or a hole.
3. **No sleeps.** Faults fire on byte offsets.
4. **Every entry names the loss it prevents.** A test whose failure nobody can
   interpret gets deleted the first time it goes red for an unrelated reason.

Tier 1 is in-process, tier 2 needs the Proxmox machine, tier 3 needs size or the
real world.

#### 1. A file arrives whole

| Scenario | What it proves | Tier |
|---|---|---|
| Byte-exact copy across every pair of backends: local, memory, archive, SFTP, FTP, S3, WebDAV | The matrix nobody has ever run in full; each pair has its own staging, streaming and commit path | 2 |
| The same, hashed rather than size-compared | Bytes in the right order, not merely the right number of them | 2 |
| Empty file, and a one-byte file | The degenerate ends of every loop, and an empty object is a real thing in S3 | 1, 2 |
| A file exactly at each threshold, and one byte either side: 256 KiB copy chunk, 8 MiB stream buffer, 64 MiB staging, 64 MiB S3 part, 256 MiB span | Every one of those numbers is a branch, and off-by-one at a boundary is the classic way to lose a byte | 2 |
| Exactly 1 GiB, and 1 GiB + 1 | The re-key point, from both sides | 2 |
| 2 GiB + 1, and 4 GiB + 1 | Signed 32-bit and unsigned 32-bit overflow, in our code and in everyone else's | 3 |
| 5 GB + 1 to S3 | The single-PUT ceiling multipart is supposed to have removed | 3 |
| A file whose length is an exact multiple of the chunk size | The final read returns zero rather than a short read — the case that distinguishes "ended" from "failed" | 1 |
| A sparse file | Content identical, and the copy does not silently become the expanded size | 2 |
| Ten thousand files of a few hundred bytes | Per-file overhead, descriptor and handle leaks, and whether the pool survives it | 2 |
| A tree one hundred levels deep | Recursion, path length, and the walker | 1, 2 |
| A directory with a hundred thousand entries | Listing memory, pagination, and the interface's idea of progress | 2 |

#### 2. Names and shapes that break things

| Scenario | What it proves | Tier |
|---|---|---|
| Spaces, double and single quotes, backslashes, tabs, newlines in a name | SFTP sends quoted commands; a newline in a name is a second command | 2 |
| `#`, `?`, `&`, `%`, `+`, `=` in a name | Every remote backend builds a URL, and each of these means something in one | 2 |
| Percent-encoded-looking names (`%20`, `%2F`) | Double-encoding and double-decoding, in both directions | 2 |
| Emoji, right-to-left marks, combining characters | UTF-8 through four protocols and a listing parser | 2 |
| The same name in NFC and NFD in one directory | Whether two files become one, which is silent data loss | 2 |
| A 255-byte name, and a 4096-byte path | The limits, and what happens one past them | 2 |
| A name beginning with `-` | Argument injection into anything that builds a command line | 2 |
| Names like `...`, `..foo`, `.`, `..` as content | Path handling that assumes only the two real ones exist | 1 |
| Two names differing only by case, into a case-insensitive destination | One overwriting the other, unremarked | 2 |
| A symlink to a file, to a directory, broken, absolute, relative, and a loop | Whether a copy follows, recurses for ever, or refuses; whether a *delete* follows one out of the tree | 1, 2 |
| A hardlinked pair copied together | Two files or one; either is defensible, silently doing the other is not | 2 |
| A fifo, a socket, a device node | Refused with a clear error rather than hanging on an open | 2 |
| A file with no read permission; a directory with no execute permission | The error is reported, the job continues, and the count is honest | 2 |
| Setuid, setgid and sticky bits, and the executable bit | What a copy claims to preserve and what it actually preserves | 2 |
| A file owned by another user, on a drive where we can read but not write | The listing's `isWritable` versus what actually happens | 2 |

#### 3. Something changes underneath

Every one of these is the fault at 30% of a large copy, deterministically
triggered by byte offset. Both a tier 1 form through the fault-injecting
wrapper and a tier 2 form on the real machine.

| Scenario | What it proves | Tier |
|---|---|---|
| Source renamed | The transfer either completes from the open handle or fails; it must not produce a short file called a success | 1, 2 |
| Source deleted | The same, for the case where the bytes genuinely stop | 1, 2 |
| Source truncated to half | The copy fails rather than reporting the file it was promised | 1, 2 |
| Source grown while being copied | The result is coherent, and the arrival check does not fail a file that legitimately changed | 1, 2 |
| Source replaced by a different file of the same name | Whether the copy is of one file or half of each | 2 |
| Source replaced by a directory | An error, not a crash | 2 |
| Source permissions revoked | Reported, and the partial destination is not passed off | 2 |
| Destination deleted | The write either recreates or fails; nothing is silently discarded | 1, 2 |
| Destination replaced by a directory | An error rather than a write into nowhere | 2 |
| Destination made read-only | The failure arrives at write or at close, and is reported either way | 1, 2 |
| Destination's parent directory removed | The error names the parent | 2 |
| Destination filesystem fills at 60% | The disk-full path, which nothing has ever tested | 2 |
| A quota exceeded rather than a disk filled | A different error from a different layer, mapped correctly | 2 |
| A symlink's target swapped between stat and open | Time-of-check to time-of-use, which decides whether a copy can be tricked into writing somewhere else | 2 |
| During a recursive walk: a file appears, a file vanishes, a directory is renamed, a directory becomes a file | The walker reports and continues rather than aborting the job or looping | 1, 2 |
| The drive unmounted mid-transfer | `shared_ptr` lifetime under `VfsManager::removeMount`; nothing proves this today | 1 |
| Credentials re-locked mid-job | The failure is "denied", not a crash | 2 |
| The system clock jumps forwards and backwards | Rate and duration arithmetic, and any timestamp comparison in sync | 1 |

#### 4. The network misbehaves

| Scenario | What it proves | Tier |
|---|---|---|
| Connection killed at N bytes, in each direction | Every one of the four network backends' error paths | 2 |
| The server stopped and restarted mid-transfer | Whether anything reconnects, and whether it lies about what it has | 2 |
| 200 ms latency; 1% and 5% packet loss | That the thing merely gets slow rather than wrong | 2 |
| Bandwidth cut to zero for 119 seconds, then restored | The stall guard's *other* side: a slow transfer must not be killed | 2 |
| Bandwidth cut to zero for 130 seconds | The stall guard fires, with the right error | 2 |
| A transfer that crosses the re-key point on a server configured to re-key, and one configured not to | The fix for today's stall, held against both | 2 |
| A host key that changes between two operations | Refused, always — the one case the trust-on-first-use policy exists to catch | 2 |
| A TLS certificate that expires or changes mid-session | S3 and WebDAV verification is not a one-time check | 2 |
| S3 returns 503 SlowDown on the third part | Whether anything retries, or whether an upload dies on a routine throttle | 2 |
| S3 returns 200 with an error document on complete | Already handled; it stays handled | 2 |
| A `Content-Length` longer than the body, and shorter | The short-transfer check, from both directions | 2 |
| A redirect answered to a PUT | A body is not silently replayed to somewhere else | 2 |
| WebDAV answering 411 to a chunked PUT | The known risk of the streaming write, with a diagnosable failure | 2 |
| An FTP data connection refused, passive and active | The mode fallback, if there is one | 2 |
| A listing that arrives truncated mid-document | A partial listing is an error, not a short directory — a mirror sync would delete the difference | 1, 2 |

#### 5. Cancellation

Each of these asserts three things: the task stops within a second, no partial
file is presented as complete, and the source is untouched.

| Cancelled at | Tier |
|---|---|
| Queued, before it starts | 1 |
| Before the first byte | 1 |
| Mid-file, in a large transfer | 1, 2 |
| Between files in a multi-file job | 1 |
| During the destination walk of a recursive copy | 1 |
| During the arrival check | 1 |
| During a move's delete pass | 1 |
| During archive finalisation | 1 |
| During an S3 multipart complete | 2 |
| Twice, in quick succession | 1 |

#### 6. Killed outright, and what is left

`SIGKILL` at a known offset, then an independent look at what survived, then the
same job run again.

| Scenario | What it proves | Tier |
|---|---|---|
| Killed mid-copy to local disk | The partial file is obviously partial, and a re-run replaces it cleanly | 2 |
| Killed mid-upload to SFTP | The known gap: what is left looks finished. This test is what a temporary-name-and-rename fix would be measured against | 2 |
| Killed mid-multipart to S3 | Orphaned parts can be found and removed; they are being charged for until they are | 2 |
| Killed mid-archive | The originals are still there — ADR-0009 lets a successful archive delete them, so this is the one that matters | 2 |
| Killed mid-index-write | The SQLite WAL recovers and the index is not corrupt | 2 |
| Killed while writing session, drives or credential files | Configuration survives; a half-written credential store is a lost credential store | 1 |

#### 7. Many at once

| Scenario | What it proves | Tier |
|---|---|---|
| Ten copies over one drive, sharing a pool of eight handles | The pool under contention; a handle used by two threads is a corrupt transfer | 2 |
| A hundred copies over ten drives | Thread pool saturation and fairness | 2 |
| Two tasks writing the same target | Last-writer-wins, or a refusal — but not a file that is half of each | 1, 2 |
| One task copying a file another is deleting | Either outcome is acceptable; a truncated copy reported as complete is not | 1, 2 |
| A move and a copy of the same source at once | The move's delete must not run under the copy | 1 |
| Half of fifty running tasks cancelled | The rest finish correctly and the cancelled ones leave nothing behind | 1 |
| A drive unmounted with transfers in flight | Lifetime, again, under load | 1 |
| `TaskManager` destroyed with tasks running | Shutdown does not deadlock or leak a thread | 1 |
| The whole concurrency set under ThreadSanitizer | Races that are invisible until the day they are not. `make asan` exists; this needs a `make tsan` beside it | 1 |
| A soak: mixed operations for an hour | Handle, memory and temporary-file growth over time | 3 |

#### 8. Move, where the loss is permanent

| Scenario | What it proves | Tier |
|---|---|---|
| Interrupted at every stage of a cross-backend move | The source survives every one of them | 1, 2 |
| A move whose arrival check fails | Covered today; it stays covered | 1 |
| A move where the delete fails after a good copy | Both copies exist and the failure is reported — the safe direction | 1 |
| A move onto an existing target, under each conflict policy | Skip, overwrite and fail each do what they say | 1 |
| Rename within one backend, including onto an existing name | The fast path, which never verifies anything because it never copies | 1, 2 |
| A move across devices on local disk (`EXDEV`) | The rename fails and the copy path takes over | 2 |
| A directory moved into its own subdirectory | Refused. Not refusing it is an infinite copy that eats the disk | 1 |
| A directory copied into itself | The same | 1 |
| A move of a symlink | The link moves, not the thing it points at | 2 |

#### 9. Delete

| Scenario | What it proves | Tier |
|---|---|---|
| A symlink to a directory inside a deleted tree | **The recursion stops at the link.** Following it deletes whatever it points at, which is how a delete of a scratch folder takes a home directory with it | 1, 2 |
| Permission denied part way through a tree | The rest continues, the failure is reported, and the count is honest | 2 |
| A tree of a hundred thousand entries | Progress, cancellation, and no stack overflow from recursion | 2 |
| Deleting the drive root | Refused or handled deliberately, not attempted by accident | 1 |
| A path containing `..` that resolves outside the drive root | Refused. Path traversal in a delete is unbounded damage | 1 |
| Deleting an S3 "directory" — marker object, prefix, or both | An object store has no directories, and getting this wrong leaves orphans or deletes siblings | 2 |
| A file being read by another task while it is deleted | Either order is fine; a crash is not | 1 |

#### 10. Archives

| Scenario | What it proves | Tier |
|---|---|---|
| Round trip byte-exact for each format written | What comes out is what went in | 1 |
| A failure part way through writing an archive | The originals are not deleted. See ADR-0009 | 1 |
| An archive written into the tree it is archiving | It does not include itself, for ever | 1 |
| An entry whose name escapes the extraction root (`../../etc/passwd`) | Zip-slip. Reading a hostile archive must not write outside the destination | 1 |
| An entry with an absolute path | The same class | 1 |
| A symlink entry pointing outside the tree | The same class, one step less obvious | 1 |
| A truncated or corrupt archive | An error, not a partial extraction called a success | 1 |
| An encrypted archive | Refused clearly rather than producing rubbish | 1 |
| Duplicate entry names in one archive | Deterministic, and documented | 1 |
| An entry over 4 GiB, and an archive over 4 GiB | Zip64, and every 32-bit assumption on the way | 3 |
| A hundred thousand entries | Memory and listing speed inside an archive mount | 2 |
| An archive on a remote drive, browsed and read | Streaming plus seeking, which is the case the read stream is worst at | 2 |
| "Delete the originals" only after the archive verifies | The order of those two operations is a data-loss bug waiting to be written | 1 |

#### 11. Sync

| Scenario | What it proves | Tier |
|---|---|---|
| A mirror where listing the source fails half way | **Nothing at the destination is deleted.** A partial source listing turned into deletions is the single most destructive bug this application could have | 1, 2 |
| A mirror where one directory of the source is unreadable | The same, in the subtler form | 1, 2 |
| A dry run, against a destination wrapped in a filesystem that fails every write | It genuinely writes nothing | 1 |
| The same sync run twice | The second run does nothing at all | 1, 2 |
| Timestamp granularity per backend: FTP to the minute, S3 to the millisecond, local to the nanosecond | Files are not copied for ever because their times cannot agree | 2 |
| A clock skewed by an hour between the two sides | Newer-wins does not mean "always copy" or "never copy" | 2 |
| Same size, different content, in each comparison mode | Size-only misses it by design; checksum mode must not | 1, 2 |
| A case-insensitive destination | Two source files do not become one silently | 2 |
| Symlinks on both sides | Followed or copied as links, consistently | 2 |
| An interrupted sync, resumed | No duplicate work, and no file left half written | 2 |
| Delete-before-copy versus copy-before-delete ordering | A rename detected as delete-then-add must not remove the only copy first | 1 |

#### 12. Duplicates, and what gets suggested for deletion

| Scenario | What it proves | Tier |
|---|---|---|
| A hardlinked pair | Reported honestly — they are one file, and deleting "the duplicate" frees nothing | 2 |
| Zero-byte files | Every one is a duplicate of every other; the result is not a proposal to delete them all | 1 |
| Same size, different content | The cheap stages are a filter, not a verdict | 1 |
| Files changing during the scan | A stale hash does not become a deletion suggestion | 1 |
| A set spanning two remote drives | The comparison reads whole files across a network; it must stream | 2 |
| A hundred thousand files | Memory, and the staging that is supposed to keep the expensive comparison rare | 2 |
| Any selection rule offered | It can never propose deleting every copy of anything | 1 |

#### 13. Bulk rename

| Scenario | What it proves | Tier |
|---|---|---|
| A batch containing a collision | Refused whole, not applied to the point of failure | 1 |
| A cycle: `a`→`b`, `b`→`a` | Both survive, through a temporary name | 1 |
| A chain: `a`→`b`, `b`→`c` | Order, and no file overwritten on the way | 1 |
| Case-only rename on a case-insensitive filesystem | `A`→`a` is not a delete | 2 |
| A rename producing an NFC/NFD twin of an existing name | Not a silent overwrite | 2 |
| A failure part way through a batch | What was applied is reported precisely | 1 |
| A rename onto a name being freed in the same batch | Either ordered correctly or refused | 1 |

#### 14. The streams themselves

| Scenario | What it proves | Tier |
|---|---|---|
| Read stream: seek to 0, to the exact end, past the end, backwards, forwards past the buffer | Every branch of a device the whole copy path depends on | 1 |
| Read stream: a span boundary landing exactly on a read boundary | The seam between two transfers is invisible | 1 |
| Read stream: a file of size 0 and of size 1 | Degenerate, and both reachable | 1 |
| Read stream: reader much faster than the fetch, and much slower | Both sides of the bounded buffer block correctly | 1 |
| Read stream: read after an error, and close during a blocked read | No hang, no second error, no crash | 1 |
| Read stream: destroyed while the fetch thread is blocked on a full buffer | Covered today; it stays covered | 1 |
| Write stream: closed with nothing written | An empty file is created, not nothing | 1 |
| Write stream: exactly one span, and one span plus a byte | The boundary that decides whether a second transfer happens | 1 |
| Write stream: failure on the first span, and on the last | Both leave a reported failure, and the target cleaned up | 1 |
| Write stream: destroyed without being closed | **Nothing is sent.** A truncated upload committed by a destructor is silent corruption | 1 |
| Buffered upload: the temporary file cannot be created, or fills up | The staged path's own failure modes | 1 |

#### 15. Backends, beyond the current conformance

Run for every backend, against the real servers.

| Scenario | What it proves | Tier |
|---|---|---|
| The full error-mapping table: missing, denied, not-a-directory, is-a-directory, exists, not-empty | Callers branch on these; a backend that reports `Unknown` breaks every one of them | 2 |
| Ranged reads at the start, the middle, the end, and past the end | The span loop rests entirely on this | 2 |
| A listing of 1,000, 1,001 and 10,000 entries in S3 | The pagination boundary, exactly | 2 |
| Empty directories, and S3 directory markers | A directory that exists only as a zero-byte key | 2 |
| Trailing slashes on files and directories, in every operation | The ambiguity that produced today's "not found" versus "not a directory" | 2 |
| Two operations on one drive from two threads | Every backend claims to tolerate this | 2 |
| Reported access versus what actually happens | The suite already checks a backend "either reports access properly or admits it cannot"; extend it to writes | 2 |
| Reconnection after an idle period longer than the server's timeout | A pooled connection that has quietly died | 2 |

#### 16. The framework underneath

| Scenario | What it proves | Tier |
|---|---|---|
| Progress never goes backwards, and never exceeds the total | The bar is not a lie | 1 |
| `bytesDone` equals the bytes actually transferred, per task | The number the arrival check compares against | 1 |
| Rate after a stall, and after a resume | No infinities, no negatives | 1 |
| Cancellation observed within a second from every task | The contract every task claims to honour | 1 |
| A task whose `run()` throws | The pool survives and the task is reported failed | 1 |
| Ten thousand metric updates a second | Coalescing keeps the event queue usable | 1 |
| A task outliving the filesystem it was given | Lifetime, held by `shared_ptr`, asserted rather than assumed | 1 |
| A queued task cancelled before it starts | It never starts | 1 |

#### 17. Running out of things

| Scenario | What it proves | Tier |
|---|---|---|
| The file descriptor limit reached mid-job | A clear error rather than an assortment of odd ones | 2 |
| The temporary directory full, unwritable, or missing | Staging is still used below the streaming threshold, and by every archive | 2 |
| A listing of a million entries | Memory, and whether anything streams it | 3 |
| More mounted drives than the pool was designed for | Handles per drive times drives | 2 |

#### 18. Today's five, permanently

Each fault found in one day becomes a named tier 1 test, because the cost of
finding it again is a day and the cost of keeping it is a second.

| Test | Guards |
|---|---|
| A transfer stopping short of an announced length is an error | `net::errorFor` |
| A read that fails is not an end of file | `TransferTask::copyStream` |
| A large SFTP read crosses the re-key point | The span loop |
| Listing a file says "not a directory" whichever way the server answers | `SftpFileSystem::list` |
| A forward seek within the buffer never depends on timing | `StreamingDownload::seek` |

#### Acceptance

Tier 1 in `make test` inside two minutes and under ThreadSanitizer inside ten.
Tier 2 in one command against the Proxmox machine, green, with every skip
accounted for. Tier 3 documented, reproducible, and run before any release.

The standing rule this project argued for now lives in
[CLAUDE.md](../../CLAUDE.md), where it governs every change rather than only
this one: a fault becomes a test first and is fixed second, the test goes in the
cheapest tier that can hold it, and a test for a race waits for a condition
rather than for a clock.

### Phase 6 — automation, if it earns it

Tier 1 is automatic from phase 2 simply by being in `make test`, and phase 5
fills it out. There is no CI in this repository at all today. A self-hosted runner on the same Proxmox could
run tier 2 nightly against the VM beside it. Left last deliberately: a
continuous pipeline around tests nobody trusts yet is effort spent in the wrong
order.

## Out of scope

- Testing the interface. The QML harness and the walkthrough already do that, and
  this project exists precisely because the faults were underneath it.
- A scenario language, a fuzzer, or generated test cases. Perhaps later; the
  catalogue above is written from faults actually seen, and that is a better
  source of scenarios than a random one.
- AWS. Rejected for now in favour of the Proxmox already in the room. If real
  provider latency turns out to matter, tier 3 is where it would go, and B2 is
  already reachable.

## Risks

- **A test environment that rots.** A VM nobody rebuilds becomes a machine with
  mysterious state that tests depend on. The provisioning script and the snapshot
  discipline are the answer, and they are part of phase 1 rather than an
  afterthought.
- **Slow tests nobody runs.** Everything that can be tier 1 must be tier 1. Every
  scenario that reaches for a server should be asked why.
- **Flaky concurrency tests.** The no-sleeps rule, and byte-triggered faults
  rather than time-triggered ones. A scenario that cannot be made deterministic
  belongs in tier 2 with an honest name, not in `make test` making noise.
- **Credentials.** Everything reaches the VM through the environment; nothing is
  committed; the account is throwaway and the machine is disposable.
