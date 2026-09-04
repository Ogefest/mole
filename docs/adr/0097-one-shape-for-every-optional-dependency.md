# ADR-0097: One shape for every optional dependency, and the line it prints is an interface

- **Date:** 2026-09-04
- **Status:** Accepted

## Context

Eleven features in Mole depend on a library that may not be installed: Arrow and
Parquet, libvterm, libgit2, xxHash, OpenSSL, libarchive, libcurl, libsmbclient,
libnfs, Qt Pdf and Qt Multimedia. The rule for all of them has been the same
since the beginning and is written in `ARCHITECTURE.md`: find it quietly at
configure time, compile behind a define, and report the feature unavailable at
run time rather than failing to build.

The rule was followed eleven times and written out eleven times, which produced
six shapes:

- `find_package(LibArchive)` was the only find that was not `QUIET`, and it —
  with libcurl and OpenSSL — reported a missing optional library as a CMake
  **warning** where the other eight printed a status line.
- **Qt Pdf and Qt Multimedia printed nothing at all when they were found.** That
  is the one that mattered, because the configure summary is read by machines: a
  row with no positive line cannot be checked for, so `feature-summary.sh` had to
  name their *not-found* text instead, and a check written as "this string is
  absent" passes the day somebody rewords the message.
- xxhash, SMB and NFS passed `target_link_directories()`; libvterm and libgit2
  did not. A library in a prefix the linker does not search therefore linked for
  three rows and failed for two.
- `MOLE_HAVE_XXHASH` was `PRIVATE` where the other ten defines were `PUBLIC`.
- libgit2 was the only row with a `MOLE_WITH_` switch, for a reason — *let a
  machine that has the library build the way a machine without it does* — that
  applies to every row.

`ARCHITECTURE.md`, meanwhile, said "three features depend on libraries that may
not be installed" and listed four.

## Decision

One CMake function, `mole_optional_dependency()` in
[cmake/MoleOptionalDependency.cmake](../../cmake/MoleOptionalDependency.cmake).
It finds the library — `find_package`, a Qt component, or pkg-config — prints
exactly one status line, adds the define, and links what was found, including the
link directories. Every one of the eleven rows goes through it.

**The line it prints is an interface, and every row prints one whether the
library was found or not.** `scripts/feature-summary.sh` holds a release build to
those lines; the release workflow refuses to publish an artefact that is missing
one; `scripts/configure-summary.sh` prints them in all four places that print them
(the fast tier, the second family, the Windows job and the AppImage packer), and
*names a row that printed nothing*.

The shape is `<summary>: <answer>` — "Git state: libgit2 1.7.2", "NFS exports:
unavailable (install libnfs-dev)". A switch that is off prints a third answer,
"not built (MOLE_WITH_SMB is OFF)", because a decision this build took is not the
same as something somebody could install.

`ARCHITECTURE.md` carries a row per dependency, and `tst_Packages.sh` holds that
table against the calls — so a library added to the build has to be written down
before the suite is green again.

## Reason

The alternatives considered:

- **Leave it and fix the two rows that print nothing.** Rejected: it is the third
  time a row has been found in the wrong shape, and each time the fix was to that
  row. Six shapes is not eleven mistakes, it is one missing abstraction.
- **A macro per kind of find** — one for pkg-config, one for `find_package`.
  Rejected: the kind of find is the least interesting difference between the rows,
  and splitting on it would leave the message shape duplicated in two places
  rather than one.
- **A generated list**: read the rows out of a data file and emit the CMake.
  Rejected as a build step to debug in exchange for nothing the function does not
  already give.
- **Print the summary from one place at the end of configure**, having collected
  the rows. This is better in one way — the summary would be contiguous in the
  output rather than interleaved with CMake's own lines — and worse in two: the
  line would be further from the decision that produced it, and a row would have
  to survive as data until the end of a configure that can `FATAL_ERROR` in
  between. Worth revisiting if the output ever becomes hard to read.

**Why only two rows carry a `MOLE_WITH_` switch.** The general mechanism for the
`find_package` rows already exists and is CMake's:
`-DCMAKE_DISABLE_FIND_PACKAGE_Arrow=ON`, which is what `make packages` passes.
`MOLE_WITH_GIT2` and `MOLE_WITH_SMB` exist because a pkg-config row has no such
switch and because SMB's is a licence decision (ADR-0094) rather than a
convenience. The function takes `SWITCH` for any row, so adding one is a line.

## Consequences

A library added to this build is found the same way, reported the same way, and
appears in the CI summaries and in `ARCHITECTURE.md` by existing rather than by
somebody remembering. Three checks that could only look for a *missing* message
now look for the feature.

The function is a layer between the row and CMake, and that has a cost: a reader
who wants to know exactly what `pkg_check_modules` was asked has one more file to
open. It is a hundred and seventy lines, most of them the note explaining what
each argument is for.

`MOLE_HAVE_XXHASH` became `PUBLIC` with the others. Nothing outside `mole_core`
reads it today, so this changes nothing that runs — it removes a difference the
next reader would have had to account for.
