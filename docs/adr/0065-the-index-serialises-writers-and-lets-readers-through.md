# ADR-0065: The index serialises writers and lets readers through

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

`IndexDatabase` opens its database in WAL mode *so that a scan can write while the
interface reads* — the comment saying so is at `IndexDatabase.cpp:56` — and then puts
every public method behind one `QMutex`, which hands that concurrency straight back.
Sixteen `QMutexLocker` sites, one at the top of every public method.

The lock has a second job, and that is what makes this a decision rather than a
deletion. `m_connections` and `m_nextConnection` are mutable members that
`connectionForCurrentThread()` writes, and its declaration says *callers must hold
`m_mutex`*. That registry does need a lock — for the microseconds a `QHash` lookup
takes, not for the length of a transaction. So one lock guards a hash on every call
*and* serialises SQL that SQLite was configured to serialise itself.

**What the interface meets is starvation, not one long hold.** It is worth being
precise, because an earlier account of this — MOLE-264's first draft and the commit
message of `486f099`, both mine — said the scan holds the lock across the walk, and
that is wrong. `carryForward()` takes the lock at `IndexDatabase.cpp:431` and is called
from inside the walk lambda at `ScanTask.cpp:114`, once per unchanged folder, right
after a batch flush that takes it again. Over 77,415 folders that is tens of thousands
of acquisitions, each around a short transaction. `QMutex` is not fair, so a thread
that releases and immediately reacquires can leave a waiter untouched: measured, the
main thread sat at **0% CPU for sixty seconds** while the scan thread ran at 99.9% and
the window never appeared at all.

The reproduction behind that is an index of 723,405 files in 77,415 folders, 734 MB,
with a 152 MB write-ahead log.

## Decision

**Split the one lock in two, and let readers take neither.**

- **The connection registry gets its own mutex**, held for the hash lookup and the
  connection setup and nothing else. `m_open` becomes `std::atomic<bool>`, because it
  is the one piece of state a reader looks at that the lock used to cover.
- **Writers keep a mutex of their own**, serialising Mole's own writers against each
  other exactly as today: `open`, `close`, `applyMigrations`, `upsertVolume`,
  `removeVolume`, `beginScan`, `carryForward`, `commitScan`, `abandonScan`,
  `insertBatch`.
- **Readers take no lock for their SQL**: `isOpen`, `directoryTimes`, `volumes`,
  `search`, `factKeys`, `fileCount`. WAL gives each one a consistent snapshot without
  asking anybody's permission, which is the whole reason it is turned on.

**What follows is two tickets and not an epic.** One is new and is this split: once the
sixteen sites are classified — and the list above is the classification — the change is
mechanical, and its substance is a test that reaches a scale where the fault exists at
all, which belongs with it. The other already exists and is unchanged in scope,
MOLE-264, for the seven reads the interface still makes on the thread that draws it.
They are independent: either can land first and neither needs the other to be useful.
Two small independent tickets is a loose end apiece, not an effort that needs a name.

## Reason

**Why not delete the lock entirely.** Letting writers run unlocked is the textbook WAL
answer and it moves a failure out of *slow* and into *an error somebody has to
handle*: two writers meet `SQLITE_BUSY` after `busy_timeout`, and `commitScan()` runs
one transaction for a whole generation swap, so the loser of that race is a caller
with a new error path and nothing written to handle it. Keeping writers serialised
costs nothing we are currently getting — they are serialised today — and buys the
whole fix, because **the starvation is readers waiting behind writers, not writers
waiting behind each other.**

**Why not a reader-writer lock.** It keeps the queue. A reader still waits for an
in-flight write, `QReadWriteLock` is no fairer than `QMutex`, and the measured symptom
is precisely a stream of short writes never letting a waiter through. It is a smaller
change that might not fix the thing it is for.

**Why not only move the question off the drawing thread.** That is worth doing and it
is MOLE-264; it is not a substitute. It would leave the answer sixty seconds late
rather than absent, and it fixes only the call sites somebody converts — and
`volumes()` is not the only synchronous question the interface asks. There are seven:
`volumes()` from `IndexesFeature.cpp:218`, `BrowserFeature.cpp:204` and
`SearchFeatures.cpp:127`, `:296`, `:335`, and `factKeys()` from `SearchFeatures.cpp:809`
and `:816`. The other two readers are already on workers — `search()` from
`IndexSearchTask` and `directoryTimes()` from `ScanTask`.

## Consequences

- A read no longer waits behind a scan. It costs its own query, which for `volumes()`
  is a select over a handful of rows.
- **It is still I/O on the thread that draws the window**, and that is deliberately not
  fixed here. MOLE-264 stays open and can be rewritten from this record without a
  second decision: the seven call sites are named above, and after this change each one
  costs a query rather than a queue — which turns a freeze into a stutter, not into
  nothing.
- **A reader sees the snapshot its transaction opened with.** A read that overlaps a
  scan's commit reports the state before it. That is already true today — the lock
  serialises access but orders nothing — and it is what the interface already assumes
  every time it says how old an index is.
- **Checkpointing is the thing to watch.** WAL is checkpointed when no reader is
  holding an old snapshot, so more overlap between readers and writers means more
  chances to defer it. The 152 MB write-ahead log on the reproduction machine says the
  WAL already grows faster than it is collected, and this makes that more likely rather
  than less. If it becomes a problem the answer is an explicit
  `wal_checkpoint(TRUNCATE)` at a quiet moment, not a return to one lock.
- No new error path for any caller, which is the point of keeping the writer mutex.
- `make tsan` builds the core suites against an instrumented Qt (MOLE-234), so the
  split can be checked rather than argued about — and it needs to be, since the point
  of it is that two threads now touch the database at once.
- **A test of this has to reach scale or it asserts nothing.** Seed through
  `insertBatch` in batches rather than walking a real tree: several hundred thousand
  rows is seconds. Then the claim is a condition and not a clock — *a read returns
  while the writer is still working*, with the writer given far more to do than the
  reader. Against today's lock that fails; the measured version of it is a window that
  never appeared in sixty seconds.

## No behaviour change here

This record decides a shape. Nothing in `src/` changes with it and there is no
changelog line; the ticket that follows earns those.
