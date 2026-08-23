# ADR-0082: A chain is a line, and a list of uris is what passes along it

- **Date:** 2026-08-23
- **Status:** Accepted

## Context

Mole has a dozen operations and no way to say what any of them takes or produces.
`TransferTask` is constructed with a mode, a list of uris and a destination;
`CompressTask` with a format and an archive name; `FindDuplicatesTask` with roots
and a strategy. Each is fine on its own, and none of them can be asked the one
question a sequence needs answered: *what do you need, and what will you hand
back*.

So *find the files, then compress them, then move the archive* is three decisions
in three dialogs, and nothing joins them or repeats them. Making that one thing
somebody can build, save and put on a clock needs two decisions before any code:
what passes between steps, and what shape a sequence is allowed to be.

## Decision

**A list of uris is the only thing that passes between steps.** No records, no
typed payloads, no key-value bag travelling down the line.

**A chain is a line.** No branches, no joins, no conditionals, no variables, no
expressions. A step has one of three roles -- a source takes nothing and gives a
list, a transform takes a list and gives a list, a sink takes a list and gives
nothing -- and a sink can only be last.

**A step hands on what it produced, not what it consumed.** Compression given
twelve files hands back one archive.

Refused in the code rather than only here: `ChainRegistry::isRunnable()` rejects a
sink that is not last, a source that is not first and a transform with nothing
before it, each with a message naming the step. The precedent for a refusal that
lives in the code is `parseQueryLine()`, whose header states the same about boolean
operators.

## Reason

**The list of uris is not a new idea, which is the strongest thing about it.** The
shell already asks the current tab for the things to act on as a list of uris
through `targetUris()`, and ARCHITECTURE.md records that this is exactly why a file
set costs nothing: a set answers the same question a pane's selection answers, so
every operation takes one without a line changing. A chain is that same list handed
on rather than acted on once. Anything richer would be a second currency, and the
operations would then have to learn which of the two they were being handed.

The alternative was a typed payload -- rows for a duplicate report, matches with
line numbers for a search -- so that a step could receive what the previous one
*knew* rather than only what it produced. Rejected because it makes every pair of
steps a compatibility question, and the answer to most pairs would be no. A list of
uris makes every transform composable with every other by construction, and a step
that needs more than the uri can read the file: it is a file manager, and the files
are right there.

**A line rather than a graph, because each addition arrives looking small.** A
condition, then a branch, then a variable to decide the branch, then an expression
in the variable -- and what they add up to is a scripting language with no
debugger, inside a file manager, running on a clock over somebody's disk. There are
tools for that and they are better at it. What a line can do is the common case:
find, filter, act, and put the result somewhere. Somebody who needs a graph needs a
different tool, and saying so plainly is kinder than half of one.

**Three roles rather than a flag per step**, because *a sink can only be last* is
then a property of what a step is rather than a setting somebody can get wrong --
and "a report on a clock" becomes an ordinary one-step chain instead of a special
case. The registry checks it, so a chain that would drop its work on the floor is
refused when it is built rather than discovered when it runs at three in the
morning.

**Hands on what it produced** is the rule that would otherwise be found by
accident: a chain that compresses and then moves must move the archive. Getting it
wrong moves the twelve originals and leaves the archive behind, which is a data
loss dressed as a feature.

**The registry is contributable** for a plain reason: `CompressTask` lives in the
archive plugin, so a registry that only knew about core could not offer compression
at all. It follows `Scheduler::registerJob()` exactly, including allowing a kind to
be replaced, so a plugin reloading does not orphan the chains that name it.

## Consequences

A step kind can be described, listed, saved and validated before anything can run
one, which is what lets the interface and the runner be built against a vocabulary
instead of against each other.

What this makes hard is deliberate. A step that wants to tell the next one *why* a
file is here -- the matching line, the duplicate group it belongs to -- has nowhere
to put it, and will have to look again. That is the cost of one currency, and it is
paid in cycles rather than in correctness.

Chain-only properties are the pressure valve, and they are the thing to watch: the
first is whether an empty result stops the line. They belong to the step kind and
are set on the step, so the chain never configures behaviour from outside. If that
list grows past a handful, it is evidence the line is being asked to be a graph, and
the answer then is still no.
