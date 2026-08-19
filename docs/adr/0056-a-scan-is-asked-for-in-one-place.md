# ADR-0056: A scan is asked for in one place, and the readers that complete one live in the sdk

- **Date:** 2026-08-19
- **Status:** Accepted

## Context

A scan of a tree can be asked for three different things beyond walking it:
keep what has not changed, record what each file says about itself, and record
what lives inside a file that is a container. `ScanTask` took the first as
`setIncremental(bool)` and the other two as two callbacks the caller had to
build, and the code that built them — `factReaderFor()` and
`containerReaderFor()` — was private to `LiveSearchController`.

So of the four callers of `ScanTask`, exactly one could ask for a complete scan.
The one that mattered most could not: `IndexScanJob`, the nightly re-index,
called `setIncremental()` and nothing else. A tree indexed with metadata and
then put on the clock lost it a subtree at a time — every directory the
incremental scan re-walked was rewritten with rows carrying no facts, while the
subtrees it carried forward kept theirs. Nothing said so. The scan succeeded,
the run log recorded a success, the row count barely moved, and the only symptom
was a search that used to find something and quietly no longer did. The feature
got worse the longer it was left running.

The rule could not carry the answer either: `scheduleScan()` wrote only the
folder and the incremental flag, and `IndexScanJob::metadataParameter()` was
declared with a comment saying what it was for and was written by nobody and
read by nobody.

## Decision

Two things, and which layer owns each is the decision.

**A scan's options are one value in `core`.** `ScanOptions` — `incremental`,
`metadata`, `archives` — and `ScanTask::setOptions()` where `setIncremental()`
used to be. Three booleans and no more: `core` links QtCore and QtSql only, and
these are what a scan *was asked for* rather than how it does it.

**The two reader factories are free functions in `sdk`**, over `PluginServices`:
`factReaderFor()`, `containerReaderFor()`, and `applyScanOptions()`, which
applies a `ScanOptions` to a `ScanTask` using them. `sdk` sits above `core` and
below `host`, `ui` and `builtin`, so the search form, the nightly re-index and
the browser can all reach the same three lines.

A `ScheduleRule` for the index job then carries all three options, and
`IndexScanJob::optionsFor()` reads them back, so a nightly run repeats the scan
that created it.

## Reason

The readers moved rather than being redesigned, because neither used anything
that is not already in `PluginServices`: `containerReaderFor()` reads
`services.vfs->factories()` and `factReaderFor()` reads `services.metadata`.
Making them free functions in `sdk` is the smallest change that lets more than
one caller build the same scan.

The alternatives, and what disqualified them:

- **Leave the readers in `LiveSearchController` and have `IndexScanJob` ask it
  for them.** A scheduled job would then depend on a tab being constructible,
  which is backwards — the whole point of the job is that it runs with no tab
  open — and it puts a `builtin`-to-`builtin` edge where there was none.
- **Put the readers in `core`, beside `ScanTask`.** `core` cannot see a metadata
  reader or a filesystem factory registry without pulling the plugin API down
  into it, which is the layering the project has held to from the start.
- **Have `ScanTask` build the readers itself from a `ScanOptions`.** Same
  problem: it would make the class that walks a tree depend on the extension
  points. Keeping the options a request and the readers a mechanism is what
  lets `core` state the intent without naming the machinery.
- **Keep three setters and fix `IndexScanJob` alone.** That fixes today's bug
  and leaves the shape that produced it: a caller can still set two of three
  and be quietly wrong, with no compiler and no test to say so.

## Consequences

Anybody with `PluginServices` to hand builds a complete scan in one line, and
adding a fourth thing a scan can be asked for is a field on `ScanOptions` and a
branch in `applyScanOptions()` rather than a new setter every caller has to
learn about.

`ScanOptions` records intent and the readers do the work, so the two can
disagree: a task with `options.metadata` set and no fact reader installed
records no facts. `applyScanOptions()` is the only thing that should set both,
and every caller with services uses it.

The CLI's `scan` (`src/tools`) takes `ScanOptions` and gains an `--incremental`
flag, and still installs no readers — `src/tools` includes nothing from `sdk`
today. Giving the command line metadata and archive contents is a separate
decision nobody has asked for yet.
