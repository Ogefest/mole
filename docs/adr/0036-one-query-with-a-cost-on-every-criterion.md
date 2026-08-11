# ADR-0036: One query, with a cost on every criterion

- **Date:** 2026-08-11
- **Status:** Accepted

## Context

A search was described twice. `IndexSearchQuery` held text, extension, volume,
case sensitivity, dirs, files, min and max size and a limit; `LiveSearchTask::Criteria`
held the same list without the volume. Nothing kept them in step but hand and
memory, and each had its own evaluator — one in SQL, one in C++ — so a criterion
had two meanings and two chances to be wrong.

ADR-0005 wrote down what that would cost when it chose to keep two engines:

> Two engines answering the same form means two paths for every criterion …
> anything the index cannot express has to fall back to walking, and that has to
> be said out loud in the status line rather than silently ignored.

The epic this sits in adds a date range, a type class, metadata fields and a
content match. Four criteria, added to two structs, in two languages, with a
third place deciding which engine can express each — before any of them is
written, that is where the wrong answers were going to come from.

There is a second problem the old shape could not state at all. *PDFs containing
"invoice"* is two criteria of wildly different price: one is a column, the other
opens the file. A struct of optional fields has no way to say which is which, so
the only implementation it admits is the one that reads every file.

## Decision

**One `SearchQuery`, a list of predicates, and a planner that splits it per
source.**

- `SearchPredicate` says what it matches and what it costs. `PredicateCost` has
  four rungs — `PushedDown`, `Cheap`, `Metadata`, `Content` — and the ladder is
  the order of evaluation.
- Cost is a **member**, not a property of the field. The same criterion costs
  different things in different places: a camera model is a column on an indexed
  volume and a header read on one that was never scanned.
- `SearchPredicate::matches(FileEntry)` is a pure function. Every criterion the
  search grows proves itself there, once.
- `planSearch(query, source)` returns what that source will state in its own
  query and what is left over, cheapest first, stable within a cost class.
- **Nothing is ever dropped.** A criterion a source cannot push down is
  evaluated afterwards, and the plan says it was left over — which is what the
  status line owes the user and what a form greying a field owes them as a
  reason.

`IndexDatabase::search()` builds its `WHERE` from the pushed-down half;
`IndexSearchTask` and `LiveSearchTask` both run the remainder through the one
evaluator.

## Reason

**A list of predicates rather than a struct of optional fields**, because the
planner has to reason about criteria one at a time and a struct cannot say *this
one is expensive, do it last*. It also ends the third state every optional field
has: a `minSize` of -1 meaning "not asked for" is a convention every reader has
to know, and a predicate that is simply absent is not.

**A cost on the predicate rather than a lookup keyed by field**, because the
lookup has to grow a second key — the source — the moment metadata arrives, and
then a third when it varies by volume. Putting it on the value keeps the planner
a sort.

**A walk pushes nothing down.** It lists a directory and looks at what came
back, so every criterion is one it evaluates. The alternative — calling the
walk's own filter a push-down — would give the same evaluator two names and two
code paths to keep in step, which is the fault this record exists to end.

**The remainder degrades to `Cheap`, not to expensive.** A criterion the source
declined does not become dearer; it becomes something we do from the entry we
already have. What was going to cost a read still costs a read.

**Rejected: one engine.** Making the index answer everything means it must hold
everything, and file contents are not going into it (see the epic's own two
positions). Making the walk answer everything throws away the reason the index
exists. ADR-0005's split stands; this record is about the query, not the engines.

## Consequences

- A criterion is added in one place — a named constructor, a case in the
  evaluator, and a line in whichever source can state it. `tst_SearchQuery`
  holds the evaluator and `everyCriterionAnswersTheSameThroughBothEngines` asks
  one fixture tree the same question through both, which is what stops the two
  drifting again.
- The index does not yet state a date range, though it has the column. It is
  evaluated on the way out instead — slower, and right — until the form that
  asks for dates arrives to add the clause. That asymmetry is now visible in the
  plan rather than hidden in a comment.
- The folder a search is scoped to is a predicate (`underPath`), not a filter
  applied by the caller afterwards. The indexed path used to narrow its own
  answer by hand in the controller; that had to be repeated by every future
  caller, and now is not.
- The two engines' result caps were 5000 and 10000, chosen independently and
  documented nowhere. They are one number now — 10000 — because two limits on
  one question is the same fault in miniature.
- `MOLE-153`'s form and the status line have something to read: the plan says
  what was not pushed down, so *this criterion made the search walk* can be said
  out loud rather than guessed at.
