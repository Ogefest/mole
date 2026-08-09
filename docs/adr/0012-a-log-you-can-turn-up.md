# ADR-0012: One log, turned up per subject rather than per place

- **Date:** 2026-08-09
- **Status:** Accepted

## Context

A copy from an SFTP drive to local disk brought back part of a file and said
nothing was wrong. There was no way to find out what had happened. The session
log ([SessionLog](../../src/app/SessionLog.cpp)) records whatever the application
says, and the application said almost nothing: no backend reported what it had
been asked for, no task reported what it had done, and libcurl's own account of
the conversation went nowhere at all. The only way to see inside was to add a
`qDebug` next to the suspicion of the hour and rebuild.

That is the wrong shape twice over. It makes every diagnosis start with a code
change, and it means the detail lands wherever somebody was last looking rather
than uniformly. A log worth having is one where the same kind of thing is
recorded the same way whichever drive or job it came from.

## Decision

**Four Qt logging categories, by subject rather than by module:**

| category | what it records | written by |
|---|---|---|
| `mole.task` | every background job: what started, what it ended as, how long it took | `Task::execute` |
| `mole.drive` | every operation on every drive: what was asked, what came back, how long it waited | `LoggingFileSystem` |
| `mole.net` | one line per network transfer: address, result, bytes expected and received, speed, whether the connection was new | `CurlPool::perform` |
| `mole.curl` | libcurl's own commentary, the SSH and SFTP conversation included | `CURLOPT_DEBUGFUNCTION` |

**Turned on with `MOLE_LOG`** — `MOLE_LOG=net,curl`, or `MOLE_LOG=all`. Output
goes to the existing session log, so a report is still one file to send.
`QT_LOGGING_RULES` works too, because these are ordinary Qt categories.

**Silent at debug, audible at warning.** Each category is declared with
`QtInfoMsg` as its default level. The running commentary costs nothing until it
is asked for; a job that failed, a download that ended short of its announced
length, or a copy that carried fewer bytes than the listing promised is logged
whether anyone asked or not.

**Two of the four are written in one place each, not in every implementation.**
`Task::execute` covers every job that will ever exist. Every mount goes behind
`LoggingFileSystem` in `VfsManager::addMount`, so local disk, SFTP, an archive
and whatever a plugin brings all report identically, and a backend written next
year is covered without knowing the wrapper exists.

**A short transfer became an error rather than a log line.** `net::errorFor` now
compares what the server announced against what arrived and fails the read when
it is short — see the Consequences below.

## Reason

**Why categories rather than a verbosity number.** The interesting question is
never "how much logging" but "logging of what". Chasing a copy wants
`net` and `curl` and nothing else; chasing a slow directory wants `drive`.

**Why a wrapper rather than logging inside each backend.** Seven backends
already exist and the whole point of `IFileSystem` is that more will arrive.
Logging written into each one is seven chances to word it differently and to
forget. The wrapper is also honest about cost: it is always in place, on or off,
so the code path being diagnosed is the code path that has the fault. Wrapping
only when logging is enabled would be the classic way to make a problem
disappear when you look at it.

**Why the session log rather than a file of its own.** A user chasing a problem
should end up with one file. `SessionLog` already flushes every line and already
keeps the previous run.

**Why not `qDebug` as it was.** Uncategorised debug output is all or nothing,
and "all" from a file manager listing a large directory is unreadable.

**What was rejected.** A logging library (spdlog and friends): Qt's categories
already integrate with the message handler, with `QT_LOGGING_RULES`, and with
QML's own output, and a second logging system would have to be bridged to all
three. An in-application log viewer: worth having one day, but it answers a
different question and does not help someone sending a report.

## Consequences

- A report about a transfer is now a matter of `MOLE_LOG=net,curl`, reproducing
  it, and sending the session log. The reproduction of the fault that prompted
  this took one run.
- Header lines that would carry a credential (`Authorization` and friends) are
  redacted in the `mole.curl` trace. A log gets sent to other people.
- Every VFS call goes through one extra virtual dispatch. Against a network
  round trip, or against a `stat` on local disk, this does not register.
- `net::errorFor` fails a download whose byte count falls short of the length the
  server announced, for every protocol. A truncated file that everything above
  believes is complete is the worst outcome a file manager has; a transfer that
  ends early is now an error with both numbers in the message. It fires only
  when the server itself stated a length and the request asked for a body, so a
  `HEAD` and a server that says nothing are unaffected.
- `IFileSystem::openRead` gained an `expectedSize` hint. It is documented as a
  hint and every backend but SFTP ignores it; see
  [ADR-0013](0013-a-large-sftp-read-arrives-in-spans.md) for what
  forced it.
