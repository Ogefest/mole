# ADR-0022: Work is tracked in Vikunja, not in GitHub issues

- **Date:** 2026-08-10
- **Status:** Accepted, and amended by
  [ADR-0023](0023-work-is-dispatched-by-epic.md) on how work is dispatched — the
  queue is the order of the epics, not the order of `Ready`; and by
  [ADR-0071](0071-the-record-of-finished-work-is-the-commit.md) on where the record
  of finished work lives — `DONE.md` is deleted and the commit message carries it

## Context

Every bug, feature and owed test was a GitHub issue, and the order they should be
picked up in was a GitHub Projects board — five columns and a `Rank` field, read
and written on every dispatch.

The reading and writing is the problem. GitHub Projects has no REST API; it is
GraphQL only, and the GraphQL endpoint is rate limited by a points budget rather
than by request count. A board read costs points, moving one card costs several,
and the calls cannot be usefully batched because each one names a different item.
Ordinary work — look at `Ready`, move a card to `In Progress`, move it to `Done`
— ran into the limit, and a tracker you cannot reliably write to stops being
believed, which is worse than not having one.

The author already runs a self-hosted Vikunja instance on their own network: an
open source tracker with kanban views, labels, task relations and a plain REST
API on a machine nobody else shares.

## Decision

**The board moves to that Vikunja instance, and GitHub stops tracking work.**

- One Vikunja project, `Mole`, holding every ticket. The five columns are
  unchanged in name, order and meaning: `Backlog`, `Ready`, `In Progress`,
  `Blocked`, `Done`.
- **A task's number is the GitHub issue number it came from.** `MOLE-19` is what
  `#19` was. Gaps in the sequence — the numbers that belonged to pull requests —
  were left as gaps on purpose.
- The board's old `Rank` field is now the position of a card inside its column, so
  the instruction is *take the top card in `Ready`* rather than *find the lowest
  rank*.
- A GitHub milestone — a topic split across several tickets — became three things
  that together do what one milestone did: a label on each ticket, a column in a
  second `By epic` kanban view, and a task in an `Epics` sub-project whose
  subtasks are the tickets.
- The 67 issues were migrated with their bodies, checklists, labels, comments and
  open/closed state, then **deleted from GitHub**. The Issues tab stays enabled so
  somebody outside can still report a fault; anything arriving there is copied
  onto the board and closed on GitHub, so there is one queue and not two.
- A bot account writes to the instance over an API token. Which machine, which
  account and where the token is kept are not in this repository — they are in
  `~/dev/workspace/mole-pm/environment/`, the same place the test-server facts
  live.

## Reason

**Staying on GitHub and spending fewer points** was the first thing considered. It
does not work: the cost is per item touched, and dispatching work is exactly a
per-item activity. Caching the board would mean holding a copy of the thing whose
whole value is being current.

**A queue file in the repository** — `Ready` as an ordered list in git — needs no
API at all, and was rejected because it makes every parallel change a merge
conflict in the one file everybody touches, and because a file is not a board:
no columns, no card, nowhere for a comment to land.

**Another hosted tracker** would trade one rate limit for another company's, and
add a second public surface to keep clean.

**Vikunja** won because it was already running and costs nothing new to operate,
its REST API has no points budget, and it happened to have an exact home for
every part of the old board — buckets for the columns, per-view positions for the
rank, labels for the labels. The one thing it lacks is milestones, which is why a
topic is represented three ways rather than one.

The migration is a script rather than a screenshot of a board:
`mole-pm/tools/github-to-vikunja/`. It reads GitHub, writes Vikunja, then reads
the result back and compares the two — every title, label, column, order,
checklist and comment. It is kept, along with a snapshot of everything GitHub
held, because a migration nobody can check is a migration nobody can trust.

## Consequences

**The queue is no longer public.** This is the real cost, and it is paid
deliberately. Mole is an open source project and a contributor could previously
read what was planned and in what order; now they cannot. What answers for that
is the repository itself — `README.md` for what works, `ARCHITECTURE.md` for how
it is built, these ADRs for why, `CHANGELOG.md` and `DONE.md` for what happened —
plus an Issues tab that still accepts a report. If that stops being enough, the
answer is to publish a view of the board, not to move back.

> **Amended 2026-08-21 (MOLE-276): `DONE.md` is no longer one of them.** It was
> deleted, and what happened to a piece of work is now the commit that did it —
> including where the first answer was wrong. The sentence above is left as it was
> written, because it was true on 2026-08-10 and the point of these records is being
> able to see that we changed our mind;
> [ADR-0071](0071-the-record-of-finished-work-is-the-commit.md) is the change and
> carries the reasoning. The decision *this* record makes is untouched.

**Nothing closes a card by itself any more.** GitHub shut an issue when a commit
said `Closes #12`; no such wiring exists here. A commit message still says
`Closes MOLE-12`, because the git history should say what a change finished, but
moving the card is now a step the worker takes.

**The board lives on one machine on one network.** It is not reachable from
outside it, and it is a single point of failure with no replica. It needs backing
up; until it is, the snapshot in `mole-pm` is the only other copy of the tickets.

**The old issue URLs are gone.** Deleting the issues means every
`github.com/Ogefest/mole/issues/N` link 404s, including the ones in `DONE.md` and
in commit messages already written. The numbers still resolve, because the task
numbers were kept equal to them — which is most of the reason they were kept.
