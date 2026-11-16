# ADR-0106: Parquet is a module loaded at run time

- **Date:** 2026-11-16
- **Status:** Accepted

## Context

Parquet was compiled into the application. `mole_core` linked
`Arrow::arrow_shared` and `Parquet::parquet_shared`, and `ParquetTable.cpp` was
written twice under `#ifdef MOLE_HAVE_PARQUET`. So a binary either needed
`libarrow.so` to start at all, or had no Parquet.

The one `.deb` is for Debian 13, Ubuntu 24.04 and Ubuntu 26.04 (ADR-0105), and is
built on 24.04. Ubuntu 26.04 is the first release in the family to package Apache
Arrow (`libarrow2300`, `libparquet2300`, Arrow 23.0.1); the other two have none. A
`.deb` linked to Arrow would refuse to install on two of its three distributions,
and there is no Arrow on 24.04 to build against anyway. The author wants one `.deb`,
not a second file to choose or a second package to install.

## Decision

- **The Arrow half of the Parquet reader is a separately built module,
  `mole-parquet.so`** (`src/modules/parquet`), which exports four C functions: its
  interface version, the Qt version it was built against, its Arrow version, and a
  factory returning a `ParquetReader` — an `ITableSource` plus what the preview
  shows about a file. That header, `core/data/ParquetReader.h`, is the whole
  boundary.
- **`ParquetTable` in `mole_core` is the loader.** It links no Arrow.
  `parquetModule::current()` finds the module beside the executable (a build tree)
  or in `<prefix>/<libdir>/mole/modules`, loads it once with every symbol resolved,
  and refuses it if it speaks another interface version or was built against a newer
  Qt than the one running. Without a loadable module, `isSupported()` is false and the
  preview falls back to the file-information view. The reason is written once to the
  session log and shown by `mole --plugins`, never in a dialog.
- **For the `.deb` the module is built in an `ubuntu:26.04` container**
  (`scripts/package-parquet-module.sh`) and added to the package built on 24.04 by a
  CPack post-build step (`packaging/deb-parquet-module.cmake`). That keeps it out of
  `dpkg-shlibdeps`, which would otherwise put Arrow in `Depends`. What it links
  becomes `Recommends` instead. The list is derived from its `NEEDED` entries with
  `dpkg -S` in the container, less what the package already depends on.
- **Every other artefact switches to the module as well.** The tarball, the `.rpm`
  and a local build compile it beside the application wherever Arrow is found, so
  Parquet is reached one way everywhere. The bundler collects the module's
  libraries the way it collects a plugin's.

## Reason

The alternatives were:

- **Two `.deb`s**, one built on 26.04 with Arrow linked. Rejected by the author:
  two files to choose between.
- **A second package, `mole-parquet`**, that depends on Arrow. It solves the same
  problem with a second thing to install and a version pairing to keep, and only on
  26.04 can it be installed at all.
- **Linking Arrow into the application with `dlopen` of Arrow itself.** Arrow's C++
  API is enormous and template-heavy. Calling it through `dlsym` is not practical,
  and its ABI changes with every major version (the soname is `2300`).
- **Building the whole `.deb` on 26.04.** That moves the floor to 26.04's glibc and
  Qt 6.10, and the package stops installing on Debian 13 and Ubuntu 24.04. This is
  ADR-0105's question, already answered.

A module with a narrow interface is what lets one file serve three distributions.
Qt keeps binary compatibility within a major version and the boundary carries
nothing else, so a module built against Qt 6.10 can be called from an application
built against 6.4, on a system running 6.10. Where it cannot be called safely — a
24.04 or Debian 13 system that has Arrow from Apache's own repository but an older
glibc, libstdc++ and Qt — it fails to load or is refused by the Qt-version check.
Mole says the module was built for a newer system rather than crashing.

`Recommends` is the right strength. apt installs recommends by default, so the
26.04 install pulls Arrow in with no extra step. A recommend that does not exist
is skipped without an error, so the other two install exactly as before.

## Consequences

- The `.deb` has the Parquet grid on Ubuntu 26.04 and not on Debian 13 or Ubuntu
  24.04; the README says which and why, and `mole --plugins` has a `parquet:` line
  that the release asserts in every container.
- `ParquetReader.h` is an ABI. Changing it means raising
  `MOLE_PARQUET_MODULE_ABI`, so that a mismatched module is refused rather than
  called wrongly.
- Building the `.deb` with its module needs docker. Without it `make deb` says so and
  builds a package with no module, the same rule `make rpm` follows.
- The next Arrow soname in 26.04's successor follows by itself: the `Recommends` are
  read from the module, not typed.
- The test fixtures that write Parquet files now find Arrow themselves, since nothing
  they link carries it any more.
