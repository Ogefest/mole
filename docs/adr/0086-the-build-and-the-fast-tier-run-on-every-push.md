# ADR-0086: The build and the fast tier run on every push

- **Date:** 2026-09-01
- **Status:** Accepted

## Context

`.github/workflows/` held four files and not one of them fired on a push.
`release.yml` runs on a tag, `second-family.yml` on a Monday clock, and
`windows.yml` and `macos.yml` only when somebody asks. So a commit that broke the
build or the fast tier was found in one of two ways: by the weekly Fedora job, up
to seven days later, or by whoever next pushed a tag.

The second is the one worth designing against. `scripts/release.sh` runs the whole
local gate — the suite and both live tiers — before it commits, tags and pushes, so
by the time a tag reaches GitHub somebody is mid-release. A red run at that moment
is a red run against a version number already spent and a tag that cannot be taken
back.

This is also what an unverified dependency bump was waiting for. `ee918a5` moved
`actions/checkout` from v4 to v7 in all four workflows and said so in writing: the
bump was recorded as unverified, "on a branch whose next push will exercise it".
No push exercised anything, so that sentence was not true — the earliest
confirmation available was a Monday, or a release.

## Decision

**A fifth workflow, `fast-tier.yml`: configure, build and the fast tier, on every
push and every pull request, on `ubuntu-24.04`.**

- **It builds against the same libraries `release.yml` does**, Arrow from Apache's
  own repository included, minus the two packages that only unpack an `.rpm`.
- **It publishes nothing.** `permissions: contents: read`, no tag, no release, no
  artefact.
- **It prints the configure summary**, the way `second-family.yml` and
  `windows.yml` do.
- **The live and heavy tiers are not in it**, and `--label-exclude heavy` is what
  makes the run the fast tier rather than all of it.
- Superseded runs are cancelled on a branch and never on `main`.

`tests/scripts/tst_Workflows.sh` holds all of that, including the library lists of
the two jobs against each other.

## Reason

**A fifth file rather than a second trigger on `release.yml`.** That job already
configures, builds and runs the fast tier on this same runner, so the cheap change
was one more `on:` entry. It was rejected because of what a reader would then have
to do: work out whether a push publishes a release, and arrive at "no, because of a
guard eleven steps down". A workflow named *Release* that runs on every push is a
thing to reason about where there should be nothing to reason about, and the
project already keeps that boundary sharp — `release.sh` is deliberately the only
thing in the repository that makes a tag, for the same class of reason.

**Not a reusable workflow either, and not yet.** Factoring `release.yml`'s setup
into something both call is the answer that removes the copy, and it means editing
the file whose first real run is still ahead of it. Restructuring the workflow that
publishes, in aid of a workflow that only tests, is the wrong order to take those
two risks in. What keeps the copy honest in the meantime is a test rather than
care: the two package lists are compared, and a library added to one has to be a
decision about the other.

**The same libraries as the release, and this is the decision most likely to look
wrong.** The ticket observed, correctly, that the job needs nothing but a compiler
and Qt 6.4: every optional dependency is found `QUIET` behind a `MOLE_HAVE_*` flag,
and `src/core/CMakeLists.txt` says outright that a missing one must never stop the
build. A lean runner would be faster and would exercise the no-optional-library
configuration, which is a shipped one — the `.deb` has no Arrow in it.

It was rejected because **a missing optional library does not go red, it goes
quiet.** The suite builds smaller, the absent features' cases skip, and the run is
green. A push job that tests a leaner machine than the release builds on therefore
misses exactly the breakages a release would meet, while looking like it is
watching. Arrow is the sharpest case: it is in no Ubuntu archive at any version, so
without Apache's repository the Parquet suites would never run on a push at all,
and Arrow is where two of the four faults in ADR-0081 lived. The configure summary
is printed anyway, because "what this runner did not have" is the first thing worth
knowing about a red run — not because anything is expected to be absent.

**`ubuntu-24.04` and not `ubuntu-latest`.** 24.04 ships Qt 6.4.2, which is the
version `CMakeLists.txt` requires and the one the author develops against. A
floating runner label would move the Qt underneath this job on GitHub's schedule
and would quietly stop answering the question it exists for.

**Not a matrix.** ADR-0081 carries that argument and it applies here unchanged: one
second family, on a clock, is what catches the faults a second distribution
catches. Growing this job sideways is a different decision with a different cost.

**Not the live or heavy tiers.** MOLE-119 settled that the release gate is the only
place they will ever be a precondition of anything, because whatever runs on a
hosted runner cannot reach the test environment. A tier added here is a tier that
can never pass, which is worse than no tier: it teaches everyone to ignore red.

**Push and pull request both.** The author's own history goes straight to `main`, so
`push` is what covers the ordinary case, and a branch nobody has opened a pull
request for still has to build. `pull_request` is what covers a fork, which is the
only way somebody outside this network can contribute at all — the GitHub Issues
tab is open for the same reason. A branch in this repository with an open pull
request is checked twice as a result, once as it stands and once as it would merge;
that is the accepted cost, and the concurrency group keeps it from stacking.

## Consequences

- **A break is found by whoever caused it**, which is the whole of it.
- **`actions/checkout@v7` is exercised on every push**, so the bump `ee918a5`
  recorded as unverified is verified by the ordinary course of work rather than by
  a release.
- **Every push spends runner minutes**, and this is the most expensive workflow in
  the repository by frequency: a full build of the application, the plugins and
  every test binary, plus an apt run and Apache's repository. Nothing here caches.
  If that becomes the thing worth fixing, a build cache is the lever, and it is a
  ticket rather than a line.
- **The library lists of two workflows are now held equal by a test.** Adding a
  package to `release.yml` for the release's own reasons fails the suite until
  somebody says which of the two it belongs to. That is the intended cost.
- **The live tier still runs only when somebody remembers**, which is MOLE-29 and
  stays in `Backlog`: it needs a self-hosted runner registered against the
  repository, and that is an account decision with a network-reach decision inside
  it.
