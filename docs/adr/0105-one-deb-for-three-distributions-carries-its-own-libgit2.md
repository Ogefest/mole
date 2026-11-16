# ADR-0105: One .deb for three distributions, carrying its own libgit2

- **Date:** 2026-11-16
- **Status:** Accepted

## Context

The `.deb` is built on Ubuntu 24.04 and its dependencies are derived:
`dpkg-shlibdeps` asks the build machine which package each linked library came
from. So the package named exactly what Ubuntu 24.04 calls them, and 0.1.4 refused
to install on Debian 13 — while the README promised it for "Debian and Ubuntu".
Of its 30 dependencies, 25 exist under the same name on Debian 13, Ubuntu 24.04 and
Ubuntu 26.04. The other five are two different problems:

- **Four Qt libraries** — `libqt6gui6t64`, `libqt6network6t64`, `libqt6sql6t64`,
  `libqt6concurrent6t64` — are called without the `t64` suffix by Debian, and
  Ubuntu 26.04 took Debian's names. The file inside is the same soname, and Qt keeps
  binary compatibility within a major version. A naming difference only.
- **`libgit2-1.7`.** libgit2 changes its soname with every minor version because its
  ABI changes: 24.04 has 1.7, the other two have 1.9, and no single version is on
  all three. A real difference.

The install check in `release.yml` ran only in `ubuntu:24.04`, the distribution that
built the package, which is why nothing caught it.

## Decision

- **One `.deb`, still built on Ubuntu 24.04**, the oldest of the three: its glibc
  2.39 and Qt 6.4 are the floor and both newer systems are above it.
- **libgit2 is linked statically into the `.deb`**, from a pinned release whose
  checksum is verified (`scripts/libgit2-static.sh`), built with every network
  transport off and its own bundled regex, HTTP parser, zlib and SHA code, so it adds
  no runtime dependency. Mole reads only local repositories (ADR-0041). `make deb`
  refuses a package whose `Depends` still names a libgit2. Every other artefact keeps
  linking the system's libgit2.
- **Every Qt `t64` name gets its unsuffixed alternative**, by rule, after
  `dpkg-shlibdeps` has run: `libqt6gui6t64 (>= 6.4.0) | libqt6gui6 (>= 6.4.0)`. It is
  a `CPACK_POST_BUILD_SCRIPTS` step (`packaging/deb-qt-alternatives.cmake`) that
  unpacks the finished `.deb`, rewrites `Depends` and packs it again with `dpkg-deb`.
- **The release installs the `.deb` in fresh `debian:13`, `ubuntu:24.04` and
  `ubuntu:26.04` containers**, and `mole --plugins` reports git state so that a build
  which lost the static libgit2 fails there rather than reaching a user.

## Reason

For libgit2, the alternatives were:

- **A `Depends` alternative, `libgit2-1.7 | libgit2-1.9`.** It installs, then fails
  to start: a binary linked to `libgit2.so.1.7` cannot load 1.9.
- **One `.deb` per distribution.** It works, and it is three files to choose between
  and three to check, for a library Mole uses a small, read-only part of. The author
  wants one file.
- **Building on Debian 13 instead.** It moves the problem rather than solving it:
  1.9 is then missing on 24.04, and glibc 2.41 would put 24.04 below the floor.
- **Loading libgit2 with `dlopen` by any soname found.** Its ABI changes with each
  minor version, so a binary compiled against one set of headers cannot safely call
  another. That is the reason the soname moves at all.

Static linking was the only option that gives one file that works on all three. Its
cost is that libgit2's security fixes are now Mole's to pick up, which is why the pin
sits in one obvious place with a note on how to bump it. With the transports off,
little of libgit2's network-facing code is in the binary at all.

For Qt, where the names differ but the files do not, an alternative in `Depends` is
enough and static linking would be absurd. It is applied by a rule rather than a
typed list so that the next Qt library the build links is covered without anybody
remembering. A post-build step was chosen over running `dpkg-shlibdeps` ourselves
and setting `CPACK_DEBIAN_PACKAGE_DEPENDS`: CPack's own derivation stays exactly as
it was, and the step only edits what it produced.

The licence allows the static link: libgit2's linking exception applies to static
and dynamic linking alike. The static copy also carries libgit2's bundled xdiff
(LGPL-2.1-or-later); it stays replaceable because everything needed to rebuild the
package with a modified libgit2 is public. `THIRD-PARTY-NOTICES.md` says this.

## Consequences

- The `.deb` installs on Debian 13, Ubuntu 24.04 and Ubuntu 26.04 and nowhere else
  is promised; the README says so, and points everyone else at the AppImage and the
  tarball.
- Bumping libgit2 in the `.deb` is a deliberate change to `scripts/libgit2-static.sh`
  rather than something the build machine decides.
- A distribution that renames another library would show up as a failed install in
  one of the three release containers, not in a user's terminal.
- Parquet in the `.deb` (MOLE-459) builds on this: still one package, with Arrow as
  something 26.04 can satisfy and the other two skip.
