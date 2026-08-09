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
fakes, plus the first catalogue of hostile scenarios in tier 1.

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

### Phase 5 — automation, if it earns it

Tier 1 is automatic from phase 2 simply by being in `make test`. There is no CI
in this repository at all today. A self-hosted runner on the same Proxmox could
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
