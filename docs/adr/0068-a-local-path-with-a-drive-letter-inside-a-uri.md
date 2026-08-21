# ADR-0068: A local path with a drive letter, and a UNC share, inside a VfsUri

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

`VfsUri` is the address of every node in Mole, and every local operation goes
through `fromLocalPath()` and `toLocalPath()`. Both were written for POSIX and
neither had ever been handed a Windows path.

`normalisePath()` returns something starting with `/` — correct for every scheme
the class was written for, and its own comment says so. So
`fromLocalPath("C:\Users\ann")` produced the path `/C:/Users/ann`, and
`toLocalPath()` handed that leading slash to `QDir::toNativeSeparators()`, which
turned it into `\C:\Users\ann`. That names nothing. On Windows the local drive
was unreachable from the first listing onwards.

There was a second half with no answer in the code at all. `isRoot()` was
`m_path == "/"`, and on Windows there is nothing above `C:\`. `parent()` of the
drive root produced `/`, which no backend can list.

A UNC share (`\\server\share\file`) had never been considered either.

Nothing had caught it because the class is pure string handling, trivially
testable anywhere, and no test had ever passed it a drive letter:
`tests/core/tst_VfsUri.cpp` was POSIX throughout, and every other caller in the
suite built its input from a temporary directory on the machine running it.

## Decision

**The drive letter is the first path component**, and the leading slash stays:

```
C:\Users\ann        ->  file:///C:/Users/ann     authority "",       path /C:/Users/ann
\\server\share\a    ->  file://server/share/a    authority "server", path /share/a
```

`toLocalPath()` drops the leading slash when the first segment is a drive letter,
and writes a share back as `\\server\share\a`.

**A drive root and a share root are roots.** There is nothing above `C:\` or
above `\\server\share`, so `isRoot()` is true for both and `parent()` of either
is itself. Walking up stops there rather than arriving at `/`.

Three consequences follow and are part of the decision:

- `..` cannot climb out of a volume. `C:\Users\..\..` is `C:\`, the same way
  `/home/../..` has always been `/`.
- `isWithin()` treats only `/` as containing everything. A drive root contains
  only itself and what is under it — `C:\` is not above `D:\`.
- The root of a drive keeps its separator: `C:` and `C:\` are different places on
  Windows, the first meaning "the current directory on that drive".

**A drive letter is a letter and a colon, exactly.** Not "a segment ending in a
colon".

**The platform is an argument, not an `#ifdef`.** `fromLocalPath()` and
`toLocalPath()` take a `HostPlatform` defaulting to the one this build targets.

## Reason

The alternative was to **put the drive in the authority** — `C:\Users` as
`file://C/Users`. Tidier to read, and rejected: `isWithin()` and `operator==`
both compare authority, and that would make them start meaning something for
local paths they have never meant, on a class every backend depends on. The
chosen answer changes one function and one accessor and leaves the rest alone.
It is also what `file:` uris look like everywhere else, and it gives the UNC case
somewhere honest to land — under it the server really is the authority.

**The platform as an argument** is the load-bearing part of this record, and it
applies well beyond this class. An `#ifdef` cannot be tested: the suite runs on
Linux, so the Windows arm of a compile-time switch is a branch no test has ever
entered, which is exactly how this fault survived in a pure function. With the
platform passed in, the Windows spellings are asserted on every machine the suite
runs on, and the fault is fixable today rather than when somebody has the right
computer. `HostPlatform` lives in `src/core/platform/` because the same argument
is owed to what counts as a drive and to what a name may contain.

**A letter and a colon, exactly**, because `notes:` is a legal name on Linux and
somebody may really have one. The looser test would read a top-level directory
called that as a drive root — no name, nothing above it. A top-level directory
called exactly `C:` is still read that way, and that is the price of the
spelling; it is small, and the alternative was a rule that could not be stated in
one line.

## Consequences

Everything local on Windows becomes reachable, and the sidebar's drive rows
(MOLE-181) have somewhere to point. `parent()` never hands a backend a location
it cannot list.

Two spellings of the same path are still two paths — `C:/Users/Ann` and
`c:/users/ann` are different values here. That is deliberately not this record's
problem: case folding is a property of the volume, not of the class, and it is
settled separately in MOLE-240.

`toLocalPath()` on a UNC uri asked for a POSIX path returns empty rather than the
path alone, which would have named an unrelated local directory. Callers that
treat empty as "not a local uri" — which is all of them — already do the right
thing with it.
