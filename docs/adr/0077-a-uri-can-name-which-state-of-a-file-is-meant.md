# ADR-0077: A uri can name which state of a file is meant

- **Date:** 2026-08-22
- **Status:** Accepted

## Context

`VfsUri` identifies a file. Where a drive keeps earlier states of that file,
there was nowhere to put *which* state, so an earlier version could not be named,
opened, previewed or copied.

The cheapest way to make every part of Mole work on an earlier version is for one
to be **an ordinary readable uri**. `F3`, `F5`, the diff in a report and every
viewer already work on those, with no change to any of them. Anything else — a
second kind of address, a version carried beside the uri — is a second code path
through the preview layer, the transfer layer and everything that stores a
location.

For a filesystem that exposes its snapshots as paths this is nearly free: the
snapshot path is already a path. For an object store it is not: the version is a
parameter on the request, not a different key.

## Decision

**`VfsUri` carries an opaque version token alongside the path.** Empty means the
file as it is now, which is what every uri in Mole meant before this existed. The
token is the drive's own — a snapshot name, an object's version id — and nothing
above the backend that issued it ever reads it.

**It is written as `?version=<token>` at the end of the uri**, and `%` and `?`
inside the path are percent-encoded so the marker cannot be confused with a file
whose name contains one. `fromString(toString(u)) == u` for every uri, versioned
or not, awkward name or not.

**It is part of what makes two uris different** — equality, the hash and
`canonicalKey()` all include it. `parent()` and `child()` drop it.

**A drive that does not understand a version refuses it**, with `NotSupported`,
and the refusal names both the token and the file. The refusal is not each
backend's job: `withVersionGuard()` wraps every drive on its way into
`VfsManager`, alongside the log wrapper, and a backend opts out of it by
answering `understandsVersions()`.

## Reason

**In the uri rather than beside it**, because a bookmark, a restored session, a
file set and a task's title are all a *string*. A version carried in a parallel
field would survive none of them, and a version that cannot be written down is a
version you cannot come back to — which is most of the point of being able to see
one.

**Percent-encoding, and only two characters.** A `?` is a legal character in a
POSIX filename; the awkward-names suite has a `really?.txt`, so the marker cannot
simply be a `?`. Encoding `%` as well is what makes the encoding reversible: a
file genuinely called `a%3Fb` would otherwise come back as `a?b`. Nothing else is
encoded, because a uri is read by people — an encoder that also took the spaces
and the accents would turn every path in every error message into something
nobody can check against their own filesystem. The two known costs are written
down here: a stored uri whose path contains the literal text `%3F` or `%25` now
reads back as one containing `?` or `%`, and no uri Mole has ever written
contains either.

**Refusal rather than silent fallback, and that is the whole risk of the
ticket.** A backend that ignored a token it did not recognise would answer with
the *current* file while the window says it is showing an earlier one. That is a
silent wrong answer on the one screen whose entire purpose is to say which
version you are looking at, and it is worse than the feature not existing.

**A wrapper rather than a guard in every backend.** The alternative was a line at
the top of every method that takes a uri — some ninety of them across the nine
backends in the tree — which is ninety chances to forget one, and forgetting one
is not a crash but the current file shown as an earlier version. It is the
argument `LoggingFileSystem`'s own header already makes about itself: written
once and applied to every mount, so a backend written next year gets it without
knowing the class exists. The conformance suite holds every backend to it, and a
backend that implements versions is handed the uri unchanged.

**`understandsVersions()` rather than a `VfsCapability` flag.** ADR-0075 drew that
line: the enum holds what the core branches on, and this is one of those — but it
is a property of the *code*, not of what the drive was pointed at, so it is a
plain virtual defaulting to false. Whether a *particular* volume or container
actually keeps earlier states is the other tier, discovered by the probe in
ADR-0076.

**A version belongs to a file, so `parent()` and `child()` drop it.** What is
above an earlier version of a file is the folder as it is now; a drive issues
versions of files and issued none of the directory. Inheriting one would name a
version that no drive ever handed out.

**Compared case-sensitively, whatever the volume does.** The token is the drive's
own identifier rather than a name on the volume, and two spellings of one are two
different versions until a drive says otherwise.

## Consequences

Every viewer, the transfer layer, the diff and the launcher work on an earlier
version with no change, because it is an ordinary uri. Nothing that stores a
location needs to learn anything.

Two wrappers now sit on every mount. Both have to forward everything, and a
decorator that quietly drops a method is the failure mode this shape has —
MOLE-282 is the fault of exactly that kind already found in the log wrapper, and
the test it carries covers both.

Nothing issues a version yet. A filesystem that keeps snapshots is MOLE-200 and
an object store keeping earlier objects is MOLE-201; both now have somewhere to
put what they find, and every other drive refuses one without being edited.
