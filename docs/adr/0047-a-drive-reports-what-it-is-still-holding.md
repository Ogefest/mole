# ADR-0047: A drive reports what it is still holding, and the shell offers to clear it

Date: 2026-08-19

Status: accepted

## Context

A multipart upload interrupted by the process being killed leaves its parts on
the server. `abandonMultipart()` removes them on every failure the process is
alive to see, and `SIGKILL` gives it nothing to see: no destructor runs, nothing
is cleaned up, and the upload id dies with the process.

**S3 charges storage for those parts until the upload is completed or aborted.**
They are not objects, so nothing that lists a bucket will ever mention them.
Somebody whose machine lost power during a large copy is paying for storage they
cannot see, cannot list and cannot remove from inside Mole.

The question this record answers is the one MOLE-96 left open, and the reason its
epic held it back: **where does a drive-level maintenance action live?** Not what
S3 has to call — that part was never in doubt — but what the shell knows about
it, and where somebody presses it.

## Decision

**The drive reports what it is holding, in its own words, and the shell offers to
clear it.** Two virtuals on `IFileSystem`, gated by a `ReportsLeftovers`
capability, and a `DriveLeftover` describing one:

```cpp
virtual Result<QList<DriveLeftover>> leftovers(std::chrono::seconds olderThan, const CancelToken&);
virtual Result<void> discardLeftover(const DriveLeftover&);
```

A `DriveLeftover` carries an opaque handle the drive issued, the path it belongs
to, when it started, and one line saying what it is. Everything the shell shows
comes from the drive; nothing in the shell knows what a multipart upload is.

**Finding and removing are two steps.** `sweepDrive(id)` reports; `sweepDrive(id,
discard)` acts. The drives dialog does the first from a button beside *Check*,
into the same banner, and the banner then offers the second.

**The age threshold is a parameter, not a constant**, and it defaults to a day.

## Reason

**Why on the drive rather than in the shell.** The alternative was for the shell
to learn about multipart uploads: an S3-shaped action, an S3-shaped dialog, and a
second one the day another backend has something of its own to leave behind. The
same argument put `space()` and `access()` on the drive rather than teaching the
interface about buckets and permissions — a backend answers about itself, and a
backend that cannot answer says so and is left alone.

**Why a capability rather than an empty answer.** A drive with nothing of this
kind must be distinguishable from one that has not been asked. The sweep says
"this drive keeps nothing back" rather than "nothing found", because those are
different facts and only one of them is reassuring.

**Why two steps.** What a sweep finds is somebody's. An upload that looks
abandoned may be a copy running on another machine, or in another window of this
application, right now — nothing in the protocol distinguishes them. Clearing on
sight would trade a fault that costs money for one that loses work.

**Why an age at all, and why the caller chooses it.** The same reason: below some
age a leftover cannot be told apart from an upload in flight. A day is a sensible
default and a wrong constant to bake in, because the right figure depends on how
long a copy takes on the link in question — which the drive does not know and the
person does.

**Why the bytes are not reported.** Knowing how much a leftover is holding means
a `ListParts` for every one of them: one request per leftover for a figure nobody
needs to decide with. What is being decided is whether to keep something that
nothing can ever finish.

## Consequences

- **The prefix filter is applied here, not on the server.** S3 documents a
  `prefix` parameter on `ListMultipartUploads`, and MinIO answers an empty list
  for a prefix that certainly matches — measured against the test machine, with a
  hand-seeded upload the unfiltered listing reports and the filtered one does
  not. A filter that silently hides leftovers is the same fault as not looking,
  so the whole list is asked for and narrowed in the backend. The cost is
  carrying uploads belonging to other prefixes of the same bucket, and a bucket
  has a handful of these at most.
- **A drive rooted at a prefix will not offer to abandon another drive's
  uploads**, which is what that narrowing is for.
- **Paging is followed**, with two markers rather than one: two uploads of the
  same key can be in flight, so the key alone does not say where to carry on
  from. Stopping at the first page would report the first thousand leftovers and
  leave the rest being paid for.
- **An error document is never read as "nothing left behind."** That answer is
  precisely the one that keeps somebody paying, so a document that is not a
  listing fails the call rather than returning an empty list.
- Whether a sweep should ever run on its own — on connect, or on a timer — is
  deliberately not decided here. It cannot run on connect for the reason above,
  and anything else is a preference nobody has asked for yet.
