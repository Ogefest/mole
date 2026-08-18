# ADR-0044: A keep rule says what it did, applies per group as well as globally, and *keep* and *remove* are both said out loud

Date: 2026-08-18

Status: accepted

## Context

Choosing what to keep is the hard half of deduplication, and the view said so in
its own header comment: *Choosing what to keep is the hard half, and this never
picks for you.* It then gave that job four flat buttons in a row wedged between
the options panel and the results, with the least visual weight of anything on
the screen.

The choices themselves are the ones people actually make — newest, oldest,
nearest the top of the tree, nothing. What was missing was any sense of what a
choice *did*: a rule was applied silently across every group at once, and the
only feedback was a count and a size in the corner. Per-group override existed
only as ticking checkboxes by hand.

MOLE-72 named three questions and asked for them to be answered rather than
assumed. This records the answers.

## What a rule applied to fifty groups looks like while you check it

**A sentence, and a mark on every row.**

The panel states the rule in words and gives the count something to be a
fraction of — *Keeping the newest of each group — 24 of 26 copies ticked for
removal, 1.2 GB*. A bare "24 ticked" is a number; a rule cannot be checked
against it.

Each group then marks its own outcome: in a group where anything is ticked, every
copy reads either *keeping* or *remove*. That is what makes fifty groups
checkable by scrolling instead of by counting checkboxes, and it is why the mark
is on the row rather than only in the summary.

A group where nothing is ticked says *not decided* rather than marking every copy
as kept. Before anything has been chosen every copy really is equally kept, and
fifty rows all reading *keeping* would be noise on a screen where no decision has
been taken.

**The rule stops being the rule the moment the ticks are edited.** One checkbox
changed by hand and the sentence reads *Chosen by hand*. Going on saying "keeping
the newest" over ticks somebody has since changed would be the view asserting
something untrue about what pressing Delete will do — and this is the screen where
that assertion costs files.

## Whether a rule belongs per group as well as globally

**Yes, as an outcome; no, as a second set of controls.**

Each copy carries *Keep this one*, which keeps that copy and ticks the rest of its
group and nothing outside it. A rule that is right for forty-nine groups and wrong
for one should not have to be abandoned for the whole scan, and the override is
one click on a row that is already on the screen.

The alternative was the four rule buttons repeated per group. It loses on
arithmetic: fifty groups is two hundred controls, all to express what one click on
the row already expresses, and the rule names — *newest*, *oldest*, *nearest the
top* — are ways of picking a winner out of a list, which is a job that stops being
worth naming when the list is three rows you can see.

The per-group override is held to the same rule the global ones are: something in
every group survives. A control that could tick every copy of a group would offer,
with one click, to delete the file entirely.

## Whether *keep* or *remove* is the honest verb

**Both, said in different places, because they are different facts.**

The rule is stated as *keep*, because that is how the decision is made — nobody
thinks "remove all but the newest", they think "keep the newest".

The ticks are stated as *remove*, because that is what a tick does. The row says
it in a word, the checkbox says it when hovered, and the button is *Delete ticked*.

Saying only one of them was the actual fault: pressing **Newest** under a heading
reading **Keep** put a tick against every file *except* the newest, and nothing on
the screen said which of the two readings a tick carried. Choosing one verb and
enforcing it everywhere was considered and rejected — "Remove all but the oldest"
is a worse button than "Oldest" under "Keep", and relabelling the rules in terms
of removal would make the common case read backwards to make the rare case read
right.

One consequence worth naming: the fourth rule button was *Nothing*, which under
*Keep* meant "keep nothing" — the opposite of what it did, which is to untick
everything. It is *Everything* now.

## Consequences

- The keep controls are a panel with the weight of the options panel above them,
  rather than four flat buttons in a strip. It is the control that decides what
  gets deleted, and it now looks like it.
- `Make a set` and `Delete ticked` sit in that panel, so the two ways out of the
  view are beside the choice that decides what they act on.
- `ruleText` is on the controller rather than worked out in QML, because the fact
  it records — which rule produced these ticks — is knowledge only the controller
  has once the ticks are in a set keyed by uri.
