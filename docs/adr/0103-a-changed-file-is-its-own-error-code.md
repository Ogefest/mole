# ADR-0103: A file that changed under a read is its own error code

- **Date:** 2026-11-08
- **Status:** Accepted

## Context

A streamed read from a network drive notices that the file under it has been
replaced in one of two ways. Either the drive checks before each span, or the
server refuses the span's precondition: WebDAV and S3 answer 412 to an `If-Match`
that no longer matches. Both report it through `net::fileChangedWhileBeingRead()`,
in one sentence (MOLE-348).

That error was a `VfsError::IoError`. `IoError` is also how a body cut short
arrives, and `net::isWorthRetrying()` retries it, rightly, because the next
connection may finish that body. So a span refused for a changed file was fetched
again with the same validator, refused again, and repeated every two seconds until
the transfer's budget of 120 s ran out. MOLE-373 fixed exactly that for 403, 404
and NotSupported, and this case got past it because it shared a code with a
dropped connection. It was found on 2026-11-03, when a weak ETag made every span
answer 412 and each read took two minutes to fail. See MOLE-443.

## Decision

**A new code, `VfsError::Changed`**: the file is no longer the one the operation
began on. `net::fileChangedWhileBeingRead()` returns it. `isWorthRetrying()` says
no to it, and every switch that names the codes names it too.

## Reason

The retry decision is made from the code, and that is deliberate. MOLE-373 made
every switch name every code, so that a new code has to come past each place that
decides something from it. A changed file is a different answer from a dropped
transfer: the remedy is to read the new file, not to wait. So the code should be
different.

Alternatives considered:

- **Match on the message text.** It would have been one line in
  `isWorthRetrying()`. It was rejected because the message is for people, and a
  rewording would quietly bring the two minutes back. Nothing would fail.
- **A flag on `VfsError`, or one the stream reads beside the error.** This keeps the
  vocabulary as it is, but every consumer would then have two things to read to know
  what happened, and the switches that MOLE-373 made exhaustive would not see it.
  It is the same information in a place nobody is made to look.
- **Keep `IoError` and have each backend stop the stream itself.** That puts four
  copies of the decision in four backends, when today there is one in the stream.

## Consequences

- The code is appended after `Unknown`, so the numeric values of the existing codes
  are unchanged. Tests print and compare them as integers.
- To the drive list a changed file says nothing about whether the drive can be
  reached, so a drive is not marked offline for it. Before this change it was,
  because `IoError` is one of the two codes that do that.
- A plugin built outside this tree that switches over the codes without a `default`
  gets a warning for the new value. That is the point of naming every code.
