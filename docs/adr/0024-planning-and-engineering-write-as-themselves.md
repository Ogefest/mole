# ADR-0024: Planning and engineering write to the board as separate accounts

- **Date:** 2026-08-10
- **Status:** Accepted

## Context

The board arrived through one machine account, and everything on it — every card
that was created, every comment, every move between columns — was attributed to
that one account. Planning and engineering are different activities that happen at
different times for different reasons, and a history that records both as the same
actor cannot answer *who decided this* or *who found that*.

## Decision

**Two accounts, each with its own token: one for planning, one for engineering.**
Both are bot accounts owned by the author, and each takes its token from
`VIKUNJA_TOKEN` in its own environment — the code and the working rules name the
variable and never a value.

The engineering account has **read and write** on the board, which is enough to
read the queue, move a card, comment, and open a task, and not enough to reshape
the board itself: its columns, its views, or the vocabulary of its labels. Those
are planning acts and stay with the planning account.

Which accounts exist and where their tokens are kept is in
`~/dev/workspace/mole-pm/environment/vikunja.md`, outside this repository, like
every other fact of that kind.

## Reason

**One shared account** is what this replaces. It costs nothing to run and makes the
board's history worthless as a record of who did what — which matters more here than
it would elsewhere, because most of the writing is done unattended.

**Named human accounts per person** is the arrangement this imitates and would be
the right answer with more than one person. There is one, working through two roles,
so the accounts follow the roles.

**Giving the engineering account admin rights** would have avoided one sharp edge:
a label can only be attached by an account that can already see it in use, so a
brand-new label is out of reach and the API says nothing more helpful than `403`.
That edge was kept on purpose. Introducing a word the board did not have is
planning, and the failure is loud rather than silent.

## Consequences

**The history separates.** A comment on a card says which role wrote it, and a card
that moved says which role moved it, without anybody having to write it down.

**A second credential exists**, so there is a second thing that can leak and a
second thing to rotate. Both live in one file outside the repository, and neither is
ever passed on a command line that a shell might record.

**An unused label is now worse than useless** — it is invisible to the engineering
account, so it cannot be used by the account most likely to want it. The migration
tool therefore creates no label that no task carries, and `blocked` was dropped for
exactly this reason: blocking is better said with a `blocked`/`blocking` relation
between two tasks, which Vikunja has and a label only approximated.
