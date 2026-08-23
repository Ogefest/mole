# ADR-0081: A second distribution is built on a clock, and it is not a matrix

- **Date:** 2026-08-23
- **Status:** Accepted

## Context

Everything this project builds and tests is built against one distribution, one
Qt and one Arrow: the author's workstation, and the Ubuntu runner that mirrors it.
That went unremarked until MOLE-121 built an `.rpm` in a Fedora container, which
found two faults in one afternoon:

- the static libraries were not position-independent, which Fedora's linker
  refuses outright and Ubuntu's accepts, so nothing that ran here could fail;
- `ParquetTable` did not compile against any Arrow older than 21. This machine can
  only ever have 25 — no Ubuntu archive carries Arrow at all, so it is built from
  source here — and Fedora 40 carries 15. The Parquet grid had therefore never
  compiled anywhere but on the workstation it was written on.

Neither was found by a test. Both were found by somebody happening to build
somewhere else, and both had been in `main` for days.

The first run of the job written for this decision then found four more, which is
the part worth recording: `mole --version` said nothing at all on a machine with
no display, because it was answered after a `QGuiApplication` had been
constructed; natural ordering in a listing was byte ordering whenever the
environment named no language, because a `C` collator ignores numeric mode; a
suite case asserted an error code that depends on whether the host filesystem
keeps snapshots; and four cases that make a file unreadable and assert the failure
is recorded cannot fail for that reason when the account is root, which a
container job is.

## Decision

One second distribution, built and tested on a clock:
`.github/workflows/second-family.yml` configures, builds and runs the fast tier in
a `fedora:40` container, weekly, and on demand.

Fedora, because that is where both original faults surfaced, because
`scripts/package-rpm.sh` already pulls it, and because it packages an older Arrow
than this project can otherwise obtain — so a second Arrow is exercised for free.

The suite there runs as an unprivileged account, and nothing sets a locale.

**This is not a build matrix and is not to grow into one.**

## Reason

A matrix over distributions, Qt versions and compilers is the obvious answer and
the wrong size. What the two faults needed was *any* second environment; the third
adds cost linearly and finds almost nothing the second did not. Weekly rather than
per-push for the same reason: a container build with Arrow in it costs minutes and
a few hundred megabytes, and an API that moved waits harmlessly until Monday. What
matters is catching it in the week it lands rather than at a release.

The alternatives considered:

- **Per-push, one distribution.** Rejected: it triples the wall time of every
  push to catch a class of fault that appears a few times a year.
- **A matrix of distributions.** Rejected as above — a different decision with a
  different cost, and one to take when there is evidence a third environment finds
  something the second does not.
- **Only at release time.** That is what happened by accident, and it is how both
  faults reached `main` and sat there. A release is the worst moment to discover
  that a feature has never compiled anywhere else.
- **A pinned container image built here.** Rejected: pinning the image is pinning
  the very thing being tested. The point is to be told when the distribution moves.

Two details are deliberate, and both are about a check being able to fail for the
reason it exists:

- **An unprivileged account.** A container job is root, and root reads a file whose
  permissions have all been taken away — so every case that makes a file
  unreadable and asserts the failure is recorded passes vacuously, or worse, fails
  misleadingly. Those cases now skip when they cannot arrange what they need, and
  the account is what makes them run rather than skip.
- **No locale.** A container has no `LANG`, which is a real machine state — a cron
  job and a service started at boot are in it too — and natural ordering was wrong
  in exactly that state. Setting one in the job would have hidden the fault it
  found on its first run.

## Consequences

A second family is exercised without anybody remembering to, and an API that moves
under this project is found in the week it moves.

What it does not do is run the live or heavy tiers: whatever runs this cannot reach
the test environment those need, and adding one would be adding a tier that can
never pass. So the job answers "does it build and does the fast tier hold", and
nothing about behaviour against a real server.

The job is red when Fedora moves under us, which will sometimes be work nobody
planned. That is the cost of the decision and it is the point of it: the
alternative is the same work at a release, with a tag half cut.

A weekly job also means a fault can be four days old before anything says so.
Anybody touching an optional-library path can ask for the answer immediately with
`workflow_dispatch` rather than waiting for Monday.
