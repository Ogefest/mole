# ADR-0071: The record of finished work is the commit

- **Date:** 2026-08-21
- **Status:** Accepted

Revises [ADR-0022](0022-work-is-tracked-in-vikunja.md) in part: the role that record
gave `DONE.md` no longer exists.

## Context

`DONE.md` held a long account of every finished task — what was asked for, what the
answer turned out to be, and the times the first answer was wrong. It was 6,920 lines
and 441 KB across 187 entries, which is **42% of every byte of Markdown in this
repository** and larger than the other 89 files together. Nothing in `README.md` or
any page of the guide linked to it, so no reader ever arrived at it; what it reliably
did was fill a session's context.

Measured against what it duplicated: those entries average 37 lines, and the average
commit body here is **37.9 lines over the last twenty commits** and covers the same
ground — the diagnosis, the alternatives rejected, the measurement, which test was
added and why. `Closes MOLE-n` ties each commit to the ticket, and the ticket holds
the brief in full. For all but a handful of entries there was nothing in the file that
was not also in the commit or on the card.

**The one thing an immutable log cannot do is correct itself**, and that is the
argument that had to be answered before deleting anything rather than after. Seven
entries were identified as correcting an earlier account, and each was checked against
its own commit:

| Entry | Commit | Carries the correction |
|---|---|---|
| A directory moved into itself on one drive | `810ba70` | yes, in as many words |
| Four of the five standing tools opened a second tab | `8e8d407` | yes |
| A dialog flashed a scrollbar it did not need | `23433d8` | yes |
| Two ways of building a set each opened another tab | `2495f9b` | yes |
| A killed heavy run left gigabytes on the test server | `6e57c14` | yes, three of them |
| The provisioning scripts had no test | `a6566da` | yes, two wrong turns |
| Six labels in the search form were on the wrong row | `e9f5d57` | **nothing to carry** |

The last one is worth stating precisely, because it makes the case stronger rather
than weaker: read again, that entry corrects nothing. It records a limitation — one of
seven spans that the new test cannot witness — and a fault it uncovered, and both are
in the commit message too. So of the seven accounts that only this file could have
held, six existed and all six are in their commits.

## Decision

**`DONE.md` is deleted, and nothing replaces it.** The record of finished work is four
things that already exist, each holding what it is good at:

- **the ticket** for what was asked for, in full;
- **the commit message** for what the answer turned out to be, *including where the
  first answer was wrong*;
- **an ADR** for a decision about shape;
- **`CHANGELOG.md`** for one line per user-visible change.

`TODO.md` stays and is not affected. It is 303 lines, a twentieth of the size, holds
what is deliberately *not* a task — behaviour decided to live with, conventions, gaps
documented rather than scheduled — and is linked from seven ADRs, `README.md`, the
guide and `tests/scripts/tst_ShellScripts.sh`. None of its content is anywhere else,
which is the test `DONE.md` failed.

## Reason

**Why not shorten it instead.** Two hundred entries at ten lines each is still a
second telling, and a file whose entries are trimmed to a summary is worse than both
alternatives: shorter than the commit it duplicates and still a place to look. The
duplication was the problem, not the length.

**Why the commit message and not the ticket.** A ticket is written before the work and
is the right place for what was wanted; the account of what the answer turned out to
be belongs where the change is, so `git log -p` reads as one story and `git blame`
leads to the reasoning for the line under the cursor. That is also what makes the
correction case work: a commit cannot be edited, but the *next* commit can say the
last one was wrong, and six of them already do.

**Why a deletion and not an archive directory.** A deleted file is not a destroyed
one. `git show <sha>:DONE.md` answers at every commit that ever had it, and the last
one is `4c9a8cc`. Moving it to `docs/history/` would keep every byte in every
checkout and every context window, which is the cost being removed, in exchange for a
path nobody links to either.

**Why this revises ADR-0022 rather than filling a gap.** That record described the
arrangement replacing GitHub as *these ADRs for why, `CHANGELOG.md` and `DONE.md` for
what happened*. The third of those is gone, so the sentence no longer holds and
somebody reading ADR-0022 needs to be sent here.

## Consequences

- `TODO.md` and `CHANGELOG.md` lose their links to it; both said "the long account
  lives in `DONE.md`" and now say where it lives instead.
- **ADR-0022 keeps both its mentions of `DONE.md`, and gains a pointer here.**
  `docs/adr/README.md` says a record that no longer holds is not edited or deleted, and
  also says to link both ways — so the sentence naming `DONE.md` stays as written,
  because it was true on 2026-08-10, and a dated note beside it says which clause was
  withdrawn. Its status stays `Accepted` rather than `Superseded`: the decision it
  makes — that work is tracked in Vikunja — holds in full, and only a clause in its
  *Consequences* is being taken back. The shape is the one ADR-0032 took when
  MOLE-259 found its list of standing tools predated the fifth tool, with one
  difference worth naming: that was a list that had gone stale and was amended in
  place, this is a role being withdrawn, so the change lives here and the old record
  only points at it. So a `grep` for `DONE.md` over the tree still finds ADR-0022 twice
  and that is correct rather than missed.
- **A commit message is now load-bearing**, which it already was in practice. A change
  whose reasoning is not in its commit has nowhere else to put it, and "no design
  discussion in `CHANGELOG.md`" now points at the commit rather than at a file.
- Nothing in `CHANGELOG.md` for this: it is the release notes, and no user of Mole can
  see it.
