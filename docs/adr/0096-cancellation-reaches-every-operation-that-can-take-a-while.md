# ADR-0096: Cancellation reaches every operation that can take a while, not only listing

- **Date:** 2026-09-04
- **Status:** Accepted

## Context

`IFileSystem::list()` took a `CancelToken` and nothing else did. `stat()`,
`makeDirectory()`, `remove()`, `rename()`, `replace()`, `openRead()`,
`openWrite()`, `space()` and `access()` all took none — so every backend that
needed one inside filled the gap with a fresh `CancelToken()`: six sites in
`SftpFileSystem.cpp`, five in `FtpFileSystem.cpp`, eight in
`WebdavFileSystem.cpp`, fourteen in `S3FileSystem.cpp`.

A token that is never cancelled is a token that does nothing, so:

- A recursive `remove()` over a large remote tree ran to the end whatever the
  user did. Somebody who cancels a delete of a big folder watched it continue.
- `rename()` of an S3 "directory" is a copy and a delete of every key under a
  prefix, and it too ran to the end.
- A whole-file `openRead()` on a remote drive — up to 64 MiB before the streaming
  path takes over — was one uninterruptible fetch.
- `BufferedUpload::close()` is a single PUT of everything staged, again up to
  64 MiB, and it happens inside `openWrite()`'s sink.

The task layer could therefore stop work only *between* backend calls. That is
enough for a copy of many small files and no use at all for one large operation,
which is exactly the case a person reaches for Cancel in.

## Decision

**An operation whose work is not bounded by one request takes a
`const CancelToken&`, and a cancelled token means no I/O at all.**

That is `remove()`, `rename()`, `replace()`, `openRead()` and `openWrite()`. Each
gained a defaulted `const CancelToken& cancel = {}` — the same shape
`openRead()`'s `expectedSize = -1` already had — every backend threads it where
it used to construct a fresh one, and every one of the five checks the token
before its first request. `TransferTask`, `DeleteTask`, `SyncTask`, `RenameTask`,
`ReadFileTask` and `ReadRangeTask` pass their own.

The conformance suite states it for every backend at once: a pre-cancelled token
must make each of the five fail with `VfsError::Cancelled`, and the `remove` case
is asserted against a directory whose contents are checked later in the same run
— so a backend that ignored the token would delete the fixture and fail loudly.

**`stat()`, `makeDirectory()`, `space()` and `access()` keep no token.** Each is
one request and one answer: there is no moment between a start and a finish for a
token to be consulted in, so a parameter there would be a promise nothing could
keep. Where a backend makes one of these expensive — SFTP and FTP answer `stat()`
with a listing of the parent — the fix is to ask less often, which is the other
half of this change.

## Reason

**Why not a token on everything.** A parameter that no implementation can act on
is worse than no parameter: it reads as a guarantee. The rule "unbounded work
takes a token" is one somebody can apply to the next method, and the four
exceptions are named here so the omission is a decision rather than an oversight.

**Why a defaulted argument rather than an overload per method.** Nine overloads
across fourteen classes, all of which would forward to the token-taking form, is
a lot of code that says nothing. The default is also what let the change land
without touching the eighty-odd call sites that have no token to give — a
thumbnailer, a metadata reader, a store writing its own file — and those really do
want the "never cancelled" answer.

The known trap with a defaulted virtual argument is that the default is taken
from the *static* type of the call. Here every declaration uses the same `{}`, so
there is no answer to get wrong; `tst_WrappedDriveAnswers` already holds every
wrapper to forwarding, which is where a mismatch would show.

**Why the token is checked at the top and not only in the loop.** "Cancelled"
has to mean nothing happened. A recursive remove that checks only between
children still deletes the first one, and a caller cannot tell that from a
refusal — so the entry check is the difference between a cancel and a partial
delete.

## Consequences

- Cancelling a delete, a rename or a large read on a remote drive stops it, at
  the next span or the next child rather than at the end of the whole operation.
- A backend written next year has to decide what its five mutating methods do
  with a token, and the conformance suite refuses it if the answer is "ignore
  it". That is the point.
- The four token-less methods are a rule with a stated reason rather than a gap,
  and if one of them ever becomes unbounded on some backend, this record is what
  gets superseded.
- The recursive `remove()` on SFTP and FTP was also quadratic — a stat per child,
  each a full listing of the parent — and is now a single stat at the top with a
  private `removeEntry()` the recursion calls with the `FileEntry` the listing
  already produced. Deleting ten thousand files over SFTP no longer parses a
  hundred million listing rows. That is not a decision, just a fault; it is
  mentioned because it is what made the cancellation matter so much in the first
  place.
