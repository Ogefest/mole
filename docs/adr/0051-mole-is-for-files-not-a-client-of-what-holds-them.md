# ADR-0051: Mole is for files, and is not a client of the systems that hold them

- **Date:** 2026-08-19
- **Status:** Accepted

## Context

The same scope decision has now been taken three times, each time from scratch, and
each time recorded only as part of the subject that provoked it:

- **[ADR-0007](0007-writing-archives.md), writing into archives.**
  Rewriting the container is a different feature with different failure modes, so
  Mole reads archives and does not write them.
- **[ADR-0041](0041-git-state-is-read-through-libgit2.md), git state.** *"Showing
  state and changing it are not the same feature, and a file manager drifting into
  being a git client is a thing that happens one reasonable-looking commit at a
  time."* The band shows the branch and the changed files; nothing commits, stages,
  checks out or fetches.
- **2026-08-18, drive capabilities.** Where a drive holds earlier versions of a file
  — ZFS snapshots, S3 object versions — the decision was that Mole lists them, opens
  them and copies one out, and never rolls back, deletes a version or turns
  versioning on. Restoring an archived S3 object was ruled out for the same reason:
  it is an operation on the storage service, not on a file.

Three subjects, one answer, arrived at independently every time. That is the signal
it is a rule rather than three coincidences — and an unwritten rule is one that gets
argued again on the fourth occasion instead of being cited.

## Decision

**Mole is for files.** It reads them, operates on them, and its ambition on the
reading side is *maximal*: everything that can be learned about a file cheaply and
safely is worth showing, including facts that come from the system underneath rather
than from the bytes — an earlier version exists, this folder is a checkout, this
object is not immediately readable. Surfacing more about a file is squarely in scope,
and this record must not be read as a reason to refuse it.

**Mole is not a client of the systems that hold those facts.** Where another system
knows something about a file, Mole surfaces it and stops. **The line is at the first
thing that writes into that other system**, and it holds even when the write looks
small and obvious.

**The question that decides a case is not "is this useful?"** — it usually is — but:

> **Does this require Mole to model another system's domain?**

If answering it means carrying that system's concepts, vocabulary, permissions or
failure modes, it is out, however convenient. A file manager that reads a queue
service as if it were a directory has stopped being a file manager, and the way there
is a series of individually reasonable steps.

Two consequences worth stating outright, because both have been asked:

- **Mole does not "support ZFS", and will not.** It reads what a filesystem already
  exposes through ordinary paths. A dedicated tool for a storage domain is a better
  tool for that domain than a file manager will ever be.
- **Mole is not the tool for regular git work**, and does not want to be. It says
  *this folder is a checkout, and these files changed*; the work itself belongs in a
  client built for it.

## Reason

**Why a rule rather than a precedent each time.** Each of the three decisions above
is correct and each is recorded where it was taken, which means a reader meeting the
fourth case finds none of them: the reasoning lives under *archives*, under *git* and
under *drive capabilities*, and the case at hand is about none of those. Writing the
rule down once turns an argument into a citation.

**Why the line is at the first write, rather than at "no integrations".** The reading
side is where a file manager earns its keep, and the systems underneath know a great
deal about files that the bytes do not say. Refusing to look would make Mole worse at
the thing it is for. Writing is where the domain arrives: a write needs the other
system's permissions model, its error taxonomy, its notion of what is atomic, and a
plausible answer for what to do when it half-succeeds. That is a client, and a client
of anything is a project.

**Why "does this model another system's domain?" rather than a list of what is
allowed.** A list goes stale the first time something arrives that is not on it,
which is exactly the situation this record exists to answer. The question can be put
to a case nobody anticipated and gives an answer that can be argued about on the
merits.

**Alternatives considered.**

*Say nothing and keep deciding case by case.* This is what happened three times, and
it works — each answer was right — but it costs the same argument every time and the
answers agree only because the same people happened to be in the room.

*Draw the line at "no dependencies on other systems".* Too tight, and already
contradicted by what ships: libgit2 reads a work tree, libarchive reads containers,
the S3 backend speaks a storage API. Reading is not the problem.

*Draw it at "nothing destructive".* Too loose, and the wrong axis. Turning on S3
object versioning destroys nothing and is squarely a storage-service operation;
deleting a file is destructive and is exactly Mole's business.

## Consequences

- **A new case is decided by asking the question, not by finding a precedent.** The
  three records above become instances rather than authorities.
- **ADR-0007 and ADR-0041 are not superseded.** Each remains the record for its own
  subject and its own reasoning; this one is the rule they turn out to be instances
  of. Both link forward to it.
- **The reading side has explicit permission to grow.** A backend that can say
  something more about a file — a snapshot exists, an object is archived, a checkout
  is dirty — is welcome to say it, and this record is not a reason to refuse the
  work.
- **Something will be lost that somebody wants.** Rolling back a ZFS snapshot from
  the listing, committing from the band, restoring a Glacier object: each is one
  keystroke from where Mole already stands, and each is refused. That is the cost,
  named rather than discovered later.
- **This is not the design of any capability mechanism.** How a drive advertises what
  it can do, and what an action hands back to the interface, is a separate decision
  that will come with the work that needs it. This record is the boundary alone, and
  it stands whether or not anything is ever built on the other side of it.
- Nothing user-visible changes, so there is no changelog line.
