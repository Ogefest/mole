# ADR-0080: The changelog is a structured log, and the release notes come out of it

- **Date:** 2026-08-23
- **Status:** Accepted

## Context

`CHANGELOG.md` was 145 lines of prose bullets under a single `## Unreleased`
heading, newest first, one sentence each. It was a good file to read and there was
nothing in it a script could take hold of: no dates, no ticket numbers, and no mark
anywhere saying that everything below a line went out as a particular version.

The second half of that had never mattered, because nothing has ever been
released. It is about to be, and a release needs notes. Notes have to come from
somewhere, and there are only two possibilities: they are written a second time
when the release is cut, or they are the changelog. Written twice they will
disagree — which is the same argument
[ADR-0071](0071-the-record-of-finished-work-is-the-commit.md) made when it deleted
`DONE.md`, a second telling of what the commit message already said, at 43% of the
repository's prose.

So the file has to be readable by a script without stopping being readable by a
person, and it has to say where one version ends and the next begins.

## Decision

**Two shapes, and the file states them itself.** An entry and a release marker:

```
entry:  ^(\d{4}-\d{2}-\d{2}) (#MOLE-\d+) (.+)$
marker: ^## (\d+\.\d+\.\d+) — released (\d{4}-\d{2}-\d{2})$
```

Those expressions live in `CHANGELOG.md`'s own header, not in a script, and
`tests/scripts/tst_Changelog.sh` reads them out of it rather than keeping a copy —
so the file and the thing that checks it cannot come apart. Outside a fenced code
block, nothing else in the file may match either.

The rules that go with them:

- **An entry is the day it landed, the ticket, and a phrase.** A phrase and not a
  sentence: this is a list, and the reasoning is in the commit message.
- **One line per change, not one per ticket.** A ticket that fixed two things
  somebody would notice separately earns two lines. Several already have two, that
  is correct, and they are not to be tidied into one.
- **`#MOLE-22`, never `#22`.** GitHub turns `#22` into a link to an issue that was
  deleted on 2026-08-10, so a bare number in a published note is a dead link;
  `#MOLE-22` links to nothing and reads correctly as text. See
  [ADR-0022](0022-work-is-tracked-in-vikunja.md).
- **A version's notes are the block below its marker**, down to the next marker or
  to the end of the file. Newest first, so cutting a release inserts a marker at
  the top and the entries it contains fall underneath.
- **Every `##` line in the file is a release marker**, and there are no other
  second-level headings.
- **The prose bullets are the tail of the file** and belong to the first release
  whatever shape they are in. Nothing is added to them and nothing dated goes below
  them.

## Reason

**Every `##` line is a marker** rather than a list of headings that are allowed.
The failure to design against is a heading somebody adds in a year silently
becoming a release, and one rule enforces itself where a list of exceptions has to
be maintained by whoever adds the next one. It costs the two headings that would
otherwise be natural: `## Unreleased`, and a heading over the prose block. Neither
is a loss. Everything above the topmost marker is unreleased by definition, so that
heading says nothing the structure does not, and it would have to be moved on every
cut — one more step for a script to get wrong. The header carries the sentence that
explains the prose, which is where a reader is anyway.

**Prose stays prose.** The 145 lines have no dates and no tickets, and recovering
them from the git log would be archaeology in exchange for nothing: they are all in
the first release whatever shape they are in. The break is explained in the header
so that it reads as a break and not as rot.

The alternatives:

- **Write the release notes by hand at each cut.** Two lists, and the second one is
  written by whoever is cutting the release, in a hurry, from the first. They
  disagree within one release and there is no way to tell which is right.
- **Generate the changelog from the git log.** The commit messages here average 37
  lines and hold the diagnosis, the alternatives considered and the test — that is
  what they are for, and none of it is release notes. A changelog is an editorial
  act: which changes a reader would notice, said in their terms. A subject line is
  not that, and making it that would spoil the commit message for its own job.
- **Fragment files, one per change, rendered at release time** (towncrier and its
  kind). It removes merge conflicts in one file, which we do not have — this is one
  repository with one line of history. In exchange it adds a directory nobody reads,
  a build step between the record and the reader, and a second place to look for
  what changed. The file itself being the record is the property worth keeping.
- **Keep a Changelog's shape** — `## Unreleased`, `### Added`, `### Fixed`. The
  subheadings collide head-on with the rule above, and the categories start an
  argument per entry about which of them a change is. A date and a ticket are facts;
  "Changed" versus "Fixed" is a taxonomy.
- **A machine format with no prose at all** (conventional commits, a YAML list).
  The file would stop being one somebody reads for pleasure, which is most of what
  it is for today.

## Consequences

- The release notes for a version are a block of lines lifted straight out of the
  file, and there is no second list to disagree with them. Cutting a release is
  therefore inserting one line — which is `make release`'s job
  (`MOLE-118`), not this record's.
- **The oldest marker's block runs to the end of the file.** An extractor written as
  "between two markers" finds nothing for the only marker there is, which is exactly
  the state of the first release. `MOLE-123` owns that and fails the workflow on an
  empty block.
- Dates in the notes are the days changes landed, not the day the release was cut.
  That is the more useful fact and it is the one available when the line is written.
- A line in the wrong shape fails `make test` rather than being discovered by a
  release workflow. The check is a shell case over the file, so it costs nothing and
  runs on any machine.
- There is no release marker in the file yet, because nothing has been released.
  The marker expression is therefore held against fixtures rather than against the
  file — a check that could only be exercised after the first release would be a
  check nobody trusted on the day it mattered.
