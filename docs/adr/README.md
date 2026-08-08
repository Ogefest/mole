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
