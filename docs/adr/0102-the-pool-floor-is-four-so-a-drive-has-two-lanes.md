# ADR-0102: The pool's floor is four, so a drive has two lanes on any machine

- **Date:** 2026-11-03
- **Status:** Accepted. Amends [ADR-0095](0095-no-one-drive-may-hold-more-than-half-the-pool.md)
  on one point, the size of the pool on a small machine; everything else in it stands.

## Context

`TaskManager` sized its pool `qBound(2, cores - 2, 8)`, and ADR-0095 lets one drive
hold at most half of it. So a two-core or four-core machine had a pool of two and
**one lane per drive**: one task at a time on any one disk, share or stick. Four-core
laptops are ordinary hardware, and the hosted runner is one.

MOLE-425 found four suites red in exactly that configuration and fixed the two that
were the product: a listing queued behind the query it was meant to cancel, so
neither ran, and a probe whose answer arrived after the cursor and was never
applied. It deliberately did not decide whether one lane per drive was a
configuration Mole meant to support, because raising the floor would have hidden
both faults rather than fixing them. With them fixed, the question could be asked
on its own terms. That is MOLE-426.

## Decision

**The floor is four: `qBound(4, cores - 2, 8)`.** Every machine gets at least two
lanes per drive. Above six cores nothing changes.

`MOLE_TASK_THREADS` in the environment overrides the size, and the suites that
found MOLE-425's races run a second time in the fast tier with it set to two, so
the one-lane configuration is still exercised on every change:
`tst_BrowserPaneControllerOneLane`, `tst_KeyboardNavigationOneLane`,
`tst_PreviewOneLane` and `tst_WalkthroughOneLane`.

## Reason

### What one lane costs, measured

One lane costs nothing until something occupies it, and then it costs everything
on that drive. A copy, a hash, an archive, a scan: each holds its lane for as long
as it runs. With one lane, every listing, preview and thumbnail of that drive
queues behind it. Copy ten gigabytes off a USB stick on a four-core laptop, and
opening a folder on the stick waits for the whole copy to finish.
`tst_TaskManager::aLongTaskOnADriveLeavesRoomToBrowseIt` is that case. With the
floor at two it fails: the listing does not start while the copy is held. With
four it passes. This is a fact about the arithmetic, not a timing, and it was the
deciding measurement.

ADR-0095 already rejected one task per drive for this reason, on a large machine
("browsing would crawl during a transfer"). The old floor quietly brought that
back on every small one.

### What a floor of four costs, measured

The reason for `cores - 2` was to leave the window's thread room to draw. So the
cost to measure was CPU contention with the window's thread, at the worst a task
can do, which is spinning. A main-thread timer every 10 ms did 4 ms of CPU work,
timed on the wall, while N pool threads span. The process was pinned with
`taskset` to stand in for a smaller machine. Linux 6.5, 2026-11-03:

| cores | busy pool threads | 4 ms of window work took (median / p99 / max) |
|---|---|---|
| 4 | 0, 2, 4 or 8 | 4 / 4 / 4 ms |
| 2 | 0, 2 or 4 | 4 / 4 / 4 ms |
| 2 | 8 | 4 / 44 / 52 ms |

The scheduler favours a thread that mostly sleeps, which is what a window's
thread is. Four busy threads on four cores, or on two, cost it nothing measurable.
Only eight on two cores did, and the ceiling `cores - 2, at most 8` never gives
two cores eight threads. Most tasks are also not spinning but waiting on a disk
or a socket, which costs a core nothing.

And the suite, the way MOLE-425 reproduced the runner
(`taskset -c 0,1 ctest --parallel 4 --label-exclude heavy`):

| floor | result | time |
|---|---|---|
| 2 (before) | 145 of 145 | 119 s |
| 4 (after) | 149 of 149, the four one-lane runs included | 183 s |

So the faults MOLE-425 fixed stay fixed at a floor of two, and the higher floor
breaks nothing on two cores.

### The alternatives

- **Keep the floor at two, and hold the invariant MOLE-425's second fault broke:
  that no operation waits on another task of the same drive.** This is the card's
  other branch, and it answers a different question. The invariant is about
  deadlock, and it is worth having, but it cannot make one lane safe. A listing
  queued behind a ten-minute copy is not waiting on anything wrong; it is waiting
  for its turn, and with one lane its turn comes after the copy. No test can make
  that acceptable. It is the configuration itself.
- **A per-drive limit of at least two, on a pool of two.** One dead drive could
  then hold the whole pool, which is exactly the freeze ADR-0095 exists to
  prevent: everything on every other drive queued behind a mount that stopped
  answering.
- **A separate lane for interactive work** (listings, previews) beside the
  long-running one. ADR-0095 rejected a second pool as two knobs to tune against
  each other. A lane by kind of task is the same idea one level down, and it
  needs every task classified correctly to work. A larger pool gets the same
  effect from arithmetic that already exists.
- **Raise the floor and stop running the one-lane configuration.** Rejected
  because of how MOLE-425 was found: by a machine that happened to be small. A
  floor that hides a configuration stops anyone noticing the next fault in it.
  The one-lane runs keep it in the fast tier.

## Consequences

- On a machine with four cores or fewer, a drive that is busy with something long
  can still be browsed. That is the one user-visible change, and it has a line in
  `CHANGELOG.md`.
- A two-core machine runs four pool threads. Measured above as costing the window
  nothing. Most of them wait on I/O anyway.
- A dead drive on a small machine now holds two threads rather than one, out of
  four rather than two. ADR-0095's guarantee is unchanged: half the pool is
  always free.
- `taskset -c 0,1` alone no longer reproduces one lane per drive, because the
  floor applies however few cores there are. `MOLE_TASK_THREADS=2` does, and
  ARCHITECTURE.md says so where it tells a reader how to be a small machine.
- Four suites run twice in the fast tier. On two cores that adds about a minute;
  on a workstation, seconds.
