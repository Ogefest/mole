# ADR-0095: No one drive may hold more than half the pool, and quitting does not wait for ever

- **Date:** 2026-09-04
- **Status:** Accepted

## Context

`TaskManager` runs every task on one `QThreadPool` of `qBound(2, cores - 2, 8)`
threads. ADR-0018 put storage there so the drawing thread never touches it, and
[ARCHITECTURE.md](../../ARCHITECTURE.md) names a stalled NFS mount as the reason
for the design.

The design moved the freeze rather than removing it. An `IFileSystem` call on a
mount that has stopped answering — an NFS server switched off, a yanked USB disk,
a stuck SMB share — blocks *in the kernel*: `QStorageInfo`, `QDirIterator`,
`QFile::open`. The cooperative `CancelToken` cannot reach a thread that is inside
such a call, so each one holds a pool thread for as long as the kernel takes,
which is `tcp_retries2` — about fifteen minutes for a hard NFS mount — or for
ever.

Two things then filled the pool without anybody doing anything.
`DriveListModel::refreshSpace()` submitted a `QuerySpaceTask` per mounted drive
on every `mountsChanged` **and** every minute from its timer, with no guard
against the previous answer still being out. Opening a `.zip` adds an unlisted
mount, which reloads the list; pruning it reloads again. So one gesture was two
rounds of queries at every disk in the sidebar, and the minute timer added one
more per dead mount for ever. Eight of those and the pool was gone in under ten
minutes, with every listing, copy, preview and search on every *other* drive
queued behind them. The window kept painting — the rule held — and the
application had stopped.

`Task::execute()` went straight to `run()`, so cancelling the queue did not help
either: each queued task still entered `run()` and made its blocking call before
its first poll. And `~TaskManager()` was `cancelAll(); m_pool->waitForDone();`
with no bound, so quitting with a task inside such a call hung the process with
the window already gone — which is what makes people reach for `kill -9`, the one
thing ADR-0020 says leaves wreckage behind.

`setBackground(true)` was not an answer: it keeps a job out of the task strip and
out of the log, and has never had anything to do with the pool.

## Decision

**Each task declares the drive it runs on, and no drive may hold more than half
the pool.**

`Task::noteRunsOn(fileSystem)` records the backend pointer as an opaque key —
never dereferenced — and `Task::lane()` hands it back. This is a second question
from `touching()`, which exists to light a dot in the sidebar and is deliberately
silent for the crowd: a listing, a space query, a ranged read and a thumbnail
declare nothing there, and those are precisely the tasks whose lane matters.

`TaskManager::submit()` starts a task on the pool when its lane has room, and
queues it in the lane otherwise; a task finishing drains its lane.
`perDriveLimit()` is `max(1, maxThreadCount / 2)` — *half*, because the point is
that something else can always start, and *at least one*, because a pool of two
still has to make progress. A task with no lane is not bounded: it is not a call
into a backend.

Three smaller decisions follow from the same problem:

- `Task::execute()` answers a cancel that arrived while the task was queued,
  before `run()`. The token is cooperative and this is the first place it can be
  co-operated with.
- `DriveListModel` keeps at most one space query per mount outstanding, and a
  reload asks only about mounts nothing has measured. The minute timer still asks
  about everything.
- `~TaskManager()` waits `kQuitGraceMs` (three seconds). Past that it names the
  tasks still in a call, takes them and the pool out of the child list, and lets
  the process end. They are leaked on purpose.

## Reason

**Why not a second pool for background work**, which was the other suggestion on
the ticket: it would keep a copy from queueing behind a space query, and it would
not stop a folder of eight dead-mount listings taking every thread in *its* pool.
The unit that has to be bounded is the drive, not the kind of job — and once the
drive is bounded, a second pool buys nothing that is worth two more knobs to
tune. The two pools would also have to be sized against each other, and getting
that wrong is a new way to starve one kind of work.

**Why the backend pointer and not the mount id.** A task holds a `FileSystemPtr`
and does not know which mount it came from; the mount table can change under it,
and the same backend can be mounted twice. The pointer is what identifies the
thing that will block.

**Why one lane per task and not one per drive it touches.** A copy and a sync work
on two drives. Acquiring both lanes is a lock-ordering problem — two transfers in
opposite directions can each hold one and wait for the other — and it buys
nothing: taking one slot per task is already enough to stop a dead mount filling
the pool. So a transfer, a sync and a compress name their source.

**Why some tasks name nothing.** `RenameTask`, `FindDuplicatesTask` and
`VerifySetTask` take a `VfsManager` and resolve a drive per uri; there is no one
drive to name and choosing one would be arbitrary. They stay unbounded, and
`tst_TaskManager::everyTaskHandedADriveDeclaresIt()` names them explicitly so the
exemption is a list somebody can argue with rather than an oversight — and so a
task written next year that forgets `noteRunsOn()` fails a test rather than
quietly going unbounded.

**Why leaking at quit rather than waiting.** A leak at process exit costs nothing:
the kernel reclaims everything a moment later. A hang costs the session and
teaches the user to `kill -9`, which is worse than either. What must not happen is
deleting a task while a pool thread is inside its `run()`, so both the tasks and
the pool leave the child list before the destructor's own children are deleted.

**Why half the pool rather than one task per drive.** Sequential per drive would
serialise a copy's read against a listing on the same drive, so browsing would
crawl during a transfer — a real cost paid every day to bound a fault that happens
rarely. Half the pool leaves a healthy drive its parallelism and still guarantees
a free thread.

## Consequences

- A dead mount now costs at most half the pool, whatever is queued against it, and
  the other half keeps answering. That is the whole claim, and
  `oneDriveThatStoppedAnsweringDoesNotHoldEveryThread()` is it in a test.
- Eight simultaneous reads of one drive now run four at a time on an eight-thread
  pool. For a local disk that is no slower — the disk was the bound, not the pool
  — and for a remote drive it is fewer connections, which
  [ADR-0013](0013-a-transfer-is-bounded-by-a-budget-not-by-a-timeout.md) already
  prefers.
- Adding a `Task` subclass that takes a `FileSystemPtr` means calling
  `noteRunsOn()`. The test says so.
- `MemoryFileSystem` grew `setListGate()`: a listing held until the test releases
  it, because "a drive that has stopped answering" is a call that has not come
  back and no duration stands in for that.
- Quitting is bounded at three seconds. A task that ignores its token for longer
  than that is now a warning in the log naming it, which is a bug report somebody
  can act on rather than a process that would not die.
- What is *not* fixed: a blocking call still cannot be interrupted, so a dead
  mount still shows a spinner that never resolves and a `Ctrl-C` on it still does
  nothing until the kernel gives up. Bounding what it costs everything else is a
  different thing from curing it, and curing it means a watchdog thread and
  timeouts per backend call — a much larger change, and one that would need this
  one underneath it anyway.
