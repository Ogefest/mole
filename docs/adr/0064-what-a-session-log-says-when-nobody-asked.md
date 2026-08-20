# ADR-0064: What a session log says when nobody asked for detail

- **Date:** 2026-08-20
- **Status:** Accepted
- **Amends:** [ADR-0012](0012-a-log-you-can-turn-up.md), for `mole.task` only

## Context

[ADR-0012](0012-a-log-you-can-turn-up.md) settled four logging categories and the
rule **silent at debug, audible at warning**: the running commentary costs nothing
until somebody asks with `MOLE_LOG`, and a failure is written whether they asked or
not. That was right about the commentary and about failures. It left a gap in the
middle.

A session log from an ordinary run held two lines about what Mole had been doing:
where the log was going, and — only if `MOLE_LOG` had been set — which categories
were recording. Every job that started, finished, was cancelled or took forty
seconds went through `Task::execute`'s two `qCDebug` lines, so none of it reached
the file unless somebody had switched it on *beforehand*. That is the one thing
nobody has done before the run that goes wrong.

So the lines existed, they were already uniform for every job that will ever be
written — which is the whole reason they live in `Task` rather than in each task —
and they were off. Only an outright failure got through, as the `qCWarning`.

The reason they were off is in the code at `Task.cpp:86` and it is a good one: the
browser cancels a listing every time the folder changes or a keystroke narrows a
filter, so a line apiece would bury the copy somebody opened the log to find.

## Decision

**A task says whether it is one of a crowd, and that decides the level.**

`Task::isOneOfMany()` is a new question beside `isBackground()`, answering *this job
is one of many identical ones that one gesture produced, and this particular one is
of no interest*. `Task::execute()` writes its start and outcome lines with `qCInfo`
when a job is **neither** one of a crowd **nor** background, and with `qCDebug`
otherwise. The categories are already declared with `QtInfoMsg` as their default
level, so info reaches the session log with nothing switched on.

**Both questions, and the second was learned by running it.** With only
`isOneOfMany()` consulted, a plain eight-second start wrote forty-two lines of which
thirty were `Check free space on …`: `QuerySpaceTask` is background but is not a
crowd in the sense above, and it runs once per mount every minute for as long as the
window is open. That is ADR-0012's burial arriving through a different door. So the
log asks both — housekeeping the user did not ask for is not something they would
look for either. The two properties stay separate; it is the log that wants both
answers to be no.

**False by default — the loud answer.** Four types say otherwise for themselves:
`ListDirectoryTask`, `ThumbnailTask`, `ReadMetadataTask` and `ReadRangeTask`.

`MOLE_LOG=task` still turns on everything it turned on before. Cancellation stays
out of the warning path, which is what MOLE-40 was about.

## Reason

**Why a property on the task rather than a list of titles or types in `execute()`.**
The one place that writes these lines should not have to know what kinds of task
exist; that is the reason ADR-0012 put them there. A task that knows it comes by the
hundred is the only thing that knows it.

**Why not fold the two into one property.** They answer different questions about
different jobs, and both answers are needed separately even though the log happens to
want both. A thumbnail is *not* background — opening a folder
of photographs is exactly asking for it, and it belongs in the task strip — but it is
one of three hundred. A periodic free-space check is background and would also be one
of a crowd. Collapsing them would either put housekeeping in the log or take
thumbnails out of the strip, and neither is wanted.

**Why the default is loud.** The alternative is a list of task types that opt *in* to
being logged, and such a list is one somebody forgets to add to. The failure mode of
this direction is a line too many in a log; the failure mode of the other is the run
that went wrong being the one that said nothing. A type written next year is in the
log without anybody remembering.

**Why info and not warning.** A job that ran and succeeded is not a problem, and
warning is what a reader scans for. ADR-0012's `audible at warning` is about things
that went wrong and is unchanged.

**Naming.** `isOneOfMany()` says what is true of the job. It was going to be
`isQuiet()` or `isDebugLevel()`, both of which name the consequence and would read as
a lie the first time something else consults it — which the task strip now does, for
its own reasons and not for the log's.

## Consequences

- A session log from a plain run says what ran, how long each job took, and how each
  one ended. That is what a report has to carry before anybody can read it.
- Browsing is still quiet. A folder of three hundred photographs writes three hundred
  thumbnail lines only under `MOLE_LOG=task`, as before, and a window left open all
  afternoon does not fill the file with free-space checks.
- **A bare start still writes two lines**, because a start that does nothing has done
  nothing: every task a plain launch runs is either housekeeping or a listing. What
  MOLE-263 adds is the other half — what Mole *started with*, which is a fact about
  the run rather than about a job.
- The other three categories are untouched. `mole.drive`, `mole.net` and `mole.curl`
  remain silent at debug, and this record says nothing about them.
- A second consumer of `isOneOfMany()` arrived immediately: the task strip counts
  finished jobs, and a crowd made the count move with what happened to be in the
  thumbnail cache — see MOLE-258. That the property was named for its meaning rather
  than for a log level is why it could be used there without reading oddly.
- `CapturedWarnings` in the test support library now takes the quietest level to
  capture. It defaulted to warnings and could not see an info line, so neither of the
  two claims here — *this ran and left a line*, *this one left none* — was testable
  before.
- A log is a thing people send to other people. Nothing in these lines is new
  material: a task's title and elapsed time were already written under `MOLE_LOG`,
  and a title is words the application chose.
