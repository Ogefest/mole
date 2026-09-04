# Architecture decision records

One file per significant decision, so that the reasoning behind the shape of
Mole survives the moment it was made. Anything that would make a future reader
ask *why is it like this?* belongs here: choosing a library, adding or reshaping
an extension point, changing a file or config format, dropping a platform,
taking on a dependency, reversing an earlier decision.

Name files `NNNN-short-title.md`, numbered in sequence. Numbers are never
reused and never renumbered. A decision that no longer holds is not edited or
deleted — write a new record, mark the old one `Superseded by ADR-NNNN`, and
link both ways. Being able to see that we changed our mind, and why, is the
point.

Records are written in English, like everything else in the repository.

**Two records were given 0035 on the same day**, which is the one thing this
naming rule cannot survive: a reference to "ADR-0035" could not be followed.
The later of the two was renumbered to
[0100](0100-the-details-are-a-drawer-and-one-setting.md) on 2026-09-04 and says
so at the top, and `tests/scripts/tst_Documents.sh` now refuses a number that
appears twice, so the next collision is caught before it is committed rather
than a year later. See MOLE-402.

**The reasoning section may be one named question rather than a heading called
`Reason`.** Three records ask theirs directly -- "Why not a row of its own",
"Whether *keep* or *remove* is the honest verb" -- and that is better writing
than a generic heading, not a deviation. What is not optional is that the
alternatives are named and disqualified somewhere: a record that only says what
was decided leaves the next reader unable to tell whether the reasoning still
holds, which is the whole purpose of keeping these.

## Template

```markdown
# ADR-NNNN: Short title

- **Date:** YYYY-MM-DD
- **Status:** Accepted | Superseded by ADR-NNNN

## Context

What forced a decision: the problem, the constraints, what was already true.

## Decision

What we are doing, stated plainly.

## Reason

Why this option and not the others. Name the alternatives that were considered
and what disqualified them, so a reader can tell whether the reasoning still
holds.

## Consequences

What this makes easy, what it makes hard, and what we have committed to.
```
