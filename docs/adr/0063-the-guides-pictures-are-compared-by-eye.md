# ADR-0063: The guide's pictures are compared by eye, not by bytes

- **Date:** 2026-08-20
- **Status:** Accepted

## Context

Every picture in the user guide is a window the test suite photographed
immediately after asserting that what it shows is real. `make guide-images` takes
them again and copies them over the committed ones.

It rewrote most of the guide whether or not anything had changed: **fifty of the
fifty-three files, over an unchanged tree**, measured on 2026-08-20. So a ticket
that moved one thing on screen produced a diff of fifty binary files, of which one
was the change. Nobody could tell whether the picture that mattered had moved, and
a regression in an unrelated picture was invisible. It had already cost a ticket:
MOLE-209 asked for the pictures to be regenerated if the sidebar was in any of
them, and the honest answer was to read all fifty-three by hand and decline to
regenerate, because committing thirty-two files of noise was worse.

Seven separate things were moving. In the order of how many pictures each one
touched:

- **the drives' free space**, read from the real disk, in thirty-nine of them —
  `135,81 GiB free` became `135,80 GiB free` between two runs minutes apart;
- **the clock**, in the date column of everything the run created rather than the
  fixture. The fixture already fixed its own dates, by a hand-written list of
  paths — and the list was incomplete, which is what a hand-written list of that
  kind becomes;
- **the order of a list**, in the sync plan and inside a duplicate group, both of
  which came out in whatever order the filesystem listed them;
- **the terminal's prompt**, which was the real shell of whoever ran the suite;
- **the text caret**, which blinks;
- **the task strip**, which is in every picture and says how many jobs have
  finished;
- **an animation's tail** — a scrollbar that fades itself out on a delay rather
  than in a transition, so `grab()`'s two-identical-frames rule could stop while it
  was still faintly there.

## Decision

Each of those is fixed at its source, and the *check* compares what an eye can
see rather than bytes.

`tests/tools/compare-shots.cpp` is a small Qt program that takes two directories
of pictures and two numbers: a pixel counts as different when a channel differs by
more than `--tolerance` (8 levels), and a picture counts as changed when more than
`--pixels` of them do (0). `scripts/check-screenshots.sh` takes the pictures twice
over an unchanged tree and compares the two, and `make screenshots-check` runs it.
The pictures that cannot be identical twice running are named in that script with
a reason each, and nothing else may differ.

`make guide-images` copies only the pictures that changed by that measure, so the
commit holds the change rather than the noise.

## Reason

**Byte-identical was the goal and it is not reachable.** With all seven causes
fixed, what remained between two runs was one to five levels out of 255, in a few
dozen pixels, in pictures whose content was letter-for-letter identical: Qt's scene
graph does not render a given frame to the same bytes twice. Forty-six of the
fifty-three do come out byte-identical, and which seven do not moves from run to
run — so a byte comparison with a list of exceptions would be a flaky check, and a
flaky check is worse than none because it teaches everyone to ignore red.

**A tolerance on how different a pixel is, not on how many.** One pixel more than
eight levels out fails. The alternative — allowing a few dozen changed pixels —
would hide a one-word change in a label, which is exactly the kind of regression
this exists to catch.

**Qt rather than ImageMagick or a Python library.** Qt is already the dependency,
so the comparator needs nothing installed that building Mole did not already need.
`compare` and Pillow were both to hand on the machine this was written on, which
is not the same as being to hand for a contributor.

**A wait for quiescence rather than hiding the task strip.** The strip could have
been left out of the comparison, but it is in every picture, so an unstable strip
puts every picture at risk rather than a nameable few. Waiting for the count to
stop moving makes it "everything the walkthrough has run so far", which is fixed.
The three pictures that are *of* something still working say so at the call site —
`Settle::Working` — because waiting there would photograph the finished state and
break the rule that a picture shows what the assertions just checked.

**Dates derived from the path rather than a longer hand-written list.** The list
that existed was already missing three files that were in photographed views. A
list of what must not carry the clock goes stale the first time somebody adds a
fixture file, so the default is now a fixed date derived from the path, with the
hand-written entries kept as the dates the guide's prose refers to. The hash is
FNV-1a of our own rather than `qHash`, whose seed is not promised to be stable
between Qt versions: an upgrade must not rewrite the guide.

## Consequences

- Regenerating the guide is reviewable: `make guide-images` rewrites the pictures
  that changed, and `make screenshots-check` says whether anything moved on its
  own.
- Two of the fixes are behaviour changes, not test scaffolding, and both are
  improvements a user would want: a sync plan and a duplicate group are now in a
  stable order, so a plan somebody reads before agreeing to it can be compared
  with the last time they read it.
- **A real user name and machine name were committed in `10-terminal.png`** —
  `$SHELL` was whoever ran the suite. The panel now starts a shell with no rc
  files and a prompt of our own, which fixes the leak and the picture together.
  It is worth knowing that this class of leak reaches the repository through
  *pictures* as well as through text.
- A picture that is genuinely of something in motion has to be named, with a
  reason, in one place. That list is a claim about the picture and not a place to
  put things that are merely hard, and it is short: three pictures of work in
  progress, three rows that say when or how long, and one that waits on MOLE-258.
- The tolerance could hide a change of one to eight levels across a whole picture
  — a barely-shifted background colour, say. Nothing in the guide is that kind of
  change, and the alternative is a check nobody can keep green.
