# ADR-0023: Work is dispatched by epic, not from a flat queue

- **Date:** 2026-08-10
- **Status:** Accepted
- **Amends:** [ADR-0022](0022-work-is-tracked-in-vikunja.md), which said dispatch
  was *take the top card in `Ready`*. The rest of that record stands.

## Context

`Ready` held 33 tasks in one deliberate order, and taking the top card meant the
order alone decided what came next. Read down that column, though, and the work
jumps between subjects: a preview fault, then six tasks about dragging files, then
three about duplicates, then one loose UI task, then two about fault injection,
then one test group, then two more loose ones, then the rest of a test catalogue
fifteen tasks long, with two more test groups after it.

Every one of those runs is a topic that was split into several tasks because a
single task would have been too big — and the split is the thing the flat order
loses. Coming back to a subject after two unrelated tasks means paying for the
context twice, and the tasks in one epic usually share the code they touch.

## Decision

**The queue is the `To-Do` column of the `Epics` board.** The topmost card there
is the epic being worked on, and it stays that way until the epic has nothing left
in `Ready`. Within an epic, the tasks are taken in the order they sit in `Ready`.

So there are two orders answering two questions — the Epics board says which topic,
`Ready` says which of its tasks — and each is reordered by dragging cards in its own
place. An epic with nothing in `Ready` is skipped: whatever it has left is in
`Backlog` or `Blocked`.

**Every task belongs to exactly one epic.** Twenty-one did not, four of them
dispatchable and one of them the next thing to do, and under a rule that reaches
tasks through epics those would have been unreachable for ever. They are in an epic
called `Loose ends` — a single fault, a single small change, nothing larger. The
`no epic` column of the `By epic` view is kept, empty, as the tell-tale: a card
there is a task nobody would ever reach.

The first order of the epics was read off the last GitHub board rather than
invented: each epic sits where its first `Ready` task sat, which is why the next
task to do is the same one it was before the rule changed.

## Reason

**Ordering `Ready` so that it happens to read epic by epic** would give the same
result today and no guarantee of it tomorrow, because nothing would stop the next
insertion from splitting a run. It also puts the priority of a topic in 33 places
instead of one: promoting an epic means moving all of its cards.

**Sub-projects per epic** — one board each — was rejected because it destroys the
single `Ready` queue and the one place to look, and because a task would then move
between projects as it is grouped and regrouped.

**Leaving the loose tasks outside every epic** and dispatching them separately
would need a third rule, and a rule that exists to catch what two other rules miss
is a rule somebody will forget. An epic for them costs one card.

## Consequences

**Priority is now a property of a topic, not of a task.** Promoting a subject is
one drag on the Epics board. The cost is that a genuinely urgent single task in a
low epic has no way to jump the queue except by being moved into `Loose ends` or by
the epics being reordered — which is a planning act, and is meant to be visible.

**An epic can now be half-finished for a long time.** Staying in one until it is
empty is the point, but `Testing: phase 5` holds fifteen open tasks, and while it is
the top card nothing else moves. If that turns out to be wrong, the answer is to
split the epic rather than to abandon the rule.

**The `Epics` board is load-bearing.** It was a way of seeing the work grouped; it
is now the thing that decides what happens next, so its order is checked by the
migration tool's verifier along with everything else.
