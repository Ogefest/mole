# ADR-0067: A search is one box, and scope is part of the query

- **Date:** 2026-08-21
- **Status:** Accepted

## Context

The search tab has a query line above a form, and the two are one query seen twice:
`parseQueryLine()` turns the line into the fields and `rewriteQueryLine()` turns the
fields back into the line, so neither can drift from the other. The line already takes
the whole vocabulary — thirteen keys plus every metadata key the scope records, with
`>` and `<`, negation, quoting, lists and regular expressions.

Which means the common case can be one box, and is not. In front of **More** sit
*Name contains* with its mode picker, *Extension*, and the *Search in* picker beside
the folder field. The keyboard goes to *Name contains* rather than to the line, and the
comment above `focusActivePane()` says the box the tab exists for should have the
keyboard from the start — while pointing at the wrong one, because the line has no `id`
and the name field is called `queryField`. A walkthrough assertion pins the mistake in
place.

Asked for by the author from use: without **More** it should be one box that searches
the way Everything does, and the scope picker reads as a second way of saying what the
folder field beside it already says.

**Scope is the one thing the line cannot say**, and that is what makes this a decision.
`everywhere` is a property set only by that picker; the folder is `rootUri`, shown in
the field next to it. So the picker is not duplication — it is the only way *Everywhere
indexed* and the volume picker are reachable at all, and removing it needs an answer
rather than a deletion.

## Decision

**The basic view is the line, and nothing else that takes a query.**

- *Name contains*, the name-mode picker and *Extension* move into **More**. They keep
  their object names and every field they had: what moves is a control, not what is
  stored, because `saveState()` writes the fields and not the widgets.
- The keyboard starts in the line — from `focusActivePane()` and from the tab opening.
  This lands with the move rather than after it, because `forceActiveFocus()` on a
  field inside a hidden panel does nothing at all, so moving the form first would open
  a tab with the keyboard nowhere.

**Scope goes into the vocabulary, and the root is shown rather than chosen.**

- `everywhere:yes` is a key like any other, with `yes` and `no` values the way `hidden`
  has, offered by the same completion and written back by the same rewrite. So
  *Everywhere indexed* is reachable by typing, which is where the rest of the query
  already lives.
- The basic view **says** where the search is aimed — the folder, or that it is every
  indexed volume and which one — and does not offer it as a choice. Choosing is in
  **More**, which keeps the *Search in* picker, the editable root and the volume
  picker, all unchanged.

## Reason

**Why `everywhere:yes` and not a bare `everywhere`.** A bare word is a name substring,
which is what a bare word means in every search box anybody has used, and the rule that
an unknown `key:` is *also* a name is what keeps a file called `notes:2026.txt`
findable. Giving one bare word a second meaning would make `everywhere` the one word
that cannot be searched for, and it would do it silently. `hidden:yes` already
establishes the shape for a scope-ish yes/no, and completion makes it as discoverable
as the picker was.

**Why the volume stays a control and does not become `volume:`.** It could be a key,
and it may be one later. It is not one here because the round trip has to stay exact
and a volume is named by a label somebody typed — spaces, punctuation, two volumes with
the same label — so it needs its own validation and its own answer to "no such volume"
before it can be on the line. Meanwhile nothing is lost: absent from the line means the
picker is what sets it, and `setQueryLine()` leaves it alone.

**Why the root is shown rather than removed.** *Which folder* is the one part of a
search somebody has usually already decided by the time they get here — the tab is
opened from a folder. Showing it answers "where am I searching" without asking the
question again, and an editable field for it in **More** covers the case where the
answer is wrong. Removing it entirely would leave the scope invisible, which is worse
than the duplication being fixed.

**The alternatives the ticket listed, and why they lost.** Shrinking the picker to a
checkbox beside the line keeps scope out of the vocabulary and adds a control to the
view this is meant to empty. Leaving the picker as it is and only moving the name
fields keeps the two-item dropdown that reads as a second way of saying the folder.
Both are smaller changes that leave the thing that was complained about.

## Consequences

- **The line is now the only thing in the basic view that takes a query**, so an empty
  line and a used form is no longer the ordinary way to work — it stays possible, from
  **More**, and the guide says so rather than presenting the two as equals.
- **Editing the line sets the scope**, because absent means false the way an absent
  `ext:` means no extension filter. Anything else would make the round trip drift: pick
  *Everywhere indexed* in **More** and the rewrite puts `everywhere:yes` on the line, so
  the line and the fields still say the same thing.
- `docs/guide/searching.md` changes in *Typing the whole thing*: the line is where a
  search starts, and "the line can be left empty and the form used on its own" is no
  longer the neutral statement it was.
- The basic `GridLayout` goes away entirely, leaving one grid instead of two — which
  removes half of what MOLE-270 was about. The remaining grid's rows still have to add
  up, and `tst_SearchForm` is what holds that.
- **Three widgets are behind a panel that starts folded**, so anybody who used *Name
  contains* by habit has to open **More** once. That is the cost, and it is the point:
  the box that is left is the one that can express what those three could.
