# ADR-0046: A duplicate is proved by comparison, not by a digest, and the reads are overlapped

Date: 2026-08-19

Status: accepted

## Context

Finding duplicates by content ran three stages: group by size, group by a
SHA-256 of the first megabyte, then group by a SHA-256 of the whole file. Files
sharing that last hash were declared identical, reported as a group, and the next
thing that happens to a group is that all but one of it is deleted.

The whole scan ran on **one** thread — the pool thread the task was handed to —
with no parallelism inside it at all. Measured on the machine this was written
on, a twenty-core desktop:

| | one thread |
|---|---|
| `QCryptographicHash` SHA-256, which is what it used | 218 MB/s |
| OpenSSL SHA-256, the same processor's SHA-NI instructions | 2 237 MB/s |
| XXH3-128 | 24 096 MB/s |
| `memcmp` | 86 957 MB/s |

Qt 6.4 carries its own SHA-2 and does not use the processor's instructions for
it, so a scan was capped at 218 MB/s *whatever the storage was*. Every SSD is
faster than that. The scan was waiting for one core.

## Decision

**Three changes, in the two places where the two questions are different.**

The stages answer different questions and only one of them has a safety
requirement, which is what decides the hash for each:

- **The head stage is a filter.** Its only job is to decide who goes on to the
  expensive stage. A collision costs one extra file read there, and that stage
  then separates the files; no false group can survive it. So it uses **XXH3-128**
  where the build has libxxhash, falling back to SHA-256 where it does not — a
  filter only has to be consistent.
- **The last stage is a verdict.** It uses **no hash at all**: the files that are
  left are compared with one another, byte for byte, streamed in lockstep.
- **The reads are overlapped.** A stage that opens files hands them to a pool of
  the task's own, several at a time.

## Reason

**Why not simply a faster hash for the last stage too.** Because of what the
answer is used for. A hash makes "these files are identical" a statement about
probability. SHA-256 makes that probability negligible and is also the slowest
thing in the scan. The fast hashes are fast precisely because they are not
cryptographic: XXH3 collisions can be *constructed*, cheaply, by whoever wrote the
files — and a file manager scans folders whose contents came from downloads,
shared drives and synchronisation. "Unlikely to collide by accident" is not the
property needed by a step whose output is a deletion.

**Comparison is the faster option as well as the exact one**, which is what makes
this an easy decision rather than a trade:

- Each file is read exactly once, as it is with a hash. Files are opened together
  and read in lockstep; a class splits the moment two of them differ.
- The work per byte is a `memcmp` rather than a digest — two orders of magnitude
  cheaper, so the scan waits for storage instead of for a core.
- Files that differ stop at the first chunk that differs. A hash always reads to
  the end of both.

**Why the head stage survives at all.** A comparison needs the files open
together; a digest does not. Grouping ten thousand same-sized files by a cheap
digest, one at a time, costs one descriptor and one megabyte read apiece, and
leaves the comparison with the handful that agreed. Dropping the head stage and
comparing every same-sized file would mean opening far more at once for far
longer.

**Why the task gets its own pool.** The scan is already running on a thread of
`TaskManager`'s pool. Queueing work onto that same pool and then waiting for it is
how a pool deadlocks. The pool is a local in `run()`, so its destructor is what
guarantees no worker outlives the scan.

**Why only the reads are overlapped.** Results are taken back in the order the
work went out — `QFuture::resultAt(i)` rather than waiting for all of them — so
the grouping, the ordering of the results, the announcement of each group and
every call to `Task`'s reporting helpers stay on the one thread `run()` was called
on. A scan on eight threads therefore produces the same groups in the same order
as a scan on one, ADR-0043's progressive reporting is untouched, and there is not
a lock anywhere in the task. The alternative — confirming groups from the worker
threads — buys nothing measurable and costs a mutex around the results list, an
ordering hazard in the `groupFound` signal, and a much harder thing to reason
about.

## Consequences

- **Memory is bounded and does not grow with the file.** Only the chunk being
  compared is held — 256 kB per file being read — so a group of hundred-gigabyte
  disk images costs what a group of documents costs. This is asserted directly:
  a test watches the drive and fails if any read is larger than a chunk.
- **Memory is bounded and does not grow with the bucket either.** At most sixteen
  files are read in lockstep. A larger bucket is compared in slices and the
  slices are joined by comparing one file from each — so a folder with ten
  thousand copies of one photograph cannot exhaust memory or descriptors. Slices
  keep their lone files rather than discarding them, because a file alone in its
  slice is very often a match for a class in another one.
- **libxxhash becomes an optional dependency**, BSD-2-Clause, dynamically linked
  like every other one. A build without it is correct and slower at one stage.
- **`IDuplicateStrategy` gains a second shape.** A stage says through
  `stageComparesContent()` whether it settles a bucket by keying each file or by
  comparing the files with one another, and gets `keyFor()` or `compare()`
  accordingly. A strategy added later — perceptual image hashing, tag comparison
  — picks whichever fits.
- **Measured end to end**, on 1.9 GB of duplicates warm in the page cache:
  9 330 ms before, 520 ms after.
- Sync still compares two files by hashing both (`SyncPlan.cpp`). The same
  argument applies there and it is not this change.
- **ThreadSanitizer has more to say about this suite than it did**: 18 warnings
  before, 55 after. None of the new ones are on this code's own data -- they are
  inside `QtConcurrent::ThreadEngineBase`'s throttling, and inside glibc's
  timezone cache reached through `QFileInfo::lastModified()` when `stat()` is
  called from several threads at once. It is more for MOLE-126 to work through,
  and it is recorded here rather than left to be discovered.
