# ADR-0026: Faults are injected by one wrapper, and always at a byte offset

- **Date:** 2026-08-10
- **Status:** Accepted

## Context

Three fakes were written in a single day, each to reproduce one fault found in a
real transfer: `CommitFailingFileSystem` for a remote write that only fails when
it is closed, `HalfReadingFileSystem` for a connection that dies mid-file, and
`ForgetfulFileSystem` for a server that acknowledges bytes and stores fewer.
Each was about forty lines of delegating `IFileSystem` around one line that did
the misbehaving, and all three sat in `tests/core/tst_TransferTask.cpp`. A
fourth fault would have been written the same way, and a fifth after that.

The faults worth testing are not exotic. They are what a network does on an
ordinary day: a read that stops, a read that returns less than it was asked for,
a write that is accepted and lost, a file that changes under the reader, a
destination that fills up, a listing that overstates a size. Everything from
phase 4 (scale and interference) and phase 5 (the catalogue) of the testing work
is a combination of those, so how they are produced is a decision that the rest
of the suite inherits.

## Decision

**One decorator, `tests/support/FaultyFileSystem`, wraps any `IFileSystem` and
misbehaves on purpose.** It is the shape `LoggingFileSystem` already uses for
every real mount: the delegation is written down once, so a fault works over
memory, local disk, SFTP and anything a plugin brings.

**Every fault fires at a byte offset.** The wrapper counts the bytes through each
stream and acts when the count arrives, and it clamps each read and write so the
count lands exactly on the offset — a fault declared at byte 1200 happens at byte
1200 whatever chunk size the caller uses.

**The one fault that cannot be an offset is a stall**, because a stall is the
absence of an event. It is still not a clock: the stream stops at its offset and
stays stopped until the test calls `release()`, and the test waits for
`isStalled()` rather than for a duration.

**Each open stream takes its own copy of the faults that apply to it**, so ten
concurrent copies of one file each drop at 30% rather than one of them consuming
the fault and nine quietly succeeding.

The three hand-rolled fakes are gone, and the four tests that used them now run
through the wrapper.

## Reason

**Why an offset rather than a duration.** "The connection drops after 30% of the
file" is a fact about the transfer. "The connection drops after 200 ms" is a fact
about the machine the test happens to run on: it passes on a laptop and fails on
a loaded build server, or worse, passes on both for different reasons. An
intermittent test is worse than no test, because it teaches everyone to ignore
red.

**Why a decorator rather than knobs on `MemoryFileSystem`.** The memory backend
already has `setFault()`, and extending it was the cheaper-looking option. But it
is a real backend the user can mount, its faults are per-path rather than
per-byte, and a fault expressed there can only ever be tested against memory —
while the faults being reproduced were found against SFTP, S3 and WebDAV. The
whole value of a wrapper is that the same declaration can be put in front of a
live drive when one is available.

**Why not a mock framework.** Nothing here needs one. `IFileSystem` is a dozen
methods over `Result<T>`, the interesting behaviour lives in the stream devices
rather than in the calls, and a generated mock cannot express "at byte 1200" —
which is the whole point.

**Why the wrapper has its own test.** It is a test tool, and one that miscounts
bytes would make every suite built on it green for the wrong reason.
`tst_FaultyFileSystem` holds each fault to the one thing it promises: that it
happens at the byte it was given, whatever chunk size the reader uses.

## Consequences

- A new fault is a method on one class, not a new fake. The ten that exist cover
  what has actually been seen; adding the eleventh is a few lines with the
  delegation already written.
- Phase 4 and phase 5 are written against this wrapper rather than against a
  fixture each, so a scenario reads as what goes wrong rather than as scaffolding.
- The wrapper can be put in front of a live backend as well as in front of the
  memory one. A fault found against a real server leaves behind a test that does
  not need a real server, which is the rule in CLAUDE.md; keeping the same
  declaration usable against the server is what makes writing both cheap.
- Faults are declared before the transfer starts and are not changed while it
  runs. A test that needs to interfere part way through does it from an action
  attached to an offset, or while a stall holds the stream still.
