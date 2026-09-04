# Licensing and Qt LGPL compliance

Mole is **Apache-2.0**. Qt is **LGPL-3.0** and is not part of this
project. That combination is fine, but only while a handful of conditions hold.
This document records what they are, which ones are already satisfied, and what
you have to do yourself when you publish a build.

## The audit

Verified against the current build:

| Requirement | Status | How it was checked |
|---|---|---|
| Qt is dynamically linked, never static | **OK** | `ldd` finds `libQt6*.so`; `nm -D` finds no Qt symbol in the binary's own dynamic table |
| Every Qt module the build asks for is LGPL | **OK** | Core, Gui, Network, Qml, Quick, QuickControls2, Sql, Concurrent, and Pdf, Multimedia and Test where present — all LGPL-3.0. Qt Pdf embeds PDFium under permissive terms; see `THIRD-PARTY-NOTICES.md` |
| A Qt module nobody has checked cannot slip in | **OK** | the check reads every `find_package(Qt6 … COMPONENTS …)` line and refuses anything not on an allowlist, so adding a module means looking its licence up |
| Qt is unmodified | **OK** | no Qt source is vendored; the system Qt is used as installed |
| The user can replace Qt with their own build | **OK** | see *Relinking* below |
| Every linked library has a row in the notices | **OK** | the check reads every `find_package` and `pkg_check_modules` name and refuses one the notices do not mention |
| Every licence text the notices name is shipped | **OK** | the required list is derived from `THIRD-PARTY-NOTICES.md` rather than written down twice |
| Qt use is stated prominently | **OK** | `NOTICE`, the README, and Help → About in the app |
| PDFium attribution, as shipped inside Qt Pdf | **OK** | `THIRD-PARTY-NOTICES.md` |
| The GPL-3 library the network plugin links | **decided** | libsmbclient; see `THIRD-PARTY-NOTICES.md` and [ADR-0094](adr/0094-smb-in-the-self-contained-artefacts.md) |

Re-run the check any time with:

```sh
make licence-check                                  # every artefact it can find
scripts/licence-check.sh <binary> [<artefact root>] # one of them
```

`make licence-check` sweeps what has been built: the bundle in `dist/`, and the
`.deb`, the `.rpm` and the AppImage in `build/packages/`. Each is unpacked and asked
about its own paperwork, and one it cannot unpack here — the `.rpm`, on a machine
with no `rpm2cpio` — is *named as unasked* rather than passed over. The release
workflow asks all four, which is why it installs `rpm` and `cpio` on the runner.

Two of those rows were added because the check could not fail for them. The
required licence texts were a fixed list of five files, so the four texts that had
never been written to `licenses/` — LGPL-2.1, GPL-2 with the linking exception,
MIT and BSD — could not be reported missing; they are now derived from the notices
themselves. The GPL-only Qt rule was a fixed list of five module names, so Qt Quick
3D, Qt Graphs and every add-on released since passed unexamined; it is now an
allowlist, and adding a module to the build means adding it there. And the
"no Qt symbols in the binary" question read `nm --defined-only`, which reads the
symbol table `strip` removes — and every published binary is stripped before the
check sees it, so on exactly the artefacts it exists for it answered "none" by
absence. It reads `nm -D` now, which survives stripping. See MOLE-355.

The script asks two kinds of question and keeps them apart, which it did not always
do. **Which Qt modules the build uses** is a question about the source tree, and it
is asked of the repository the script lives in, found from its own path. **The
paperwork, and whether a bundled Qt stays replaceable**, are questions about an
artefact, and they are asked of a root: given as the second argument, or derived
from the binary's own location. They used to be asked of the working directory, so
`make bundle` running the check from the repository root was asking whether *this
repository* carries `LICENSE`. It always does; an artefact carrying none of the five
files passed. Nothing published was ever wrong — `make bundle` copies them itself —
but a guard that cannot fail for the reason it exists is not a guard. See MOLE-298.

## Why dynamic linking is the whole ballgame

LGPL-3.0 lets a differently-licensed application use Qt, on condition that the
recipient can replace Qt with a modified version and still run the application.
Dynamic linking is what makes that possible without shipping our object files.

Two things would break it:

1. **Statically linking Qt.** Then you would owe recipients either your object
   files or your source under LGPL terms. `MOLE_ENABLE_STATIC_QT` does not exist
   and should not be added without reading this first.
2. **Preventing replacement** — signing, hard-coding an absolute path to a
   specific Qt, or bundling in a way the user cannot open. We do none of these.

## Relinking

**Installed build** (`make install`): Qt comes from the system. Replace the
distribution's Qt packages, or set `LD_LIBRARY_PATH` to a different Qt before
launching. Nothing else is required.

**Bundle** (`make bundle`): Qt lives in `dist/usr/lib` as ordinary, unsigned,
writable `.so` files, and `dist/mole` is a shell script that points
`LD_LIBRARY_PATH` at that directory. To use a different Qt, drop the files in
or edit one line of the launcher. That is exactly the freedom the licence asks
for, and it is worth not breaking it later for the sake of a tidier layout.

## What you still have to do when you publish

- **Ship `LICENSE`, `NOTICE`, `THIRD-PARTY-NOTICES.md` and `licenses/`** with
  every build. The `make bundle` target copies them in; a hand-assembled
  archive will not. Which texts `licenses/` has to hold is decided by the
  notices: every `licenses/…` path in that table has to be there, and
  `make licence-check` says which one is not.
- **Be able to supply the Qt sources you linked against.** In practice: record
  the exact Qt version, and either link to the matching upstream tarball or
  keep a copy. Distribution packages (`.deb`, RPM, Flatpak) inherit this from
  the distribution and are the easiest route.
- **Read `THIRD-PARTY-NOTICES.md` before redistributing the bundle.** It
  carries around 120 libraries from the build machine, and their licences come
  with it. A distribution package sidesteps the entire problem.

## Plugins

Apache-2.0 was chosen so that plugin authors are not forced into any particular
licence. A plugin links `mole_sdk` and may be released under whatever terms its
author prefers, including a proprietary one.

The exception worth knowing: a plugin that links Qt directly — most will —
inherits Qt's LGPL obligations for itself. That is between the plugin author
and Qt, not something this project can grant or withhold.
