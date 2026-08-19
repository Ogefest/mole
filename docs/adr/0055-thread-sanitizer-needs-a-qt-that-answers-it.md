# ADR-0055: ThreadSanitizer needs a Qt that answers it, so `make tsan` builds one

- **Date:** 2026-08-19
- **Status:** Accepted

## Context

`make tsan` existed and nobody could act on what it said. A whole run reported
**5038 warnings across 45 of the 91 suites**, which is not a list anybody triages
— it is a wall, and a wall gets ignored, which is the one thing a race detector
must never become.

The reason is a single line in Qt's own headers. Qt annotates its mutexes and
futexes for ThreadSanitizer through `QtCore/qtsan_impl.h`, and every annotation
is behind a macro evaluated **when the translation unit is compiled**:

```c
#if (__has_feature(thread_sanitizer) || defined(__SANITIZE_THREAD__)) && __has_include(<sanitizer/tsan_interface.h>)
#  define QT_BUILDING_UNDER_TSAN
```

Our code is compiled with `-fsanitize=thread`, so it picks the annotations up
from the header. A distribution's Qt is not, so the locking inside its libraries
is invisible: TSan sees a write on one thread and a read on another with no
happens-before between them and reports a race that does not exist.
`libQt6Core.so.6` from the distribution does not link `libtsan` at all.

Classified by where *both* of the racing accesses lived, that run was:

| Warnings | Both accesses in |
|---:|---|
| 2227 | Qt |
| 1005 | glibc |
| 713 | one ours, one theirs |
| 402 | libstdc++ headers |
| 366 | our own code |

The fifteen "double lock of a mutex" reports were the same cause from the other
end — our side of a lock annotated, Qt's side not, so the pairing never
completed.

The glibc row deserves a note, because it misled us first. Those were
`tzset_internal` under `QFileInfo::lastModified()`, and they read exactly like
the documented case where concurrent `localtime_r` races on lazy timezone
initialisation. Calling `tzset()` once at startup changed nothing, which was the
clue: **the serialisation was Qt's, not glibc's, and TSan could not see it
either.** They disappeared with everything else.

## Decision

**`make tsan` builds against a ThreadSanitizer-instrumented qtbase, and refuses
to run without one.**

- `scripts/qt-tsan.sh` fetches qtbase at the version the project baselines,
  checks it against a recorded SHA-256, configures it with Qt's own
  `-sanitize thread`, and installs it outside this repository —
  `~/opt/qt-6.4.2-tsan` by default, `MOLE_TSAN_QT` elsewhere.
- The `tsan` preset sets `MOLE_CORE_ONLY=ON` and takes `CMAKE_PREFIX_PATH` from
  `MOLE_TSAN_QT`.
- `MOLE_CORE_ONLY` builds core, host, ui, tools and their headless tests: every
  one of those needs nothing outside qtbase. `src/plugins` and `src/app` are
  skipped, and so is everything in `tests/` from the assembled-application
  section down.
- `make tsan` checks the prefix is there and, when it is not, says what to run
  rather than quietly producing the wall again.

**qtbase only.** QtQuick, QtQml, QtPdf and QtMultimedia are not needed by any
suite this tier runs — and QtPdf pulls in PDFium while QtMultimedia pulls in a
media stack, which is a great deal of building for suites that have nothing to do
with concurrency. The concurrency this tool exists to find is in core.

**Two suites are excluded by name.** `tst_KilledOutright` and `tst_MoleTasks`
both fork or start a process, and GCC 13's ThreadSanitizer runtime aborts on an
internal assertion when a multithreaded program forks —
`CHECK failed: tsan_rtl.cpp:253`. That is the tool falling over, not a finding:
both pass under `make test` and `make asan`. `TSAN_EXCLUDE` names them, so the
exclusion is visible rather than a filter that happens to miss them.

## Reason

**Suppressing instead.** MOLE-126's brief offered a fallback: every remaining
warning gets a suppression entry naming what it is and why it is not ours. With
Qt uninstrumented that means suppressing Qt wholesale, and nearly every real race
in this codebase touches a `QString` or a `QObject` somewhere in its stack — so
the suppression would hide the defects the tool exists to find. It is also the
opposite of the rule `lsan.supp` already follows, where every entry is scoped to
a third-party module *precisely so a leak in our own code still fails the build*.

**Living with the noise.** A tool whose output is four-fifths unattributable is a
tool nobody reads. That was the position, and it is why `make tsan` had produced
no fixes since it was added.

**Instrumenting all of Qt.** Rejected on cost for no gain: the modules beyond
qtbase serve suites that drive a window, and a window is not where a race in a
transfer lives.

**Vendoring a prebuilt instrumented Qt.** No such build is published, and one we
published would be a binary dependency nobody could audit.

## Consequences

- **The measurement:** 5038 warnings became **25**, all of them the same race in
  our own code — `Task`'s metric map, written on the main thread in
  `applyPending()` and read on a worker in `bytesDone()`. That is MOLE-126's
  work, and it is now evidence rather than guesswork.
- `make tsan` costs a Qt build the first time. It is one command, it is checked
  against a published checksum, and it is skipped on every later run.
- **`setarch -R` covers the build, not just the run.** `-sanitize thread`
  instruments Qt's build tools too, so the `moc` that runs during *our* build is
  itself a TSan binary and dies on "unexpected memory mapping" without it. Both
  the script and the `tsan` target wrap the build as well as the tests. It cost
  two failed builds to learn, and the error names no cause.
- `make tsan` no longer covers the QML and plugin suites. They were never the
  point of this tier, and they are covered by `make test` and `make asan`.
- A contributor who runs `make tsan` without the prefix gets an instruction, not
  five thousand warnings.
